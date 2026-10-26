# Shared source list for the native and freestanding Wasm WAHE builds
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
    src/WAHE/timer_wahe.cpp)
