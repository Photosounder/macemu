/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHEEPSHAVER_WASM_SYSDEPS_H
#define SHEEPSHAVER_WASM_SYSDEPS_H
#include "../config.h"
#include "user_strings_windows.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cfenv>
#include <sys/types.h>

#define SHEEPSHAVER 1
#define POWERPC_ROM 1
#define EMULATED_PPC 1
#define DIRECT_ADDRESSING 1
#define USE_ETHER_FULL_DRIVER 1
#define ROM_IS_WRITE_PROTECTED 0
#define USE_SCRATCHMEM_SUBTERFUGE 1
#define PPC_REENTRANT_JIT 1
#define PPC_CHECK_INTERRUPTS 1
#define PPC_DECODE_CACHE 1
#define PPC_FLIGHT_RECORDER 1
#define PPC_PROFILE_COMPILE_TIME 0
#define PPC_PROFILE_GENERIC_CALLS 0
#define KPX_MAX_CPUS 1
#define UNALIGNED_PROFITABLE 1
#define ATTRIBUTE_PACKED __attribute__((__packed__))

using uint8 = uint8_t;
using int8 = int8_t;
using uint16 = uint16_t;
using int16 = int16_t;
using uint32 = uint32_t;
using int32 = int32_t;
using uint64 = uint64_t;
using int64 = int64_t;
using uintptr = uintptr_t;
using intptr = intptr_t;
using loff_t = int64_t;
using tm_time_t = int64_t;
#define VAL64(x) x##LL
#define UVAL64(x) x##ULL
#define bswap_16 __builtin_bswap16
#define bswap_32 __builtin_bswap32
#define bswap_64 __builtin_bswap64
#define do_byteswap_16 bswap_16
#define do_byteswap_32 bswap_32
#define tswap16 bswap_16
#define tswap32 bswap_32
#define tswap64 bswap_64
#define ntohl bswap_32
#define ntohs bswap_16
#define htonl bswap_32
#define htons bswap_16

using spinlock_t = int;
static constexpr spinlock_t SPIN_LOCK_UNLOCKED = 0;
#define HAVE_SPINLOCKS 1
inline void spin_lock(spinlock_t *lock)
{
    // Serialize shared CPU state with Wasm atomics rather than operating-system locks
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {}
}
inline void spin_unlock(spinlock_t *lock)
{
    // Publish modifications before another runner acquires the lock
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
inline int spin_trylock(spinlock_t *lock)
{
    // Attempt one atomic acquisition without blocking the calling runner
    return !__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE);
}
struct rgb_color { uint8 red, green, blue, alpha; };
extern void timer_init();
extern uint64 GetTicks_usec();
extern void Delay_usec(uint32);

#define CallMacOS(type, t) call_macos((uintptr)(t))
#define CallMacOS1(type, t, a) call_macos1((uintptr)(t), (uintptr)(a))
#define CallMacOS2(type, t, a, b) call_macos2((uintptr)(t), (uintptr)(a), (uintptr)(b))
#define CallMacOS3(type, t, a, b, c) call_macos3((uintptr)(t), (uintptr)(a), (uintptr)(b), (uintptr)(c))
#define CallMacOS4(type, t, a, b, c, d) call_macos4((uintptr)(t), (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d))
#define CallMacOS5(type, t, a, b, c, d, e) call_macos5((uintptr)(t), (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e))
#define CallMacOS6(type, t, a, b, c, d, e, f) call_macos6((uintptr)(t), (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), (uintptr)(f))
#define CallMacOS7(type, t, a, b, c, d, e, f, g) call_macos7((uintptr)(t), (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), (uintptr)(f), (uintptr)(g))
extern "C" {
uint32 call_macos(uint32);
uint32 call_macos1(uint32, uint32);
uint32 call_macos2(uint32, uint32, uint32);
uint32 call_macos3(uint32, uint32, uint32, uint32);
uint32 call_macos4(uint32, uint32, uint32, uint32, uint32);
uint32 call_macos5(uint32, uint32, uint32, uint32, uint32, uint32);
uint32 call_macos6(uint32, uint32, uint32, uint32, uint32, uint32, uint32);
uint32 call_macos7(uint32, uint32, uint32, uint32, uint32, uint32, uint32, uint32);
}
#endif
