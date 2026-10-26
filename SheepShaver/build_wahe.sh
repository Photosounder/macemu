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

sources=(src/main.cpp ../shared/prefs.cpp src/prefs_items.cpp src/rom_patches.cpp
    src/rsrc_patches.cpp src/emul_op.cpp src/name_registry.cpp src/macos_util.cpp
    ../shared/timer.cpp ../shared/xpram.cpp ../shared/adb.cpp ../shared/sony.cpp ../shared/disk.cpp ../shared/cdrom.cpp
    ../shared/scsi.cpp ../shared/dummy/scsi_dummy.cpp src/gfxaccel.cpp src/video.cpp ../shared/audio.cpp
    src/ether.cpp src/dummy/ether_dummy.cpp src/thunks.cpp src/user_strings.cpp
    src/Windows/user_strings_windows.cpp
    src/kpx_cpu/src/mathlib/ieeefp.cpp src/kpx_cpu/src/mathlib/mathlib.cpp
    src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp src/kpx_cpu/src/cpu/ppc/ppc-decode.cpp
    src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp src/kpx_cpu/src/cpu/ppc/ppc-translate.cpp
    src/kpx_cpu/src/utils/utils-cpuinfo.cpp src/kpx_cpu/sheepshaver_glue.cpp
    src/kpx_cpu/ppc-dis.c
    src/WAHE/module_wahe.cpp src/WAHE/main_wahe.cpp src/WAHE/io_wahe.cpp
    src/WAHE/prefs_wahe.cpp src/WAHE/video_wahe.cpp src/WAHE/audio_wahe.cpp
    src/WAHE/timer_wahe.cpp src/WAHE/vm_wahe.cpp src/WAHE/exception_wahe.cpp)

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
