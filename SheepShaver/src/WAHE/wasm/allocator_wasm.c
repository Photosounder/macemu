/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wahe_imports.h>
#define CITA_WASM_THREADS
#include <cita_wasm.h>
#define CITA_WASM_IMPLEMENTATION_PART2
#include <cita_wasm.h>
#undef malloc
#undef calloc
#undef realloc
#undef free

void *malloc(size_t size)
{
    // Use the same shared-memory allocator as the other freestanding WAHE modules
    return cita_wasm_malloc(size ? size : 1, __FILE__, __func__, __LINE__);
}
void *calloc(size_t count, size_t size)
{
    // Reject overflow before requesting a zero-initialized module allocation
    if (size && count > SIZE_MAX / size) return NULL;
    return cita_wasm_calloc(count ? count : 1, size ? size : 1, __FILE__, __func__, __LINE__);
}
void *realloc(void *pointer, size_t size)
{
    // Preserve the C allocator contract for zero-size requests
    if (!size) { cita_wasm_free(pointer, __FILE__, __func__, __LINE__); return NULL; }
    return cita_wasm_realloc(pointer, size, __FILE__, __func__, __LINE__);
}
void free(void *pointer)
{
    // Release module-owned buffers under the allocator's recursive runner lock
    cita_wasm_free(pointer, __FILE__, __func__, __LINE__);
}
void *__libc_malloc(size_t size) { /* Keep runtime allocations on the module allocator */ return malloc(size); }
void *__libc_calloc(size_t count, size_t size) { /* Keep runtime zeroed allocations on the module allocator */ return calloc(count, size); }
void __libc_free(void *pointer) { /* Release runtime allocations through the module allocator */ free(pointer); }
int posix_memalign(void **result, size_t alignment, size_t size)
{
    // Satisfy the fundamental alignments used by this interpreter and reject unsupported requests explicitly
    if (!alignment || (alignment & (alignment - 1)) || alignment > 16) return 22;
    void *pointer = malloc(size);
    if (!pointer) return 12;
    *result = pointer;
    return 0;
}
