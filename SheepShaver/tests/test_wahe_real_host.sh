#!/bin/bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
cd "$(dirname "$0")/.."
if [[ $# != 2 ]]; then
    echo "Usage: bash tests/test_wahe_real_host.sh ROM DISK_IMAGE" >&2
    exit 2
fi
wahelib_dir="${WAHELIB_DIR:-/c/msys/home/wahelib}"
provider="${WAHE_FILE_PROVIDER:-/c/msys/home/wahe/modules/os_basics/os_basics.dll}"
mkdir -p build-wahe
${CC:-clang} tests/wahe_real_host_test.c -I"$(dirname "$wahelib_dir")" \
    -Wno-incompatible-pointer-types -Wno-pointer-sign -Wno-macro-redefined \
    -lwinmm -lcomdlg32 -lole32 -lShcore -lDbgHelp -o build-wahe/wahe_real_host_test.exe
cat > build-wahe/real-host-test.wahe <<EOF
WAHE 0.0
Module OS_Basics: "$(cygpath -m "$provider")"
Module SheepShaver: "$(cygpath -m "${SHEEPSHAVER_WAHE_OUTPUT:-$PWD/sheepshaver.dll}")"
Send to SheepShaver: SheepShaver ROM path $(cygpath -m "$1")
Send to SheepShaver: SheepShaver disk path *$(cygpath -m "$2")
Send to SheepShaver: SheepShaver NVRAM path $(cygpath -m "$PWD/build-wahe/real-host-test.nvram")
EOF
build-wahe/wahe_real_host_test.exe "$(cygpath -m "$PWD/build-wahe/real-host-test.wahe")"
