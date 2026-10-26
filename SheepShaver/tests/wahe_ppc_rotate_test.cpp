/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sysdeps.h"
#include "cpu/ppc/ppc-operations.hpp"

#ifdef __wasm32__
extern "C" __attribute__((export_name("test_rotates")))
#endif
int test_rotates()
{
    // Compare the instruction helpers with repeated single-bit rotations at every word boundary
    const uint32 values[] = {0, 1, 0x80000000U, 0xffffffffU, 0x12345678U, 0xdeadbeefU};
    for (uint32 value : values) for (uint32 count = 0; count < 64; ++count) {
        // Build independent references without a shift count that can reach the word size
        uint32 left = value, right = value;
        for (uint32 bit = 0; bit < (count & 31); ++bit) {
            // Move one end bit to the opposite end using only defined unsigned operations
            left = (left << 1) | (left >> 31);
            right = (right >> 1) | (right << 31);
        }
        if (op_rotl::apply(value, count) != left || op_rotr::apply(value, count) != right) return 0;
    }
    return 1;
}
#ifndef __wasm32__
int main()
{
    // Make the same regression runnable without a Wasm host or a window
    return test_rotates() ? 0 : 1;
}
#endif
