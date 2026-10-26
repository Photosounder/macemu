/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHEEPSHAVER_WAHE_CONFIG_H
#define SHEEPSHAVER_WAHE_CONFIG_H
#if defined(__wasm32__)
#define SHEEPSHAVER_WAHE_WASM 1
#elif !defined(_WIN32) || !defined(__x86_64__)
#error The native WAHE backend currently targets Windows x86-64
#endif
#define HAVE_CEIL 1
#define HAVE_CEILF 1
#define HAVE_EXP2 1
#define HAVE_EXP2F 1
#define HAVE_FLOOR 1
#define HAVE_FLOORF 1
#define HAVE_LOG2 1
#define HAVE_LOG2F 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#ifndef SHEEPSHAVER_WAHE_WASM
#define HAVE_WIN32_EXCEPTIONS 1
#define HAVE_WIN32_VM 1
#define HAVE_SIGSEGV_SKIP_INSTRUCTION 1
#endif
#define SIZEOF_DOUBLE 8
#define SIZEOF_FLOAT 4
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_SHORT 2
#ifdef SHEEPSHAVER_WAHE_WASM
#define SIZEOF_VOID_P 4
#else
#define SIZEOF_VOID_P 8
#endif
#define STDC_HEADERS 1
#define _FILE_OFFSET_BITS 64
#define PACKAGE_NAME "SheepShaver WAHE"
#define PACKAGE_VERSION "2.3"
#endif
