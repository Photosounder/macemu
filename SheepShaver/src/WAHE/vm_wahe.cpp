/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sysdeps.h"
#include "vm_alloc.h"

uintptr VMBaseDiff = 0;
static const size_t guest_extent = 0x60000000;

int vm_init()
{
    // Reserve one contiguous guest address space without committing unused guest pages
    VMBaseDiff = (uintptr)VirtualAlloc(nullptr, guest_extent, MEM_RESERVE, PAGE_NOACCESS);
    return VMBaseDiff ? 0 : -1;
}
void vm_exit()
{
    // Release the entire guest reservation after all CPU and timer workers have joined
    if (VMBaseDiff) VirtualFree((void *)VMBaseDiff, 0, MEM_RELEASE);
    VMBaseDiff = 0;
}
void *vm_acquire(size_t bytes, int)
{
    // Allocate native scratch storage without performing external I/O
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
int vm_acquire_fixed(void *address, size_t bytes, int)
{
    // Commit only ranges belonging to this module's reserved guest address space
    uintptr offset = (uintptr)address - VMBaseDiff;
    if (!VMBaseDiff || offset >= guest_extent || bytes > guest_extent - offset) return -1;
    return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) == address ? 0 : -1;
}
int vm_release(void *address, size_t bytes)
{
    // Decommit guest pages while retaining the reservation until vm_exit
    uintptr offset = (uintptr)address - VMBaseDiff;
    if (VMBaseDiff && offset < guest_extent) return VirtualFree(address, bytes, MEM_DECOMMIT) ? 0 : -1;
    return VirtualFree(address, 0, MEM_RELEASE) ? 0 : -1;
}
int vm_protect(void *address, size_t bytes, int protection)
{
    // Apply guest ROM and zero-page protection without registering a process-wide crash filter
    DWORD previous, flags = PAGE_NOACCESS;
    if (protection & VM_PAGE_WRITE) flags = protection & VM_PAGE_EXECUTE ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    else if (protection & VM_PAGE_READ) flags = protection & VM_PAGE_EXECUTE ? PAGE_EXECUTE_READ : PAGE_READONLY;
    return VirtualProtect(address, bytes, flags, &previous) ? 0 : -1;
}
int vm_get_page_size() { /* Query memory layout only */ SYSTEM_INFO info; GetSystemInfo(&info); return info.dwPageSize; }
int vm_get_write_watch(void *, size_t, void **, unsigned int *, int) { /* VOSF is not used by this backend */ return -1; }
int vm_reset_write_watch(void *, size_t) { /* VOSF is not used by this backend */ return -1; }
int vm_init_reserved(void *) { /* The backend allocates its framebuffer explicitly */ return 0; }
void *vm_acquire_reserved(size_t) { /* The backend allocates its framebuffer explicitly */ return nullptr; }
