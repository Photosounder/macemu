#!/bin/sh
# Package an emulator and its shared sources without copying or linking sources
set -eu
emulator=$1
version=$2
output=$3
case "$emulator" in
    BasiliskII|SheepShaver) ;;
    *) echo "Unknown emulator: $emulator" >&2; exit 1 ;;
esac
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
tar -czf "$output" --transform="s,^,macemu-$version/," \
    --exclude='*/build' --exclude='*/build-wahe' --exclude='*/obj' \
    --exclude='*/obj.*' --exclude='autom4te.cache' \
    --exclude='*.o' --exclude='*.ho' --exclude='*.exe' --exclude='*.dll' \
    --exclude='*.log' --exclude='*.tar.gz' --exclude='*.rom' --exclude='*.hfv' \
    --exclude='*.dsk' --exclude='*~' --exclude='*.bak' \
    --exclude='*_nvram.dat' --exclude='*.nvram' \
    --exclude='*/src/Unix/SheepShaver' --exclude='*/src/Unix/BasiliskII' \
    --exclude='config.cache' --exclude='config.status' \
    --exclude='*/src/Unix/config.h' --exclude='*/src/Windows/config.h' \
    --exclude='*/src/Unix/Makefile' --exclude='*/src/Windows/Makefile' \
    --exclude='ppc-execute-impl.cpp' --exclude='cpuemu.cpp' --exclude='cpustbl.cpp' \
    --exclude='cpudefs.cpp' --exclude='cputbl.h' --exclude='compemu.cpp' \
    --exclude='comptbl.h' --exclude='compstbl.cpp' \
    "$emulator" shared tools/make-source-archive.sh
