/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "timer.h"
#include "macos_util.h"
#include <cmath>
#include <locale>
#include <sstream>
#ifndef __wasm32__
#include <thread>
#include <chrono>
#endif

bool wahe_epoch_configured = false;
static WaheMutex wall_time_mutex;
static double wall_time_unix;
static int wall_time_offset;
static uint64 wall_time_sample;

bool wahe_sync_wall_time()
{
    // Preserve an explicitly configured startup epoch without requiring a wall time provider
    if (wahe_epoch_configured) return true;

    // Parse the provider's paired UTC instant and local offset using protocol decimal punctuation
    char *response = wahe_request("Get wall time");
    std::istringstream input(response ? response : "");
    free(response);
    input.imbue(std::locale::classic());
    std::string unix_label, time_label, unix_unit, utc_label, offset_label, offset_unit;
    double seconds = 0;
    int offset = 0;
    bool valid = bool(input >> unix_label >> time_label >> seconds >> unix_unit >> utc_label >> offset_label >> offset >> offset_unit) &&
        unix_label == "Unix" && time_label == "time" && unix_unit == "seconds" &&
        utc_label == "UTC" && offset_label == "offset" && offset_unit == "seconds" &&
        (input >> std::ws).eof() && std::isfinite(seconds) && std::abs(seconds) <= 1e12 &&
        offset >= -86400 && offset <= 86400;
    if (!valid) {
        // Report a missing or malformed service instead of silently starting with an incorrect date
        wahe_fail("Host did not provide Get wall time with Unix time and UTC offset; update OS_Basics or configure SheepShaver clock unix");
        return false;
    }

    // Refresh the monotonic anchor after potentially lengthy guest initialization or host callbacks
    uint64 sampled_at = wahe_host_time();
    if (wahe_stopping.load()) return false;

    // Publish fractional wall time independently of the monotonic Time Manager clock
    std::lock_guard<WaheMutex> lock(wall_time_mutex);
    wahe_clock_usec.store(sampled_at);
    wall_time_unix = seconds;
    wall_time_offset = offset;
    wall_time_sample = sampled_at;
    return true;
}

void timer_init() { /* Host time is initialized before starting the worker */ }
uint64 GetTicks_usec() { /* Read the clock supplied by WAHE callbacks */ return wahe_clock_usec.load() - wahe_clock_origin; }
void Microseconds(uint32 &hi, uint32 &lo) { /* Split the host-driven microsecond counter */ uint64 t = GetTicks_usec(); hi = t >> 32; lo = t; }
uint32 TimerDateTime()
{
    // Retain the existing manual epoch's advancing clock with no automatic local offset
    if (wahe_epoch_configured) return uint32(wahe_epoch + GetTicks_usec() / 1000000 + 2082844800ULL);

    // Convert to local seconds since 1904 and wrap the classic Mac's unsigned 32-bit clock explicitly
    std::lock_guard<WaheMutex> lock(wall_time_mutex);
    double elapsed = double(wahe_clock_usec.load() - wall_time_sample) / 1000000.0;
    int64 seconds = (int64)std::floor(wall_time_unix + elapsed + wall_time_offset + 2082844800.0);
    return uint32(uint64(seconds) & 0xffffffffULL);
}
void timer_current_time(tm_time_t &time) { /* Match the Time Manager's 100-nanosecond units */ time = (tm_time_t)GetTicks_usec() * 10; }
void timer_add_time(tm_time_t &out, tm_time_t a, tm_time_t b) { /* Preserve Time Manager arithmetic */ out = a + b; }
void timer_sub_time(tm_time_t &out, tm_time_t a, tm_time_t b) { /* Preserve Time Manager arithmetic */ out = a - b; }
int timer_cmp_time(tm_time_t a, tm_time_t b) { /* Compare without subtracting potentially large deadlines */ return a < b ? -1 : a > b ? 1 : 0; }
void timer_mac2host_time(tm_time_t &out, int32 time) { /* Positive Mac delays are milliseconds and negative delays are microseconds */ out = time > 0 ? (int64)time * 10000 : -(int64)time * 10; }
int32 timer_host2mac_time(tm_time_t time)
{
    // Keep representable subsecond deadlines in microseconds
    if (time <= 0) return 0;
    uint64 us = time / 10;
    return us > 0x7fffffffULL ? (int32)(us / 1000) : -(int32)us;
}
void Delay_usec(uint32 us)
{
    // Yield cooperatively while waiting for host-driven time and honor module shutdown
    uint64 end = GetTicks_usec() + us;
    while (GetTicks_usec() < end) {
        wahe_check_stop();
#ifdef __wasm32__
        // Leave a pending delay when the CPU owner requests shutdown
        if (wahe_stop_requested()) return;
        // Yield through the host while the interpreter supplies updated timer ticks
        wahe_wasm_sleep(1000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
}
