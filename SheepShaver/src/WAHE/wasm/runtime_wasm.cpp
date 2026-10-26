/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include <wahe_imports.h>
#include <cstdarg>
#include <cerrno>
#include <limits>
#include <pthread.h>
#include <cmath>
#undef printf
#undef fprintf
#undef fflush
#undef fopen
#undef fclose
#undef fgets

extern "C" void __wasm_call_ctors();
extern "C" EXPORT("_initialize") void initialize_wasm()
{
    // Run C++ constructors once while leaving process startup under the WAHE host's control
    static std::atomic<bool> initialized{false};
    if (!initialized.exchange(true)) __wasm_call_ctors();
}
extern "C" void abort()
{
    // Report an unrecoverable runtime failure through the only host import before trapping this module
    wahe_run_command(const_cast<char *>("Print SheepShaver: freestanding runtime aborted"));
    __builtin_trap();
}
extern "C" void exit(int)
{
    // Keep the embedding host alive if a library attempts process termination
    abort();
}
extern "C" void __assert_fail(const char *expression, const char *file, int line, const char *)
{
    // Preserve assertion diagnostics without depending on a process console
    char text[1024];
    snprintf(text, sizeof(text), "Print SheepShaver assertion: %s at %s:%u", expression, file, line);
    wahe_run_command(text);
    abort();
}
extern "C" int __cxa_guard_acquire(uint64_t *guard)
{
    // Serialize function-local initialization across independently instantiated WAHE runners
    auto *state = reinterpret_cast<uint32_t *>(guard);
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(state, &expected, 256, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
        if (expected == 1) return 0;
    }
}
extern "C" void __cxa_guard_release(uint64_t *guard)
{
    // Publish completed local-static initialization to every runner
    __atomic_store_n(reinterpret_cast<uint32_t *>(guard), 1, __ATOMIC_RELEASE);
}
extern "C" char *getenv(const char *)
{
    // Keep environment configuration in WAHE messages without importing a process environment
    return nullptr;
}
extern "C" void _tzset_js(long *timezone, int *daylight, char *standard, char *summer)
{
    // Use UTC for C-library formatting while guest wall time comes from its explicit WAHE epoch
    *timezone = *daylight = 0;
    strcpy(standard, "UTC");
    strcpy(summer, "UTC");
}
extern "C" int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *)
{
    // Back runtime mutexes with shared atomic ownership instead of Emscripten's JavaScript thread runtime
    memset(mutex, 0, sizeof(*mutex));
    return 0;
}
extern "C" int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    // Allow library code to reenter synchronous host callbacks on its own TLS-identified runner
    auto *words = reinterpret_cast<uint32_t *>(mutex);
    uint32_t token = wahe_runner_token(), expected = 0;
    if (__atomic_load_n(words, __ATOMIC_RELAXED) == token) { ++words[1]; return 0; }
    while (!__atomic_compare_exchange_n(words, &expected, token, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) expected = 0;
    words[1] = 1;
    return 0;
}
extern "C" int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    // Release runtime ownership only after the outermost recursive operation completes
    auto *words = reinterpret_cast<uint32_t *>(mutex);
    if (--words[1] == 0) __atomic_store_n(words, 0, __ATOMIC_RELEASE);
    return 0;
}
extern "C" int pthread_mutex_destroy(pthread_mutex_t *) { /* Runtime mutexes own no external resources */ return 0; }
extern "C" int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    // Wait for library initialization using only the Wasm shared-memory scheduling instructions
    auto *generation = reinterpret_cast<int *>(condition);
    int observed = __atomic_load_n(generation, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(mutex);
    __builtin_wasm_memory_atomic_wait32(generation, observed, -1);
    return pthread_mutex_lock(mutex);
}
extern "C" int pthread_cond_broadcast(pthread_cond_t *condition)
{
    // Publish initialization and wake all runners sharing the runtime condition
    auto *generation = reinterpret_cast<int *>(condition);
    __atomic_add_fetch(generation, 1, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify(generation, INT_MAX);
    return 0;
}
extern "C" int pthread_cond_destroy(pthread_cond_t *) { /* Runtime conditions own no external resources */ return 0; }
extern "C" int emscripten_futex_wait(volatile void *address, uint32_t value, double milliseconds)
{
    // Implement library contention waits directly without Emscripten's JavaScript worker machinery
    int64_t timeout = std::isfinite(milliseconds) ? int64_t(std::max(0.0, milliseconds) * 1000000.0) : -1;
    int result = __builtin_wasm_memory_atomic_wait32(static_cast<int *>(const_cast<void *>(address)), value, timeout);
    return result == 0 ? 0 : result == 1 ? -EAGAIN : -ETIMEDOUT;
}
extern "C" int emscripten_futex_wake(volatile void *address, int count)
{
    // Wake runtime waiters sharing the requested atomic word
    return __builtin_wasm_memory_atomic_notify(static_cast<int *>(const_cast<void *>(address)), count);
}

extern "C" int fflush(FILE *) { /* File-provider writes complete synchronously */ return 0; }
extern "C" FILE *fopen(const char *path, const char *mode) { /* Keep runtime file opens callback-backed */ return wahe_fopen(path, mode); }
extern "C" int fclose(FILE *stream) { /* Close runtime streams through their provider */ return wahe_fclose(stream); }
extern "C" char *fgets(char *text, int count, FILE *stream) { /* Read runtime lines through their provider */ return wahe_fgets(text, count, stream); }
