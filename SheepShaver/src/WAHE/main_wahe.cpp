/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "main.h"
#include "cpu_emulation.h"
#include "prefs.h"
#include "rom_patches.h"
#include "timer.h"
#include "xlowmem.h"
#include "thunks.h"
#include "vm_alloc.h"
#include "sigsegv.h"
#include "xpram.h"
#ifndef __wasm32__
#include <condition_variable>
#include <thread>
#include <chrono>
#endif
#include <stdexcept>

uint32 RAMBase, RAMSize, ROMBase, KernelDataAddr, BootGlobsAddr, DRCacheAddr, PVR;
uint8 *RAMBaseHost, *ROMBaseHost;
uint8 gKernelData[0x2000];
int64 CPUClockSpeed, BusClockSpeed, TimebaseSpeed;
volatile uint32 InterruptFlags;
bool tick_inhibit;
uint32 SheepMem::page_size;
uintptr SheepMem::base = 0x51000000, SheepMem::zero_page, SheepMem::proc, SheepMem::data;

std::atomic<bool> wahe_stopping{false}, wahe_running{false}, wahe_paused{false};
std::atomic<uint64> wahe_clock_usec{0};
uint64 wahe_clock_origin, wahe_epoch;
int wahe_width = 800, wahe_height = 600, wahe_sample_rate = 44100;
std::string wahe_error;
WaheMutex wahe_error_mutex;
static WaheMutex interrupt_mutex, shutdown_mutex, cpu_lifetime_mutex;
#ifndef __wasm32__
static std::thread cpu_thread, tick_thread;
static WaheMutex idle_mutex;
static std::condition_variable idle_condition;
static std::atomic<DWORD> cpu_thread_id{0};
#else
static size_t cpu_thread_handle;
static thread_local bool on_cpu_runner;
static std::atomic<bool> runner_entered{false};
static int wake_generation;
#endif
static std::atomic<bool> ready{false};
static std::atomic<bool> started{false};
static uintptr signal_stack;
#ifndef __wasm32__
static PVOID exception_handler;
static HMODULE own_module;
static std::atomic<bool> memory_fault{false};
static uintptr fault_address, fault_instruction;
#endif

extern uintptr VMBaseDiff;
extern void init_emul_ppc();
extern void exit_emul_ppc();
extern void emul_ppc(uint32);

static void wake_cpu()
{
    // Keep the CPU alive until a concurrent host shutdown has finished signaling it
    std::lock_guard<WaheMutex> lock(cpu_lifetime_mutex);
    if (ready.load()) TriggerInterrupt();
}

void wahe_wake()
{
#ifdef __wasm32__
    // Wake the CPU runner using shared-memory scheduling primitives without an OS import
    __atomic_add_fetch(&wake_generation, 1, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify(&wake_generation, 1);
#else
    // Wake an idle CPU or paused worker without suspending a thread inside a critical section
    idle_condition.notify_all();
#endif
}

void wahe_fail(const std::string &message)
{
    // Preserve the first failure for the host's Status query and initiate cooperative shutdown
    bool first_failure;
    {
        // Release the error lock before calling a host that may reenter Status
        std::lock_guard<WaheMutex> lock(wahe_error_mutex);
        first_failure = wahe_error.empty();
        if (first_failure) wahe_error = message;
    }
    wahe_stopping.store(true);
    wahe_wake();
    // Surface the first asynchronous failure in the host console as well as Status
    if (first_failure) wahe_log("Error: " + message);
}

bool wahe_stop_requested()
{
    // Let each nested interpreter invocation return normally during shutdown
    return wahe_stopping.load();
}

void wahe_check_stop()
{
#ifdef __wasm32__
    // Return from stopped Wasm execution without unwinding the stack
    if (wahe_stopping.load()) return;
    // Advance guest interrupts from callback time while the interpreter owns the CPU state
    static uint64 next_poll, next_vbl;
    static unsigned ticks;
    if (on_cpu_runner) {
        uint64 now = wahe_host_time();
        wahe_clock_usec.store(now);
        if (ready.load() && !wahe_paused.load() && now >= next_poll) {
            next_poll = now + 1000;
            uint32 flags = INTFLAG_TIMER;
            if (now >= next_vbl && !tick_inhibit) {
                next_vbl = now + 16625;
                flags |= INTFLAG_VIA;
                if (++ticks >= 60) { ticks = 0; WriteMacInt32(0x20c, TimerDateTime()); }
            }
            if (wahe_audio_needed()) flags |= INTFLAG_AUDIO;
            SetInterruptFlag(flags);
            TriggerInterrupt();
        }
    }
    // Let a paused runner sleep through the host rather than blocking the embedding thread
    while (wahe_paused.load() && !wahe_stopping.load()) wahe_wasm_sleep(1000);
#else
    // Preserve native worker unwinding at its established safe points
    if (wahe_stopping.load()) throw WaheStop{};
    if (wahe_paused.load()) {
        std::unique_lock<WaheMutex> lock(idle_mutex);
        idle_condition.wait(lock, [] { return !wahe_paused.load() || wahe_stopping.load(); });
        if (wahe_stopping.load()) throw WaheStop{};
    }
#endif
}

#ifndef __wasm32__
static LONG CALLBACK guest_exception(EXCEPTION_POINTERS *exception)
{
    // Leave faults in the host and other modules to their own exception handlers
    if (GetCurrentThreadId() != cpu_thread_id.load() ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr address = exception->ExceptionRecord->ExceptionInformation[1];
    uintptr pc = exception->ContextRecord->Rip;
    MEMORY_BASIC_INFORMATION memory;
    if (!VirtualQuery((void *)pc, &memory, sizeof(memory)) || memory.AllocationBase != own_module ||
        address < VMBaseDiff || address - VMBaseDiff > 0xffffffffULL) return EXCEPTION_CONTINUE_SEARCH;

    // Let the existing decoder skip emulated ROM and hardware accesses using this thread's context
    sigsegv_info_t info{};
    info.addr = (char *)address;
    info.pc = (char *)pc;
    extern bool wahe_skip_fault(EXCEPTION_POINTERS *);
    bool expected = address >= (uintptr)ROMBaseHost && address - (uintptr)ROMBaseHost < ROM_AREA_SIZE;
    expected = expected || (address >= (uintptr)Mac2HostAddr(SheepMem::ZeroPage()) &&
        address - (uintptr)Mac2HostAddr(SheepMem::ZeroPage()) < SheepMem::PageSize());
    uint32 guest_address = (uint32)(address - VMBaseDiff);
    expected = expected || guest_address == 0xf3012000 || guest_address == 0xf3012002 || guest_address == 0xf8000000;
    if (!expected) {
        // Defer diagnostics and C++ unwinding until normal interpreter execution resumes
        fault_address = address;
        fault_instruction = pc;
        memory_fault.store(true);
        wahe_stopping.store(true);
    }
    if (!wahe_skip_fault(exception)) return EXCEPTION_CONTINUE_SEARCH;
    TriggerInterrupt();
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void tick_worker()
{
    // Feed host time and timer interrupts to the CPU without owning a presentation loop
    uint64 next_vbl = 0;
    int ticks = 0;
    try {
        while (!wahe_stopping.load()) {
            uint64 now = wahe_host_time();
            wahe_clock_usec.store(now);
            if (ready.load() && !wahe_paused.load()) {
                uint32 flags = INTFLAG_TIMER;
                if (now >= next_vbl && !tick_inhibit) {
                    next_vbl = now + 16625;
                    flags |= INTFLAG_VIA;
                    if (++ticks >= 60) {
                        ticks = 0;
                        WriteMacInt32(0x20c, TimerDateTime());
                    }
                }
                if (wahe_audio_needed()) flags |= INTFLAG_AUDIO;
                SetInterruptFlag(flags);
                TriggerInterrupt();
            }
            std::unique_lock<WaheMutex> lock(idle_mutex);
            idle_condition.wait_for(lock, std::chrono::milliseconds(1));
        }
    } catch (const std::exception &error) {
        // Propagate callback clock failures to the lifecycle owner
        wahe_fail(error.what());
        if (ready.load()) TriggerInterrupt();
    }
}

#endif

static void cpu_worker()
{
    // Keep initialization, Mac callbacks and CPU cleanup on the same native thread
#ifdef __wasm32__
    // Mark the host-owned runner so shutdown never tries to join itself
    on_cpu_runner = true;
#else
    cpu_thread_id.store(GetCurrentThreadId());
#endif
    bool preferences = false, vm = false, initialized = false, sheep_memory = false;
#ifndef __wasm32__
    try {
#endif
    // Return startup errors explicitly while retaining ownership of partially initialized resources
    auto run = [&]() -> const char * {
        int argc = 1;
        char name[] = "SheepShaver WAHE";
        char *args[] = {name, nullptr};
        char **argv = args;
        PrefsInit(nullptr, argc, argv);
        preferences = true;
        if (wahe_stopping.load()) return nullptr;
        RAMSize = PrefsFindInt32("ramsize");
        if (RAMSize < 16U * 1024 * 1024 || RAMSize > 512U * 1024 * 1024) return "Invalid RAM size";
        if (vm_init() < 0) return "Cannot reserve guest memory";
        vm = true;
        RAMBase = 0;
        ROMBase = 0x40800000;
#ifdef __wasm32__
        // Allocate compact backing stores before translating the fixed Macintosh addresses
        if (!wahe_wasm_map(RAMBase, RAMSize) || !wahe_wasm_map(ROMBase, ROM_AREA_SIZE))
            return "Cannot allocate guest RAM and ROM";
#endif
        RAMBaseHost = Mac2HostAddr(RAMBase);
        ROMBaseHost = Mac2HostAddr(ROMBase);
        if (vm_acquire_fixed(RAMBaseHost, RAMSize) < 0 || vm_acquire_fixed(ROMBaseHost, ROM_AREA_SIZE) < 0)
            return "Cannot commit guest RAM and ROM";
        memset(gKernelData, 0, sizeof(gKernelData));
        KernelDataAddr = KERNEL_DATA_BASE;
        PVR = 0x000c0000;
        CPUClockSpeed = 100000000;
        BusClockSpeed = 100000000;
        TimebaseSpeed = 25000000;
        if (!SheepMem::Init()) return "Cannot allocate SheepShaver globals";
        sheep_memory = true;

        // Read and decode the real ROM using only the host file provider
        WaheFile *rom = wahe_file_open(PrefsFindString("rom"), "rb");
        if (!rom) return "Cannot open ROM through host callback";
        uint64 bytes = wahe_file_size(rom);
        if (!bytes || bytes > 16 * 1024 * 1024) {
            wahe_file_close(rom);
            return "ROM size must be between 1 byte and 16 MiB";
        }
        std::vector<uint8> contents((size_t)bytes);
        size_t actual = wahe_file_read(rom, contents.data(), 0, contents.size());
        wahe_file_close(rom);
        if (actual != contents.size() || !DecodeROM(contents.data(), (uint32)actual)) return "Invalid or unreadable Macintosh ROM";
        wahe_check_stop();
        if (wahe_stopping.load()) return nullptr;
        timer_init();
        initialized = true;
        if (!InitAll(nullptr)) return "Macintosh initialization failed";
        if (vm_protect(ROMBaseHost, ROM_AREA_SIZE, VM_PAGE_READ) < 0) return "Cannot protect guest ROM";
        wahe_check_stop();
        if (wahe_stopping.load()) return nullptr;
        {
            // Publish the initialized CPU under the same lock used by host shutdown
            std::lock_guard<WaheMutex> lock(cpu_lifetime_mutex);
            init_emul_ppc();
            ready.store(true);
        }
#ifndef __wasm32__
        tick_thread = std::thread(tick_worker);
#endif
        wahe_check_stop();
        if (wahe_stopping.load()) return nullptr;
        emul_ppc(ROMBase + 0x310000);
        // Complete initialization and execution through the same cleanup owner
        return nullptr;
    };
    if (const char *error = run()) wahe_fail(error);
#ifndef __wasm32__
    } catch (const WaheStop &) {
        // A cooperative guest or host shutdown returns through this worker's cleanup path
    } catch (const std::exception &error) {
        // Retain startup and provider failures for the host instead of displaying a native dialog
        wahe_fail(error.what());
    } catch (...) {
        // Contain unexpected engine exceptions within the module
        wahe_fail("Unhandled emulation exception");
    }
#endif

    // Stop interrupt production before destroying any guest memory or CPU state
    wahe_stopping.store(true);
    wahe_wake();
#ifndef __wasm32__
    if (tick_thread.joinable()) tick_thread.join();
    if (memory_fault.load()) {
        char text[192];
        snprintf(text, sizeof(text), "Guest memory fault at host 0x%zx, instruction 0x%zx", (size_t)fault_address, (size_t)fault_instruction);
        wahe_fail(text);
    }
#endif
    {
        // Prevent a late interrupt callback from racing deletion of the CPU object
        std::lock_guard<WaheMutex> lock(cpu_lifetime_mutex);
        ready.store(false);
        exit_emul_ppc();
    }
    if (initialized) {
        // Release pending host-side timer descriptors before the guest reservation disappears
        TimerReset();
        ExitAll();
    }
    if (sheep_memory) SheepMem::Exit();
    if (vm) vm_exit();
    if (preferences) PrefsExit();
    RAMBaseHost = ROMBaseHost = nullptr;
#ifdef __wasm32__
    // Release the runner identity after all guest state has been destroyed
    on_cpu_runner = false;
#else
    cpu_thread_id.store(0);
#endif
    wahe_running.store(false);
}

void wahe_start()
{
    // Initialize one machine per loaded DLL without introducing a process entry point
    if (started.load() || wahe_stopping.load()) {
        // Reject repeated initialization before creating another CPU owner
#ifdef __wasm32__
        wahe_fail("This module was already initialized; reload it to start again");
        return;
#else
        throw std::runtime_error("This module was already initialized; reload it to start again");
#endif
    }
    wahe_clock_origin = wahe_host_time();
    wahe_clock_usec.store(wahe_clock_origin);
#ifdef __wasm32__
    // Abort startup when the host clock failed validation
    if (wahe_stopping.load()) return;
#endif
#ifdef __wasm32__
    // Ask WAHE to run the CPU on a private stack and TLS block without a Wasm process entry point
    started.store(true);
    wahe_running.store(true);
    char *response = wahe_request("Create thread with 1024 kB stack\nSheepShaver CPU runner");
    bool valid = response && sscanf(response, "Thread created %zu", &cpu_thread_handle) == 1 && cpu_thread_handle;
    free(response);
    if (!valid) {
        wahe_running.store(false);
        wahe_stopping.store(true);
        wahe_fail("Host could not create the SheepShaver CPU runner");
        return;
    }
#else
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)(uintptr)wahe_start, &own_module);
    exception_handler = AddVectoredExceptionHandler(1, guest_exception);
    if (!exception_handler) throw std::runtime_error("Cannot register guest exception handler");
    started.store(true);
    wahe_running.store(true);
    try { cpu_thread = std::thread(cpu_worker); }
    catch (...) {
        // Undo registration if creating the only owner of guest state fails
        wahe_running.store(false);
        wahe_stopping.store(true);
        RemoveVectoredExceptionHandler(exception_handler);
        exception_handler = nullptr;
        throw;
    }
#endif
}

void wahe_stop()
{
    // Join the CPU even after a guest-initiated shutdown before the host unloads the DLL
    wahe_stopping.store(true);
    wahe_wake();
#ifdef __wasm32__
    // Join the host-created runner before releasing any memory that its callbacks may reference
    if (on_cpu_runner) {
        // Leave joining to the host caller rather than waiting for this runner to exit itself
        wahe_fail("Cannot join the CPU from its own runner");
        return;
    }
    std::lock_guard<WaheMutex> lock(shutdown_mutex);
    if (cpu_thread_handle) {
        wake_cpu();
        char command[96];
        snprintf(command, sizeof(command), "Join thread ID %zu", cpu_thread_handle);
        char *response = wahe_request(command);
        size_t joined = 0;
        bool valid = response && sscanf(response, "Thread joined %zu", &joined) == 1 && joined == cpu_thread_handle;
        free(response);
        if (!valid) {
            // Retain runner ownership when the host could not confirm completion
            wahe_fail("Host could not join the SheepShaver CPU runner");
            return;
        }
        cpu_thread_handle = 0;
    }
#else
    if (GetCurrentThreadId() == cpu_thread_id.load()) throw std::runtime_error("Cannot join the CPU from a reentrant host callback");
    std::lock_guard<WaheMutex> lock(shutdown_mutex);
    if (cpu_thread.joinable()) {
        wake_cpu();
        cpu_thread.join();
    }
    if (exception_handler) RemoveVectoredExceptionHandler(exception_handler);
    exception_handler = nullptr;
#endif
}

#ifdef __wasm32__
void wahe_wasm_run()
{
    // Accept exactly one CPU invocation even if a host delivers a duplicate runner message
    bool expected = false;
    if (!wahe_running.load() || !runner_entered.compare_exchange_strong(expected, true)) return;
    cpu_worker();
}
void wahe_wasm_sleep(uint32 microseconds)
{
    // Yield only the CPU runner until input, shutdown or the next bounded timer poll
    int observed = __atomic_load_n(&wake_generation, __ATOMIC_ACQUIRE);
    __builtin_wasm_memory_atomic_wait32(&wake_generation, observed, uint64(microseconds) * 1000);
}
#endif

std::string wahe_status()
{
    // Return a stable lifecycle state and the first engine error
    std::lock_guard<WaheMutex> lock(wahe_error_mutex);
    if (!wahe_error.empty()) return "State: failed\nError: " + wahe_error;
    if (ready.load()) return "State: running";
    if (wahe_running.load()) return "State: starting";
    return started.load() || wahe_stopping.load() ? "State: stopped" : "State: configured";
}

void QuitEmulator()
{
    // Stop all nested Wasm execution loops by normal return to their caller
    wahe_stopping.store(true);
    wahe_wake();
#ifndef __wasm32__
    throw WaheStop{};
#endif
}
void ErrorAlert(const char *text)
{
    // Surface engine errors through Status and let the CPU owner perform cleanup
    wahe_fail(text);
#ifndef __wasm32__
    throw WaheStop{};
#endif
}
void WarningAlert(const char *text) { /* Delegate warnings to the host logger */ wahe_log(text); }
bool ChoiceAlert(const char *text, const char *, const char *) { /* Require the host to authorize choices explicitly */ wahe_log(text); return false; }
void PauseEmulator() { /* Pause at interpreter safe points */ wahe_paused.store(true); TriggerInterrupt(); }
void ResumeEmulator() { /* Resume the cooperatively paused CPU */ wahe_paused.store(false); wahe_wake(); }
uintptr SignalStackBase() { /* Expose the guest interrupt stack */ return signal_stack + 0x10000; }
void MakeExecutable(int, uint32 address, uint32 length) { /* Invalidate emulated code after guest modifications */ FlushCodeCache(address, address + length); }
void Dump68kRegs(M68kRegisters *r)
{
    // Deliver guest register diagnostics through the host logger
    for (int i = 0; i < 8; ++i) wahe_printf("d%d=%08x a%d=%08x\n", i, r->d[i], i, r->a[i]);
}
struct B2_mutex { WaheMutex mutex; };
B2_mutex *B2_create_mutex() { /* Allocate a module-owned lock */ return new B2_mutex; }
void B2_lock_mutex(B2_mutex *m) { /* Serialize shared emulation state */ m->mutex.lock(); }
void B2_unlock_mutex(B2_mutex *m) { /* Release the shared emulation lock */ m->mutex.unlock(); }
void B2_delete_mutex(B2_mutex *m) { /* Dispose locks after worker shutdown */ delete m; }
void SetInterruptFlag(uint32 flag) { /* Merge interrupt producers under a native lock */ std::lock_guard<WaheMutex> lock(interrupt_mutex); InterruptFlags |= flag; }
void ClearInterruptFlag(uint32 flag) { /* Clear acknowledged interrupt sources */ std::lock_guard<WaheMutex> lock(interrupt_mutex); InterruptFlags &= ~flag; }
void DisableInterrupt() { /* Preserve Mac interrupt nesting */ WriteMacInt32(XLM_IRQ_NEST, ReadMacInt32(XLM_IRQ_NEST) + 1); }
void EnableInterrupt() { /* Preserve Mac interrupt nesting */ WriteMacInt32(XLM_IRQ_NEST, ReadMacInt32(XLM_IRQ_NEST) - 1); }
#ifdef __wasm32__
void idle_wait()
{
    // Advance idle guest timers before returning scheduling control to the host
    wahe_check_stop();
    wahe_wasm_sleep(1000);
}
#else
void idle_wait() { /* Sleep only until an interrupt or shutdown notification */ std::unique_lock<WaheMutex> lock(idle_mutex); idle_condition.wait_for(lock, std::chrono::milliseconds(1)); }
#endif
void idle_resume() { /* Wake an idle CPU after input or a timer callback */ wahe_wake(); }

bool SheepMem::Init()
{
    // Commit the guest globals and interrupt stack inside this module's virtual memory
    page_size = vm_get_page_size();
    proc = base;
    data = base + size;
    zero_page = base + size / 2;
    signal_stack = base + size;
#ifdef __wasm32__
    // Back the fixed guest globals and interrupt stack with compact Wasm linear-memory allocations
    if (!wahe_wasm_map(base, size + 0x10000)) return false;
#endif
    if (vm_acquire_fixed(Mac2HostAddr(base), size + 0x10000) < 0) return false;
    return vm_protect(Mac2HostAddr(zero_page), page_size, VM_PAGE_READ) == 0;
}
void SheepMem::Exit() { /* Release committed globals before releasing the guest reservation */ vm_release(Mac2HostAddr(base), size + 0x10000); data = proc = zero_page = 0; }
