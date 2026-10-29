#!/bin/bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
build_script="$(realpath "$0")"
cd "$(dirname "$build_script")"

build_dir="${SHEEPSHAVER_WAHE_BUILD_DIR:-build-wahe/wasm}"
output="${SHEEPSHAVER_WAHE_OUTPUT:-sheepshaver.wasm}"
wahe_include="${WAHE_INCLUDE_DIR:-/d/VC_libs/include}"
sysroot="${WASM_SYSROOT:-$PWD/build-wahe/wasm-toolchain/cache5/sysroot}"
libc_source="${WASM_LIBC_SOURCE:-$PWD/build-wahe/wasm-toolchain/emscripten-5.0.7/system/lib/libc/musl}"
runtime_dir="$sysroot/lib/wasm32-emscripten"
for library in libc++-mt-noexcept libc++abi-mt-noexcept libc-mt libcompiler_rt-mt; do
    if [[ ! -f "$runtime_dir/$library.a" ]]; then
        echo "Missing $runtime_dir/$library.a; set WASM_SYSROOT to the Wasm C/C++ runtime sysroot described in WAHE.md" >&2
        exit 1
    fi
done
if [[ ! -f "$libc_source/src/internal/stdio_impl.h" ]]; then
    echo 'Set WASM_LIBC_SOURCE to the musl source directory matching the runtime archives' >&2
    exit 1
fi
mkdir -p "$build_dir"
compiler=(clang --target=wasm32-unknown-emscripten --sysroot="$sysroot" -D__EMSCRIPTEN__=1
    -isystem "$sysroot/include/compat")
flags=(-std=gnu++17 -O3 -g -fno-exceptions -pthread -D__EMSCRIPTEN_SHARED_MEMORY__=1 -mbulk-memory -msimd128
    -DMEM_BULK -DSHEEPSHAVER_WAHE -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION
    -Wno-deprecated-declarations -Wno-register -Wno-format -Wno-deprecated-enum-enum-conversion
    -Isrc/WAHE -I"$build_dir" -Isrc/include -Isrc/Windows -Isrc/kpx_cpu
    -I../shared/include -I../shared/Windows -I../shared/CrossPlatform
    -Isrc/kpx_cpu/include -Isrc/kpx_cpu/src -I"$wahe_include")
source src/WAHE/sources.sh
sources=("${sources[@]/src\/Windows\/user_strings_windows.cpp/src\/WAHE\/wasm\/user_strings_wasm.cpp}")
sources+=(src/WAHE/wasm/vm_wasm.cpp src/WAHE/wasm/runtime_wasm.cpp src/WAHE/wasm/allocator_wasm.c src/WAHE/wasm/stdio_wasm.c)
"${compiler[@]}" "${flags[@]}" -E -DGENEXEC src/kpx_cpu/src/cpu/ppc/ppc-decode.cpp |
    perl src/kpx_cpu/src/cpu/ppc/genexec.pl > "$build_dir/ppc-execute-impl.tmp"
if ! cmp -s "$build_dir/ppc-execute-impl.tmp" "$build_dir/ppc-execute-impl.cpp"; then
    mv "$build_dir/ppc-execute-impl.tmp" "$build_dir/ppc-execute-impl.cpp"
fi
objects=()
# Invalidate cached objects when the compiler or an external runtime dependency changes
printf '%s\n' "$(clang --version)" "$sysroot" "$libc_source" "$wahe_include" > "$build_dir/configuration.tmp"
rebuild_all=0
if ! cmp -s "$build_dir/configuration.tmp" "$build_dir/configuration"; then
    mv "$build_dir/configuration.tmp" "$build_dir/configuration"
    rebuild_all=1
fi
# Track headers as well as sources when deciding whether an object needs recompiling
mapfile -d '' headers < <(find src/WAHE src/include src/kpx_cpu ../shared/include ../shared/Windows ../shared/CrossPlatform "$wahe_include" \
    -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \) -print0)
headers+=("$build_dir/ppc-execute-impl.cpp" "$build_script" "src/WAHE/sources.sh")
for source in "${sources[@]}"; do
    object="$build_dir/${source//\//_}.o"
    rebuild=$rebuild_all
    if [[ ! -f "$object" || "$source" -nt "$object" ]]; then rebuild=1; fi
    for header in "${headers[@]}"; do
        if [[ "$header" -nt "$object" ]]; then rebuild=1; break; fi
    done
    if [[ "$rebuild" == 0 ]]; then objects+=("$object"); continue; fi
    if [[ "$source" == *.c ]]; then
        "${compiler[@]}" -std=gnu11 -O2 -g -pthread -D__EMSCRIPTEN_SHARED_MEMORY__=1 -mbulk-memory -msimd128 \
            -DSHEEPSHAVER_WAHE -Isrc/WAHE -Isrc/kpx_cpu -Isrc/kpx_cpu/include -I"$wahe_include" \
            -I"$libc_source/src/internal" -I"$libc_source/src/include" \
            -I"$libc_source/arch/emscripten" \
            -c "$source" -o "$object"
    else
        "${compiler[@]}" "${flags[@]}" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
clang --target=wasm32 -nostdlib -mbulk-memory -matomics -msimd128 -fno-exceptions \
    "${objects[@]}" \
    "$runtime_dir/libc++-mt-noexcept.a" "$runtime_dir/libc++abi-mt-noexcept.a" \
    "$runtime_dir/libc-mt.a" "$runtime_dir/libcompiler_rt-mt.a" \
    -Wl,--no-entry,--shared-memory,--import-memory,--export-memory,--stack-first \
    -Wl,--initial-memory=16777216,--max-memory=2147483648 -Wl,-z,stack-size=16384 \
    -Wl,--export=__heap_base,--export=__data_end,--export=__stack_pointer \
    -Wl,--export=__tls_size,--export=__tls_align,--export=__tls_base,--export=__wasm_init_tls \
    -Wl,-Map="$build_dir/sheepshaver.map" -o "$output"
node tests/check_wahe_wasm.mjs "$output"
echo "Built $output (freestanding WAHE callbacks; no WASI or application entry point)"
