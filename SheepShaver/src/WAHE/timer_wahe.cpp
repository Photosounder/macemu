/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "timer.h"
#include "macos_util.h"
#include <thread>
#include <chrono>

void timer_init() { /* Host time is initialized before starting the worker */ }
uint64 GetTicks_usec() { /* Read the clock supplied by WAHE callbacks */ return wahe_clock_usec.load() - wahe_clock_origin; }
void Microseconds(uint32 &hi, uint32 &lo) { /* Split the host-driven microsecond counter */ uint64 t = GetTicks_usec(); hi = t >> 32; lo = t; }
uint32 TimerDateTime() { /* Derive guest wall time from the host-supplied Unix epoch */ return uint32(wahe_epoch + GetTicks_usec() / 1000000 + 2082844800ULL); }
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
