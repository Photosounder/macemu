/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "timer.h"
#include <cassert>
#include <cstring>
#include <locale>

std::atomic<bool> wahe_stopping{false};
std::atomic<uint64> wahe_clock_usec{5000000};
uint64 wahe_clock_origin = 5000000, wahe_epoch;
static const char *host_reply;
static std::string failure;
static unsigned queries;

char *wahe_request(const std::string &command)
{
    // Model ownership of a provider reply while counting automatic wall time requests
    assert(command == "Get wall time");
    ++queries;
    return host_reply ? strdup(host_reply) : nullptr;
}

uint64 wahe_host_time()
{
    // Keep the monotonic clock deterministic across independent calendar adjustments
    return wahe_clock_usec.load();
}

void wahe_fail(const std::string &message)
{
    // Record clock service failures without starting an emulator or displaying a dialog
    failure = message;
    wahe_stopping.store(true);
}

void wahe_check_stop()
{
    // Leave delay handling available to the linked timer backend without running a worker
}

struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override
    {
        // Simulate a host application whose C++ locale uses comma decimals
        return ',';
    }
};

int main()
{
    // Retain fractional seconds until the final unsigned Macintosh calendar conversion
    host_reply = "Unix time 1793263567.750000000 seconds\nUTC offset +3600 seconds\n";
    assert(wahe_sync_wall_time());
    assert(TimerDateTime() == uint32(1793263567ULL + 3600 + 2082844800ULL));
    wahe_clock_usec.fetch_add(500000);
    assert(TimerDateTime() == uint32(1793263568ULL + 3600 + 2082844800ULL));
    assert(GetTicks_usec() == 500000);

    // Apply a spring transition using the offset paired with each sampled UTC instant
    host_reply = "Unix time 1774745999.9 seconds\nUTC offset +3600 seconds";
    assert(wahe_sync_wall_time());
    uint32 before = TimerDateTime();
    wahe_clock_usec.fetch_add(1000000);
    host_reply = "Unix time 1774746000.9 seconds\nUTC offset +7200 seconds";
    assert(wahe_sync_wall_time());
    assert(TimerDateTime() == before + 3601);
    assert(GetTicks_usec() == 1500000);

    // Follow the autumn clock rollback while elapsed timers continue advancing normally
    host_reply = "Unix time 1792889999.9 seconds\nUTC offset +7200 seconds";
    assert(wahe_sync_wall_time());
    before = TimerDateTime();
    wahe_clock_usec.fetch_add(1000000);
    host_reply = "Unix time 1792890000.9 seconds\nUTC offset +3600 seconds";
    assert(wahe_sync_wall_time());
    assert(TimerDateTime() == before - 3599);
    assert(GetTicks_usec() == 2500000);

    // Accept a fractional timezone offset and a host clock correction independent of elapsed time
    host_reply = "Unix time 1700000000.25 seconds\r\nUTC offset -12600 seconds\r\n";
    assert(wahe_sync_wall_time());
    assert(TimerDateTime() == uint32(1700000000ULL - 12600 + 2082844800ULL));
    assert(GetTicks_usec() == 2500000);

    // Parse protocol dot decimals even when the host changes the global C++ locale
    std::locale previous = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    assert(wahe_sync_wall_time());
    assert(TimerDateTime() == uint32(1700000000ULL - 12600 + 2082844800ULL));
    std::locale::global(previous);

    // Wrap the guest's calendar in 2040 without overflowing a floating point to unsigned conversion
    host_reply = "Unix time 2212122495.75 seconds\nUTC offset +0 seconds";
    assert(wahe_sync_wall_time());
    assert(TimerDateTime() == 0xffffffffU);
    wahe_clock_usec.fetch_add(500000);
    assert(TimerDateTime() == 0);

    // Floor negative fractional Unix timestamps only after converting to the Macintosh epoch
    host_reply = "Unix time -0.25 seconds\nUTC offset +0 seconds";
    assert(wahe_sync_wall_time());
    assert(TimerDateTime() == 2082844799U);

    // Reject missing, partial, nonfinite, out of range and trailing malformed provider data
    const char *invalid[] = {nullptr, "", "Unix time 1700000000 seconds", "Unix time nan seconds\nUTC offset +0 seconds",
        "Unix time inf seconds\nUTC offset +0 seconds", "Unix time 1e20 seconds\nUTC offset +0 seconds",
        "Unix time 1,5 seconds\nUTC offset +0 seconds", "Unix time 1 seconds\nUTC offset +86401 seconds",
        "Unix time 1 seconds\nUTC offset -86401 seconds", "Unix time 1 seconds\nUTC offset +3600.5 seconds",
        "Unix time 1 seconds\nUTC offset +999999999999 seconds", "Unix time 1 milliseconds\nUTC offset +0 seconds",
        "Unix time 1 seconds\nUTC offset +0 seconds garbage"};
    for (const char *reply : invalid) {
        // Require validation failures to initiate the same cooperative shutdown as other provider errors
        host_reply = reply;
        wahe_stopping.store(false);
        failure.clear();
        assert(!wahe_sync_wall_time());
        assert(wahe_stopping.load());
        assert(failure.find("Get wall time") != std::string::npos);
    }

    // Preserve explicitly configured epochs even when the wall time service is unavailable
    wahe_stopping.store(false);
    wahe_epoch_configured = true;
    wahe_epoch = 1700000000;
    host_reply = nullptr;
    unsigned previous_queries = queries;
    assert(wahe_sync_wall_time());
    assert(queries == previous_queries);
    assert(TimerDateTime() == uint32(1700000000ULL + GetTicks_usec() / 1000000 + 2082844800ULL));
    wahe_epoch = 0;
    assert(TimerDateTime() == uint32(GetTicks_usec() / 1000000 + 2082844800ULL));
    puts("PASS: fractional wall time, local offsets, DST transitions, monotonic timers, validation and manual override");
}
