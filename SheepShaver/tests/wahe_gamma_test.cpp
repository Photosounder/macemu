/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/video.cpp"
#include "vm_alloc.h"
#include <cassert>
#include <vector>

bool wahe_force_identity_gamma;
uint8 gKernelData[0x2000];
static int gamma_updates;

uint32 Mac_sysalloc(uint32 size)
{
    // Provide a small guest heap for the actual video driver's gamma-table allocation
    assert(size <= 4096);
    return 4096;
}

void Mac_sysfree(uint32) { /* The test guest heap is released with its VM reservation */ }
void video_set_gamma(int) { /* Record publication of the driver's effective correction table */ ++gamma_updates; }

static void check_identity(const VidLocals &saved)
{
    // Check the table exposed to indexed palette updates and guest GetGamma requests
    assert(ReadMacInt16(saved.gammaTable + gChanCnt) == 1);
    assert(ReadMacInt16(saved.gammaTable + gDataCnt) == 256);
    assert(ReadMacInt16(saved.gammaTable + gDataWidth) == 8);
    for (int i = 0; i < 256; ++i) {
        // Require both the guest-memory table and direct-colour cache to stay at identity
        assert(ReadMacInt8(saved.gammaTable + gFormulaData + i) == i);
        assert(mac_gamma[i].red == i && mac_gamma[i].green == i && mac_gamma[i].blue == i);
    }
}

static void test_resolution_status()
{
    // Query the real Mac display-driver interface using a mode table with all standard and custom IDs
    const struct { uint32 id; int width, height; } sizes[] = {
        {APPLE_640x480,640,480}, {APPLE_800x600,800,600}, {APPLE_1024x768,1024,768},
        {APPLE_CUSTOM,960,540}, {APPLE_832x624,832,624}
    };
    memset(VModes, 0, sizeof(VModes));
    for (int i = 0; i < 5; ++i) {
        VModes[i] = {DIS_WINDOW, uint32(sizes[i].width * 4), uint16(sizes[i].width),
            uint16(sizes[i].height), APPLE_32_BIT, sizes[i].id};
    }
    const uint32 pb = 16384, parameter = 16640, video_parameters = 16896;
    WriteMacInt32(pb + csParam, parameter);
    WriteMacInt16(pb + csCode, cscGetNextResolution);
    WriteMacInt32(parameter + csPreviousDisplayModeID, kDisplayModeIDFindFirstResolution);
    VidLocals saved{};
    for (const auto &size : sizes) {
        // Require correct enumeration dimensions and maximum depth including the newly added 832 mode
        assert(VideoStatus(pb, &saved) == noErr);
        assert(ReadMacInt32(parameter + csRIDisplayModeID) == size.id);
        assert(ReadMacInt32(parameter + csHorizontalPixels) == uint32(size.width));
        assert(ReadMacInt32(parameter + csVerticalLines) == uint32(size.height));
        assert(ReadMacInt16(parameter + csMaxDepthMode) == APPLE_32_BIT);
        WriteMacInt32(parameter + csPreviousDisplayModeID, size.id);
    }
    assert(VideoStatus(pb, &saved) == noErr);
    assert(ReadMacInt32(parameter + csRIDisplayModeID) == kDisplayModeIDNoMoreResolutions);

    // Provide safe Apple 16-inch timing and matching pixel geometry for the Monitors control panel
    WriteMacInt16(pb + csCode, cscGetModeTiming);
    WriteMacInt32(parameter + csTimingMode, APPLE_832x624);
    assert(VideoStatus(pb, &saved) == noErr);
    assert(ReadMacInt32(parameter + csTimingData) == timingApple_832x624_75hz);
    assert(ReadMacInt32(parameter + csTimingFlags) & (1 << kModeSafe));
    WriteMacInt16(pb + csCode, cscGetVideoParameters);
    WriteMacInt32(parameter + csDisplayModeID, APPLE_832x624);
    WriteMacInt16(parameter + csDepthMode, APPLE_32_BIT);
    WriteMacInt32(parameter + csVPBlockPtr, video_parameters);
    assert(VideoStatus(pb, &saved) == noErr);
    assert(ReadMacInt16(video_parameters + vpBounds + 4) == 624);
    assert(ReadMacInt16(video_parameters + vpBounds + 6) == 832);
    assert(ReadMacInt16(video_parameters + vpRowBytes) == 832 * 4);
}

int main()
{
    // Exercise the actual shared video driver without booting a guest or opening a window
    assert(vm_init() == 0);
    assert(vm_acquire_fixed(Mac2HostAddr(0), 65536) == 0);
    // Confirm the shared driver's Display Manager queries expose the expected selectable resolutions
    test_resolution_status();
    VidLocals saved{};
    assert(!wahe_force_identity_gamma && set_gamma(&saved, 0) == noErr);
    check_identity(saved);
    const uint32 source = 8192;

    // Submit single-channel and RGB tables through the same implementation used by guest SetGamma
    for (int channels : {1, 3}) {
        memset(Mac2HostAddr(source), 0, SIZEOF_GammaTbl + channels * 256);
        WriteMacInt16(source + gChanCnt, channels);
        WriteMacInt16(source + gDataCnt, 256);
        WriteMacInt16(source + gDataWidth, 8);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < 256; ++i) WriteMacInt8(source + gFormulaData + c * 256 + i, i / (c + 2));
        std::vector<uint8> original(Mac2HostAddr(source), Mac2HostAddr(source) + SIZEOF_GammaTbl + channels * 256);
        wahe_force_identity_gamma = false;
        assert(set_gamma(&saved, source) == noErr);
        // Preserve the submitted guest table and allow a nonidentity direct-colour cache in guest mode
        assert(memcmp(original.data(), Mac2HostAddr(saved.gammaTable), original.size()) == 0);
        assert(ReadMacInt8(saved.gammaTable + gFormulaData + 128) == 64);
        assert(mac_gamma[128].red != 128 && mac_gamma[128].green != 128 && mac_gamma[128].blue != 128);

        // Repeated guest calibration requests must not undo the configured identity override
        wahe_force_identity_gamma = true;
        for (int update = 0; update < 3; ++update) {
            assert(set_gamma(&saved, source) == noErr);
            check_identity(saved);
        }
        assert(memcmp(original.data(), Mac2HostAddr(source), original.size()) == 0);
    }

    // Publish every update and release the test's guest-memory reservation
    assert(gamma_updates == 9);
    vm_exit();
    puts("PASS: Mac resolution enumeration, 832x624 timing/geometry, guest gamma tables, identity override, repeated calibration writes, indexed palette table and direct-colour cache");
}
