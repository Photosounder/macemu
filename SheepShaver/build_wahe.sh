#!/bin/bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
cd "$(dirname "$0")"

build_dir="${SHEEPSHAVER_WAHE_BUILD_DIR:-build-wahe}"
output="${SHEEPSHAVER_WAHE_OUTPUT:-sheepshaver.dll}"
wahe_include="${WAHE_INCLUDE_DIR:-/d/VC_libs/include}"
cxx="${CXX:-g++}"
case "$(uname -s):$(uname -m)" in
    MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64) ;;
    *) echo 'The native WAHE build currently requires MSYS2 UCRT64/MinGW64 on Windows x86-64' >&2; exit 1 ;;
esac
mkdir -p "$build_dir"
flags=(-std=gnu++17 -O2 -g -DMEM_BULK -DSHEEPSHAVER_WAHE -fpermissive
    -Wno-deprecated-enum-enum-conversion -Wno-deprecated-declarations
    -Isrc/WAHE -I"$build_dir" -Isrc/include -Isrc/Windows -Isrc/kpx_cpu
    -I../shared/include -I../shared/Windows -I../shared/CrossPlatform -I../shared/uae_cpu/compiler
    -Isrc/kpx_cpu/include -Isrc/kpx_cpu/src -I"$wahe_include")

source src/WAHE/sources.sh
sources+=(src/WAHE/vm_wahe.cpp src/WAHE/exception_wahe.cpp)

"$cxx" "${flags[@]}" -E -DGENEXEC src/kpx_cpu/src/cpu/ppc/ppc-decode.cpp |
    perl src/kpx_cpu/src/cpu/ppc/genexec.pl > "$build_dir/ppc-execute-impl.cpp"
objects=()
for source in "${sources[@]}"; do
    object="$build_dir/${source//\//_}.o"
    if [[ "$source" == *.c ]]; then
        "${CC:-gcc}" -std=gnu11 -O2 -g -DSHEEPSHAVER_WAHE -Isrc/WAHE -Isrc/kpx_cpu \
            -Isrc/kpx_cpu/include -c "$source" -o "$object"
    else
        "$cxx" "${flags[@]}" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
"$cxx" -shared -static "${objects[@]}" -o "$output" -Wl,--no-undefined
echo "Built $output (native WAHE callbacks; no SDL or application entry point)"
