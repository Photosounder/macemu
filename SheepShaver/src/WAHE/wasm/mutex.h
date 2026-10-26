/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHEEPSHAVER_WASM_MUTEX_H
#define SHEEPSHAVER_WASM_MUTEX_H
#include <atomic>
#include <cstdint>

inline uintptr_t wahe_runner_token()
{
    // Identify each WAHE runner by its host-initialized private TLS block
    static thread_local uint8_t marker;
    return reinterpret_cast<uintptr_t>(&marker);
}
class WaheRecursiveMutex {
    std::atomic<uintptr_t> owner{0};
    unsigned depth = 0;
public:
    void lock()
    {
        // Allow synchronous host callbacks to reenter on the same runner
        const uintptr_t token = wahe_runner_token();
        if (owner.load(std::memory_order_relaxed) == token) { ++depth; return; }
        uintptr_t expected = 0;
        while (!owner.compare_exchange_weak(expected, token, std::memory_order_acquire)) expected = 0;
        depth = 1;
    }
    void unlock()
    {
        // Publish shared state only when the outermost callback releases the lock
        if (--depth == 0) owner.store(0, std::memory_order_release);
    }
};
using WaheMutex = WaheRecursiveMutex;
#endif
