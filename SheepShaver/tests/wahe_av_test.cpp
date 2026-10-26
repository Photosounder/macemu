/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "cpu_emulation.h"
#include "main.h"
#include "video.h"
#include "video_defs.h"
#include "audio.h"
#include "audio_defs.h"
#include "vm_alloc.h"
#include <cassert>
#include <tuple>
#include <cmath>
#include <set>

uint8 gKernelData[0x2000];
int wahe_width = 17, wahe_height = 2, wahe_sample_rate = 44100;
double wahe_monitor_gamma = 1.8;
VideoInfo VModes[64];
uint32 screen_base;
int cur_mode, display_type;
bool video_activated, audio_open;
VidLocals *private_data;
rgb_color mac_pal[256], mac_gamma[256];
audio_status AudioStatus;
int audio_frames_per_block;
uint32 audio_component_flags, audio_data = 4096;
std::vector<uint32> audio_sample_rates;
std::vector<uint16> audio_sample_sizes;
std::vector<uint8> audio_channel_counts;
static std::vector<std::tuple<int, int, int>> events;
static int mixer_calls;
static bool relative_mouse;
bool PrefsFindBool(const char *) { /* Keep native test defaults deterministic */ return false; }
void wahe_wake() { /* Tests explicitly drain the input queue */ }
int16 VSLDoInterruptService(uint32) { /* The unit fixture has no guest VSL service */ return 0; }
void ADBSetRelMouseMode(bool relative) { /* Record absolute and grabbed-pointer mode transitions */ relative_mouse = relative; }
void ADBMouseMoved(int x, int y) { /* Record the CPU-delivered mouse position */ events.emplace_back(0, x, y); }
void ADBMouseDown(int button) { /* Record button presses */ events.emplace_back(1, button, 1); }
void ADBMouseUp(int button) { /* Record button releases */ events.emplace_back(1, button, 0); }
void ADBKeyDown(int key) { /* Record USB-to-ADB key presses */ events.emplace_back(2, key, 1); }
void ADBKeyUp(int key) { /* Record key releases */ events.emplace_back(2, key, 0); }
void Execute68k(uint32 pc, M68kRegisters *r)
{
    // Verify the actual audio backend requests the common guest mixer using the expected registers
    assert(pc == audio_data + adatGetSourceData);
    assert(r->a[0] == audio_data + adatStreamInfo && r->a[1] == AudioStatus.mixer);
    WriteMacInt32(audio_data + adatStreamInfo, 8192);
    ++mixer_calls;
}

static void test_resolutions()
{
    // Check that every configured startup size coexists with the four standard resolutions
    const int startup_sizes[][2] = {{640,480}, {800,600}, {832,624}, {1024,768}, {960,540}};
    for (const auto &size : startup_sizes) {
        wahe_width = size[0]; wahe_height = size[1];
        assert(VideoInit());
        assert(VModes[cur_mode].viXsize == size[0] && VModes[cur_mode].viYsize == size[1]);
        assert(VModes[cur_mode].viAppleMode == APPLE_32_BIT);
        std::set<std::tuple<int, int, int>> modes;
        int count = 0;
        for (; count < 64 && VModes[count].viType != DIS_INVALID; ++count) {
            const VideoInfo &mode = VModes[count];
            assert(modes.emplace(mode.viXsize, mode.viYsize, mode.viAppleMode).second);
        }
        assert(count == (size[0] == 960 ? 30 : 24));
        for (const auto &standard : startup_sizes) {
            // Require every depth for the common sizes and the configured custom size without duplicates
            if (standard[0] == 960 && size[0] != 960) continue;
            for (int depth = APPLE_1_BIT; depth <= APPLE_32_BIT; ++depth)
                assert(modes.count(std::make_tuple(standard[0], standard[1], depth)) == 1);
        }
        VideoExit();
    }

    // Switch through every registered resolution and depth and capture the last pixel of each scan layout
    wahe_width = 960; wahe_height = 540;
    assert(VideoInit());
    uint32 base = screen_base;
    for (int i = 0; VModes[i].viType != DIS_INVALID; ++i) {
        VideoInfo mode = VModes[i];
        WriteMacInt16(256 + csMode, mode.viAppleMode);
        WriteMacInt32(256 + csData, mode.viAppleID);
        VidLocals saved{};
        assert(video_mode_change(&saved, 256) == noErr && cur_mode == i);
        assert(saved.saveData == mode.viAppleID && saved.saveMode == mode.viAppleMode);
        assert(screen_base == base && saved.saveBaseAddr == base && ReadMacInt32(256 + csBaseAddr) == base);
        uint8 *pixels = Mac2HostAddr(base);
        memset(pixels, 0, mode.viRowBytes * mode.viYsize);
        uint8 *row = pixels + (mode.viYsize - 1) * mode.viRowBytes;
        int x = mode.viXsize - 1;
        mac_pal[0] = {0,0,0,255}; mac_pal[1] = {255,0,0,255};
        if (mode.viAppleMode == APPLE_32_BIT) row[x * 4 + 1] = 255;
        else if (mode.viAppleMode == APPLE_16_BIT) row[x * 2] = 0x7c;
        else {
            // Address packed indexed pixels using the selected mode's row stride and depth
            int bits = 1 << (mode.viAppleMode - APPLE_1_BIT);
            row[x * bits / 8] |= 1 << (8 - bits - (x * bits % 8));
        }
        VideoVBL();
        std::vector<float> frame;
        int width, height;
        assert(wahe_frame(frame, width, height));
        assert(width == mode.viXsize && height == mode.viYsize && frame.size() == size_t(width) * height * 4);
        assert(frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 1);
        size_t last = frame.size() - 4;
        assert(frame[last] == 1 && frame[last + 1] == 0 && frame[last + 2] == 0 && frame[last + 3] == 1);
    }

    // Reject an unknown resolution without altering the active framebuffer mode
    int previous_mode = cur_mode;
    WriteMacInt32(256 + csData, 0xffffffff);
    VidLocals saved{};
    assert(video_mode_change(&saved, 256) == paramErr && cur_mode == previous_mode);
    VideoExit();
    wahe_width = 17; wahe_height = 2;
}

static void test_monitor_gamma(double gamma)
{
    // Exercise all framebuffer formats with padded scanlines in actual guest-mapped memory
    wahe_monitor_gamma = gamma;
    assert(VideoInit());
    std::vector<float> frame;
    int width, height;
    for (int mode = APPLE_1_BIT; mode <= APPLE_32_BIT; ++mode) {
        WriteMacInt16(256 + csMode, mode);
        WriteMacInt32(256 + csData, APPLE_CUSTOM);
        VidLocals saved{};
        assert(video_mode_change(&saved, 256) == noErr && saved.saveBaseAddr == screen_base);
        uint8 *pixels = Mac2HostAddr(screen_base);
        memset(pixels, 0, VModes[cur_mode].viRowBytes * wahe_height);
        // Restore the identity guest correction before checking black and full-intensity endpoints
        for (int i = 0; i < 256; ++i) mac_gamma[i] = {(uint8)i, (uint8)i, (uint8)i, 255};
        mac_pal[0] = {255, 0, 0, 255};
        mac_pal[1] = {0, 255, 0, 255};
        if (mode == APPLE_32_BIT) { pixels[1] = 255; pixels[6] = 255; }
        else if (mode == APPLE_16_BIT) { pixels[0] = 0x7c; pixels[2] = 0x03; pixels[3] = 0xe0; }
        else {
            int bits = 1 << (mode - APPLE_1_BIT);
            if (bits == 8) pixels[1] = 1;
            else pixels[0] = 1 << (8 - 2 * bits);
        }
        VideoVBL();
        assert(wahe_frame(frame, width, height) && width == 17 && height == 2);
        assert(frame.size() == 17 * 2 * 4);
        assert(frame[0] == 1 && frame[1] == 0 && frame[2] == 0 && frame[3] == 1);
        assert(frame[4] == 0 && frame[5] == 1 && frame[6] == 0 && frame[7] == 1);

        // Require guest correction before monitor decoding and avoid correcting indexed palettes twice
        mac_pal[0] = {128, 64, 192, 255};
        mac_gamma[128].red = 7;
        mac_gamma[64].green = 8;
        mac_gamma[192].blue = 9;
        if (mode == APPLE_32_BIT) {
            // Supply different source components so each guest correction channel must be used
            pixels[1] = 33; pixels[2] = 66; pixels[3] = 99;
            mac_gamma[33].red = 128; mac_gamma[66].green = 64; mac_gamma[99].blue = 192;
        } else if (mode == APPLE_16_BIT) {
            // Account for RGB555 expansion before indexing the guest correction tables
            WriteMacInt16(screen_base, (16 << 10) | (8 << 5) | 24);
            mac_gamma[16 * 255 / 31].red = 128;
            mac_gamma[8 * 255 / 31].green = 64;
            mac_gamma[24 * 255 / 31].blue = 192;
        }
        VideoVBL();
        assert(wahe_frame(frame, width, height));
        assert(std::fabs(frame[0] - std::pow(128 / 255.0, gamma)) < 1e-6);
        assert(std::fabs(frame[1] - std::pow(64 / 255.0, gamma)) < 1e-6);
        assert(std::fabs(frame[2] - std::pow(192 / 255.0, gamma)) < 1e-6);
        for (size_t p = 0; p < frame.size(); ++p) {
            // Validate every published component and keep alpha as linear opaque coverage
            assert(std::isfinite(frame[p]) && frame[p] >= 0 && frame[p] <= 1);
            if (p % 4 == 3) assert(frame[p] == 1);
        }
    }
    VideoExit();
}

int main()
{
    // Check the classic Mac default and the optional 2.2 virtual monitor without opening a display
    assert(vm_init() == 0);
    assert(vm_acquire_fixed(Mac2HostAddr(0), 65536) == 0);
    // Exercise runtime resolution changes before the small padded-row and gamma fixtures
    test_resolutions();
    assert(wahe_monitor_gamma == 1.8);
    test_monitor_gamma(1.8);
    test_monitor_gamma(2.2);
    wahe_monitor_gamma = 1.8;
    assert(VideoInit());

    // Ensure input is queued, mapped to ADB and released when SDLwindow reports an unavailable pointer
    events.clear();
    wahe_input("Mouse position (pixels) 12 9");
    wahe_input("Key down: 4 / \"A\" / \"A\"");
    wahe_input("Key repeat: 4");
    wahe_input("Mouse left button down");
    assert(events.empty());
    wahe_dispatch_input();
    assert(events.size() == 3 && events[0] == std::make_tuple(0, 12, 9));
    assert(events[1] == std::make_tuple(2, 0, 1));
    wahe_input("Mouse position (pixels) NAN NAN");
    wahe_dispatch_input();
    assert(events[3] == std::make_tuple(2, 0, 0) && events[4] == std::make_tuple(1, 0, 0));

    // Check each SDL arrow and Control scancode against the classic ADB keyboard protocol
    const int navigation_keys[][2] = {{79, 0x3c}, {80, 0x3b}, {81, 0x3d}, {82, 0x3e}, {224, 0x36}, {228, 0x36}};
    for (const auto &key : navigation_keys) {
        events.clear();
        wahe_input("Key down: " + std::to_string(key[0]));
        wahe_input("Key repeat: " + std::to_string(key[0]));
        wahe_input("Key up: " + std::to_string(key[0]));
        wahe_dispatch_input();
        assert(events.size() == 2);
        assert(events[0] == std::make_tuple(2, key[1], 1));
        assert(events[1] == std::make_tuple(2, key[1], 0));
    }

    // Keep Control and Left independent and release both when the host window loses focus
    events.clear();
    wahe_input("Key down: 224 / \"Left Ctrl\" / \"Left Ctrl\"");
    wahe_input("Key down: 80 / \"Left\" / \"Left\"");
    wahe_input("Window focus lost");
    wahe_dispatch_input();
    assert(events.size() == 4);
    assert(events[0] == std::make_tuple(2, 0x36, 1) && events[1] == std::make_tuple(2, 0x3b, 1));
    assert(events[2] == std::make_tuple(2, 0x36, 0) && events[3] == std::make_tuple(2, 0x3b, 0));

    // Handle captured motion without applying both absolute and relative events from one message
    events.clear();
    wahe_input("Mouse delta -4 7");
    wahe_dispatch_input();
    assert(relative_mouse && events[0] == std::make_tuple(0, -4, 7));
    wahe_input("Mouse position (pixels) 3 8");
    wahe_input("Mouse delta -4 7", false);
    wahe_dispatch_input();
    assert(!relative_mouse && events.size() == 2 && events[1] == std::make_tuple(0, 3, 8));

    // Check exact guest audio conversion instead of merely accepting silent callback buffers
    AudioInit();
    AudioStatus.mixer = 1234;
    AudioStatus.num_sources = 1;
    WriteMacInt32(8192 + scd_sampleCount, 2);
    WriteMacInt16(8192 + scd_numChannels, 2);
    WriteMacInt16(8192 + scd_sampleSize, 16);
    WriteMacInt32(8192 + scd_buffer, 12288);
    WriteMacInt16(12288, 0x8000);
    WriteMacInt16(12290, 0x4000);
    WriteMacInt16(12292, 0x2000);
    WriteMacInt16(12294, 0xc000);
    AudioInterrupt();
    float sound[8];
    wahe_audio_pull(sound, 4);
    assert(sound[0] == -1.f && sound[1] == .5f && sound[2] == .25f && sound[3] == -.5f);
    assert(sound[4] == 0 && sound[7] == 0 && mixer_calls == 1 && wahe_audio_needed());
    WriteMacInt16(8192 + scd_numChannels, 1);
    WriteMacInt16(8192 + scd_sampleSize, 8);
    WriteMacInt8(12288, 0);
    WriteMacInt8(12289, 192);
    AudioInterrupt();
    wahe_audio_pull(sound, 2);
    assert(sound[0] == -1.f && sound[1] == -1.f && sound[2] == .5f && sound[3] == .5f);
    audio_set_main_mute(true);
    AudioInterrupt();
    wahe_audio_pull(sound, 2);
    for (float sample : {sound[0], sound[1], sound[2], sound[3]}) assert(sample == 0);
    AudioExit();
    VideoExit();
    vm_exit();
    puts("PASS: standard/custom resolution switches, deduplication, six framebuffer depths, guest gamma, 1.8/2.2 monitor decoding, linear float RGBA, SDLwindow-to-ADB input including arrows and Control, focus release, stereo/mono audio, mute and underrun");
}
