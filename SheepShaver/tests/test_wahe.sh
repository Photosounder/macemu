#!/bin/bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
cd "$(dirname "$0")/.."
mkdir -p build-wahe
${CXX:-g++} -std=gnu++17 -O2 -static -DMEM_BULK -DSHEEPSHAVER_WAHE \
    -Isrc/WAHE -Isrc/include -I../shared/include -I../shared/Windows -I../shared/CrossPlatform -I../shared/uae_cpu/compiler -Isrc/Windows -Isrc/kpx_cpu/include -Isrc/kpx_cpu/src \
    tests/wahe_av_test.cpp src/WAHE/video_wahe.cpp src/WAHE/audio_wahe.cpp src/WAHE/vm_wahe.cpp \
    -o build-wahe/wahe_av_test.exe
build-wahe/wahe_av_test.exe
${CXX:-g++} -std=gnu++17 -O2 -static -DMEM_BULK -DSHEEPSHAVER_WAHE -fpermissive \
    -flto -fwhole-program \
    -Isrc/WAHE -Isrc/include -I../shared/include -I../shared/Windows -I../shared/CrossPlatform -I../shared/uae_cpu/compiler -Isrc/Windows -Isrc/kpx_cpu/include -Isrc/kpx_cpu/src \
    tests/wahe_gamma_test.cpp src/WAHE/vm_wahe.cpp -o build-wahe/wahe_gamma_test.exe
build-wahe/wahe_gamma_test.exe
${CXX:-g++} -std=gnu++17 -O2 -static tests/wahe_host_test.cpp -o build-wahe/wahe_host_test.exe
build-wahe/wahe_host_test.exe "$(cygpath -w "${SHEEPSHAVER_WAHE_OUTPUT:-$PWD/sheepshaver.dll}")" "$@"
