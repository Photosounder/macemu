/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "cpu_emulation.h"
#include "vm_alloc.h"

uintptr VMBaseDiff;
struct Region { uint32 address; size_t size; uint8 *data; };
struct Protection { uint32 address; size_t size; };
static Region regions[16];
static Protection protections[16];
static unsigned region_count, protection_count;
static uint8 *pages[65536];
static uint32 page_end[65536];
static const uint8 zero[16] = {};
static thread_local uint8 discard[16];
extern uint8 gKernelData[0x2000];

bool wahe_wasm_map(uint32 address, size_t size)
{
    // Keep each fixed guest region contiguous without allocating its unused address-space gaps
    if (!size || (address & 65535) || uint64(address) + size > 0x100000000ULL || region_count == 16) return false;
    for (unsigned i = 0; i < region_count; ++i) {
        if (address == regions[i].address && size == regions[i].size) return true;
        if (uint64(address) < uint64(regions[i].address) + regions[i].size && uint64(regions[i].address) < uint64(address) + size) return false;
    }
    uint8 *data = static_cast<uint8 *>(calloc(size, 1));
    if (!data) return false;
    regions[region_count++] = {address, size, data};
    // Publish constant-time page translations after allocating the complete region
    for (size_t offset = 0; offset < size; offset += 65536) {
        unsigned page = (address + offset) >> 16;
        pages[page] = data + offset;
        page_end[page] = size - offset;
    }
    return true;
}

uint8 *wahe_wasm_address(uint32 address, size_t size, bool write)
{
    // Preserve the two Macintosh aliases for SheepShaver's small kernel data area
    if (((address & ~0x1fffU) == KERNEL_DATA_BASE || (address & ~0x1fffU) == KERNEL_DATA2_BASE) && size <= 0x2000 - (address & 0x1fff))
        return gKernelData + (address & 0x1fff);
    // Emulate the hardware accesses that the native fault handler intentionally ignores
    if ((address == 0xf3012000 || address == 0xf3012002 || address == 0xf8000000) && size <= sizeof(discard))
        return write ? discard : const_cast<uint8 *>(zero);
    // Reject unmapped accesses before the interpreter can touch unrelated module memory
    unsigned page = address >> 16;
    size_t offset = address & 65535;
    if (!pages[page] || offset > page_end[page] || size > page_end[page] - offset) {
        char text[128];
        snprintf(text, sizeof(text), "Unmapped Macintosh %s at 0x%08x (%zu bytes)", write ? "write" : "read", address, size);
        wahe_fail(text);
        return nullptr;
    }
    // Discard protected scalar writes after ROM initialization just as the native handler does
    if (write) for (unsigned i = 0; i < protection_count; ++i) {
        uint64 end = uint64(address) + size, protected_end = uint64(protections[i].address) + protections[i].size;
        if (uint64(address) < protected_end && uint64(protections[i].address) < end) {
            if (size <= sizeof(discard)) return discard;
            wahe_fail("Bulk write to protected Macintosh memory");
            return nullptr;
        }
    }
    return pages[page] + offset;
}

uint32 wahe_wasm_virtual_address(const uint8 *address)
{
    // Reverse mapped pointers without relying on a fixed host-to-guest offset
    uintptr value = reinterpret_cast<uintptr>(address);
    if (value >= uintptr(gKernelData) && value - uintptr(gKernelData) < sizeof(gKernelData)) return KERNEL_DATA_BASE + value - uintptr(gKernelData);
    for (unsigned i = 0; i < region_count; ++i)
        if (value >= uintptr(regions[i].data) && value - uintptr(regions[i].data) < regions[i].size)
            return regions[i].address + value - uintptr(regions[i].data);
    wahe_fail("Host pointer is outside mapped Macintosh memory");
    return 0;
}
int vm_init() { /* Start with no committed guest regions */ return 0; }
void vm_exit()
{
    // Release only allocated backing stores after the CPU runner has exited
    for (unsigned i = 0; i < region_count; ++i) free(regions[i].data);
    region_count = protection_count = 0;
    memset(pages, 0, sizeof(pages));
    memset(page_end, 0, sizeof(page_end));
}
void *vm_acquire(size_t size, int) { /* Allocate interpreter scratch storage on the module heap */ return calloc(size, 1); }
int vm_acquire_fixed(void *p, size_t size, int)
{
    // Verify fixed allocations have already received a complete guest mapping
    uint32 address = wahe_wasm_virtual_address(static_cast<uint8 *>(p));
    return wahe_wasm_address(address, size, false) == p ? 0 : -1;
}
int vm_release(void *p, size_t)
{
    // Retain guest regions until vm_exit while releasing independent interpreter scratch blocks
    for (unsigned i = 0; i < region_count; ++i) if (regions[i].data == p) return 0;
    free(p);
    return 0;
}
int vm_protect(void *p, size_t size, int flags)
{
    // Record software write protection instead of requesting operating-system page permissions
    if (!(flags & VM_PAGE_WRITE)) {
        if (protection_count == 16) return -1;
        protections[protection_count++] = {wahe_wasm_virtual_address(static_cast<uint8 *>(p)), size};
    }
    return 0;
}
int vm_get_page_size() { /* Match the software protection granularity */ return 4096; }
int vm_get_write_watch(void *, size_t, void **, unsigned int *, int) { /* VOSF is unused */ return -1; }
int vm_reset_write_watch(void *, size_t) { /* VOSF is unused */ return -1; }
int vm_init_reserved(void *) { /* Framebuffers have an explicit guest mapping */ return 0; }
void *vm_acquire_reserved(size_t) { /* Framebuffers have an explicit guest mapping */ return nullptr; }
