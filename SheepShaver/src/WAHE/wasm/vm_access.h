/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHEEPSHAVER_WASM_VM_ACCESS_H
#define SHEEPSHAVER_WASM_VM_ACCESS_H
uint8 *wahe_wasm_address(uint32 address, size_t bytes, bool write);
uint32 wahe_wasm_virtual_address(const uint8 *address);
static inline uint8 *vm_do_get_real_address(vm_addr_t address)
{
    // Translate guest addresses into compact module-owned backing stores
    return wahe_wasm_address(address, 1, false);
}
static inline vm_addr_t vm_do_get_virtual_address(uint8 *address)
{
    // Preserve fixed Macintosh addresses independently of the Wasm allocator layout
    return wahe_wasm_virtual_address(address);
}
template<int Size> static inline uint8 *wahe_wasm_read(vm_addr_t address)
{
    // Check the entire scalar access including reads at the end of a mapped region
    uint8 *p = wahe_wasm_address(address, Size, false);
    // Supply bounded zero data until the interpreter reaches its cooperative stop check
    static const uint8 zero[Size] = {};
    return p ? p : const_cast<uint8 *>(zero);
}
template<int Size> static inline uint8 *wahe_wasm_write(vm_addr_t address)
{
    // Send protected ROM and hardware writes to discard storage without taking a Wasm trap
    uint8 *p = wahe_wasm_address(address, Size, true);
    // Keep an invalid scalar write inside runner-local discard storage until execution returns
    static thread_local uint8 discard[Size];
    return p ? p : discard;
}
static inline uint32 vm_read_memory_1(vm_addr_t a) { /* Read one guest byte */ return vm_do_read_memory_1(wahe_wasm_read<1>(a)); }
static inline uint32 vm_read_memory_2(vm_addr_t a) { /* Read a big-endian guest halfword */ return vm_do_read_memory_2((uint16 *)wahe_wasm_read<2>(a)); }
static inline uint32 vm_read_memory_4(vm_addr_t a) { /* Read a big-endian guest word */ return vm_do_read_memory_4((uint32 *)wahe_wasm_read<4>(a)); }
static inline uint64 vm_read_memory_8(vm_addr_t a) { /* Read a big-endian guest doubleword */ return vm_do_read_memory_8((uint64 *)wahe_wasm_read<8>(a)); }
static inline uint32 vm_read_memory_2_reversed(vm_addr_t a) { /* Read a little-endian guest halfword */ return vm_do_read_memory_2_reversed((uint16 *)wahe_wasm_read<2>(a)); }
static inline uint32 vm_read_memory_4_reversed(vm_addr_t a) { /* Read a little-endian guest word */ return vm_do_read_memory_4_reversed((uint32 *)wahe_wasm_read<4>(a)); }
#define vm_read_memory_1_reversed vm_read_memory_1
static inline void vm_write_memory_1(vm_addr_t a, uint32 v) { /* Write one guest byte */ vm_do_write_memory_1(wahe_wasm_write<1>(a), v); }
static inline void vm_write_memory_2(vm_addr_t a, uint32 v) { /* Write a big-endian guest halfword */ vm_do_write_memory_2((uint16 *)wahe_wasm_write<2>(a), v); }
static inline void vm_write_memory_4(vm_addr_t a, uint32 v) { /* Write a big-endian guest word */ vm_do_write_memory_4((uint32 *)wahe_wasm_write<4>(a), v); }
static inline void vm_write_memory_8(vm_addr_t a, uint64 v) { /* Write a big-endian guest doubleword */ vm_do_write_memory_8((uint64 *)wahe_wasm_write<8>(a), v); }
static inline void vm_write_memory_2_reversed(vm_addr_t a, uint32 v) { /* Write a little-endian guest halfword */ vm_do_write_memory_2_reversed((uint16 *)wahe_wasm_write<2>(a), v); }
static inline void vm_write_memory_4_reversed(vm_addr_t a, uint32 v) { /* Write a little-endian guest word */ vm_do_write_memory_4_reversed((uint32 *)wahe_wasm_write<4>(a), v); }
#define vm_write_memory_1_reversed vm_write_memory_1
static inline void *vm_memset(vm_addr_t a, int c, size_t n)
{
    // Skip invalid bulk writes without passing an invalid pointer to the C runtime
    uint8 *p = wahe_wasm_address(a, n, true);
    return p ? memset(p, c, n) : nullptr;
}
static inline void *vm_memcpy(void *d, vm_addr_t s, size_t n)
{
    // Copy only a fully mapped guest source
    uint8 *p = wahe_wasm_address(s, n, false);
    return p ? memcpy(d, p, n) : nullptr;
}
static inline void *vm_memcpy(vm_addr_t d, const void *s, size_t n)
{
    // Copy only into a fully mapped writable guest destination
    uint8 *p = wahe_wasm_address(d, n, true);
    return p ? memcpy(p, s, n) : nullptr;
}
static inline void *vm_memcpy(vm_addr_t d, vm_addr_t s, size_t n)
{
    // Validate both guest ranges before copying any bytes
    uint8 *source = wahe_wasm_address(s, n, false), *destination = wahe_wasm_address(d, n, true);
    return source && destination ? memcpy(destination, source, n) : nullptr;
}
#endif
