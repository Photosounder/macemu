/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "video.h"
#include "video_defs.h"
#include "main.h"
#include "cpu_emulation.h"
#include "adb.h"
#include "prefs.h"
#include "vm_alloc.h"
#include <deque>
#include <cmath>

static WaheMutex frame_mutex, input_mutex;
static std::vector<float> presented;
static float monitor_to_linear[256];
static int presented_width, presented_height;
static bool frame_ready;
static const uint32 video_base = 0x52000000;
static const size_t video_capacity = 1920 * 1200 * 4;
struct Input { int kind, a, b; };
static std::deque<Input> input_queue;
static bool held_keys[128], held_buttons[3];

static int adb_code(int scan)
{
    // Translate SDL's USB scancodes without linking an SDL window or keyboard subsystem
    static const int letters[] = {0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    static const int digits[] = {18,19,20,21,23,22,26,28,25,29};
    if (scan >= 4 && scan <= 29) return letters[scan - 4];
    if (scan >= 30 && scan <= 39) return digits[scan - 30];
    switch (scan) {
        case 40: return 36; case 41: return 53; case 42: return 51; case 43: return 48;
        case 44: return 49; case 45: return 27; case 46: return 24; case 47: return 33;
        case 48: return 30; case 49: return 42; case 50: return 42; case 51: return 41;
        case 52: return 39; case 53: return 50; case 54: return 43; case 55: return 47;
        case 56: return 44; case 57: return 57;
        case 58: return 122; case 59: return 120; case 60: return 99; case 61: return 118;
        case 62: return 96; case 63: return 97; case 64: return 98; case 65: return 100;
        case 66: return 101; case 67: return 109; case 68: return 103; case 69: return 111;
        case 73: return 114; case 74: return 115; case 75: return 116; case 76: return 117;
        case 77: return 119; case 78: return 121;
        // Match the classic ADB arrow codes used by SheepShaver's desktop keyboard backend
        case 79: return 0x3c; case 80: return 0x3b;
        case 81: return 0x3d; case 82: return 0x3e;
        case 83: return 71; case 84: return 75; case 85: return 67; case 86: return 78;
        case 87: return 69; case 88: return 76; case 89: return 83; case 90: return 84;
        case 91: return 85; case 92: return 86; case 93: return 87; case 94: return 88;
        case 95: return 89; case 96: return 91; case 97: return 92; case 98: return 82;
        case 99: return 65; case 103: return 81;
        // Use the Control code read by the ADB modifier register without colliding with Left
        case 224: case 228: return 0x36; case 225: case 229: return 56;
        case 226: case 230: return 58; case 227: case 231: return 55;
        default: return -1;
    }
}

static void enqueue(Input event)
{
    // Bound host input and release held state if an overloaded host loses events
    std::lock_guard<WaheMutex> lock(input_mutex);
    if (input_queue.size() >= 4096) {
        input_queue.clear();
        input_queue.push_back({5, 0, 0});
    }
    input_queue.push_back(event);
    wahe_wake();
}

void wahe_input(const std::string &line, bool allow_relative)
{
    // Accept SDLwindow's existing text events without polling a private event queue
    double x, y;
    char action[24], button[24];
    int scan, count;
    if (sscanf(line.c_str(), "Mouse position (pixels) %lg %lg", &x, &y) == 2 &&
        std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 && x < 32768 && y < 32768)
        enqueue({0, (int)std::lround(x), (int)std::lround(y)});
    else if (sscanf(line.c_str(), "Mouse position (pixels) %lg %lg", &x, &y) == 2 &&
        (!std::isfinite(x) || !std::isfinite(y))) enqueue({5, 0, 0});
    else if (allow_relative && sscanf(line.c_str(), "Mouse delta %lg %lg", &x, &y) == 2 &&
        std::isfinite(x) && std::isfinite(y) && fabs(x) < 32768 && fabs(y) < 32768)
        enqueue({4, (int)std::lround(x), (int)std::lround(y)});
    else if (sscanf(line.c_str(), "Mouse %23s button %23s", button, action) == 2) {
        int number = !strcmp(button, "left") ? 0 : !strcmp(button, "right") ? 1 : !strcmp(button, "middle") ? 2 : -1;
        if (number >= 0 && (!strcmp(action, "down") || !strcmp(action, "up"))) enqueue({1, number, !strcmp(action, "down")});
    } else if (sscanf(line.c_str(), "Key %23[^:]: %d", action, &scan) == 2) {
        int code = adb_code(scan);
        if (code >= 0 && (!strcmp(action, "down") || !strcmp(action, "up"))) enqueue({2, code, !strcmp(action, "down")});
    } else if (sscanf(line.c_str(), "Mouse scroll %23s %d", action, &count) == 2 && count > 0 && count <= 64) {
        enqueue({3, !strcmp(action, "down") ? 121 : 116, count});
    } else if (line == "Window focus lost" || line == "Release input") enqueue({5, 0, 0});
}

void wahe_dispatch_input()
{
    // Apply queued ADB changes only on the CPU thread that owns the guest's input state
    std::deque<Input> events;
    {
        std::lock_guard<WaheMutex> lock(input_mutex);
        events.swap(input_queue);
    }
    for (const auto &event : events) {
        if (event.kind == 0 || event.kind == 4) {
            // Use relative motion only when the input message has no absolute framebuffer position
            ADBSetRelMouseMode(event.kind == 4);
            ADBMouseMoved(event.a, event.b);
        }
        else if (event.kind == 1) {
            held_buttons[event.a] = event.b;
            if (event.b) ADBMouseDown(event.a); else ADBMouseUp(event.a);
        } else if (event.kind == 2) {
            // Honor the existing Windows Option/Command preference on the CPU thread
            int code = event.a;
            if ((code == 55 || code == 58) && PrefsFindBool("swap_opt_cmd")) code = 113 - code;
            held_keys[code] = event.b;
            if (event.b) ADBKeyDown(code); else ADBKeyUp(code);
        } else if (event.kind == 3) {
            for (int i = 0; i < event.b; ++i) { ADBKeyDown(event.a); ADBKeyUp(event.a); }
        } else {
            for (int i = 0; i < 128; ++i) if (held_keys[i]) { ADBKeyUp(i); held_keys[i] = false; }
            for (int i = 0; i < 3; ++i) if (held_buttons[i]) { ADBMouseUp(i); held_buttons[i] = false; }
        }
    }
}

bool VideoInit()
{
    // Reserve enough guest memory to switch between resolutions without moving the framebuffer
#ifdef __wasm32__
    // Allocate the guest framebuffer before translating its fixed Macintosh address
    if (!wahe_wasm_map(video_base, video_capacity)) return false;
#endif
    if (vm_acquire_fixed(Mac2HostAddr(video_base), video_capacity) < 0) return false;
    memset(VModes, 0, sizeof(VideoInfo) * 64);
    // Offer the common Mac resolutions and the configured startup size at every colour depth
    const struct { int width, height; uint32 id; } resolutions[] = {
        {640, 480, APPLE_640x480}, {800, 600, APPLE_800x600},
        {832, 624, APPLE_832x624}, {1024, 768, APPLE_1024x768},
        {wahe_width, wahe_height, APPLE_CUSTOM}
    };
    int mode_count = 0;
    cur_mode = -1;
    for (const auto &resolution : resolutions) {
        // Reuse a standard resolution instead of listing the configured size twice
        if (resolution.id == APPLE_CUSTOM && cur_mode >= 0) continue;
        for (int depth = APPLE_1_BIT; depth <= APPLE_32_BIT; ++depth) {
            VideoInfo &mode = VModes[mode_count];
            mode.viType = DIS_WINDOW;
            mode.viXsize = resolution.width;
            mode.viYsize = resolution.height;
            mode.viAppleMode = depth;
            mode.viAppleID = resolution.id;
            mode.viRowBytes = (TrivialBytesPerRow(resolution.width, depth) + 15) & ~15U;
            if (resolution.width == wahe_width && resolution.height == wahe_height && depth == APPLE_32_BIT)
                cur_mode = mode_count;
            ++mode_count;
        }
    }
    display_type = DIS_WINDOW;
    screen_base = video_base;
    video_activated = true;
    ADBSetRelMouseMode(false);
    for (int i = 0; i < 256; ++i) {
        // Initialize palette and direct-color gamma tables before Mac OS installs its own tables
        mac_pal[i] = {(uint8)i, (uint8)i, (uint8)i, 255};
        mac_gamma[i] = mac_pal[i];
        // Decode the configured virtual monitor response once instead of exponentiating every pixel
        monitor_to_linear[i] = (float)std::pow(i / 255.0, wahe_monitor_gamma);
    }
    return true;
}

static void capture_frame()
{
    // Convert the guest-owned framebuffer on its CPU thread before publishing the snapshot
    const VideoInfo mode = VModes[cur_mode];
    std::lock_guard<WaheMutex> lock(frame_mutex);
    presented.resize((size_t)mode.viXsize * mode.viYsize * 4);
    const uint8 *screen = Mac2HostAddr(screen_base);
    for (int y = 0; y < mode.viYsize; ++y) for (int x = 0; x < mode.viXsize; ++x) {
        const uint8 *row = screen + (size_t)y * mode.viRowBytes;
        uint8 r, g, b;
        if (mode.viAppleMode == APPLE_32_BIT) { r = row[x * 4 + 1]; g = row[x * 4 + 2]; b = row[x * 4 + 3]; }
        else if (mode.viAppleMode == APPLE_16_BIT) {
            uint16 color = (row[x * 2] << 8) | row[x * 2 + 1];
            r = ((color >> 10) & 31) * 255 / 31; g = ((color >> 5) & 31) * 255 / 31; b = (color & 31) * 255 / 31;
        } else {
            int bits = 1 << (mode.viAppleMode - APPLE_1_BIT);
            int index = (row[x * bits / 8] >> (8 - bits - (x * bits % 8))) & ((1 << bits) - 1);
            r = mac_pal[index].red; g = mac_pal[index].green; b = mac_pal[index].blue;
        }
        if (IsDirectMode(mode.viAppleMode)) {
            // Indexed entries already include gamma correction from the common Mac video driver
            r = mac_gamma[r].red; g = mac_gamma[g].green; b = mac_gamma[b].blue;
        }
        size_t out = ((size_t)y * mode.viXsize + x) * 4;
        // Convert corrected monitor signals into linear light for the host's presentation pipeline
        presented[out] = monitor_to_linear[r];
        presented[out + 1] = monitor_to_linear[g];
        presented[out + 2] = monitor_to_linear[b];
        presented[out + 3] = 1.0f;
    }
    presented_width = mode.viXsize;
    presented_height = mode.viYsize;
    frame_ready = true;
}

bool wahe_frame(std::vector<float> &output, int &width, int &height)
{
    // Copy a completed CPU snapshot so callback buffers cannot race guest screen writes
    std::lock_guard<WaheMutex> lock(frame_mutex);
    if (!frame_ready) return false;
    output = presented;
    width = presented_width;
    height = presented_height;
    return true;
}

void VideoVBL()
{
    // Handle input and guest video interrupts before capturing a completed frame
    wahe_dispatch_input();
    if (private_data && private_data->interruptsEnabled) VSLDoInterruptService(private_data->vslServiceID);
    capture_frame();
}
void VideoExit() { /* Release presentation state after the CPU has stopped */ std::lock_guard<WaheMutex> lock(frame_mutex); std::vector<float>().swap(presented); frame_ready = false; video_activated = false; delete private_data; private_data = nullptr; }
void VideoQuitFullScreen() { /* The SDLwindow module owns fullscreen state */ }
void video_set_palette() { /* The next CPU snapshot uses the current Mac palette */ }
void video_set_gamma(int) { /* Gamma tables remain owned by the common video driver */ }
void video_set_cursor() { /* Mac OS draws its software cursor into the guest framebuffer */ }
bool video_can_change_cursor() { /* Keep cursor rendering on the guest side */ return false; }
void video_set_dirty_area(int, int, int, int) { /* CPU snapshots cover the complete framebuffer */ }
int16 video_mode_change(VidLocals *saved, uint32 parameter)
{
    // Switch resolution or depth at a CPU safe point while keeping the guest address stable
    for (int i = 0; VModes[i].viType != DIS_INVALID; ++i) if (VModes[i].viAppleMode == ReadMacInt16(parameter + csMode) &&
        VModes[i].viAppleID == ReadMacInt32(parameter + csData)) {
        cur_mode = i;
        saved->saveMode = VModes[i].viAppleMode;
        saved->saveData = VModes[i].viAppleID;
        saved->savePage = ReadMacInt16(parameter + csPage);
        saved->saveBaseAddr = screen_base;
        WriteMacInt32(parameter + csBaseAddr, screen_base);
        return noErr;
    }
    return paramErr;
}
