# Basilisk II and SheepShaver

### Clankerslop changes by me, Michel Rouzic

- **Merged the two incompatible upstream codebases.** Reconciled the [kanjitalk755/macemu](https://github.com/kanjitalk755/macemu) fork with the latest [cebix/macemu master](https://github.com/cebix/macemu/tree/master), merging the 31 outstanding commits. Resolved conflicts while preserving the fork's existing ARM64, GTK, memory-management, and macOS build configuration, and incorporated the upstream GNU configuration scripts.
- **Fixed SheepShaver's Autotools bootstrap from the Windows build directory.** `Unix/autogen.sh` now finds its bundled `m4` macros relative to the script, preventing the generated `configure` script from stopping at an unexpanded `AM_PATH_GTK_2_0` call.
- **Made SheepShaver's MinGW runtime and SDL linkage static.** The executable can be launched directly without supplying `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`, or `SDL2.dll` alongside it. Regenerating the Makefile also forces the application and preferences editor to relink with the updated settings.
- **Fixed incomplete Windows drive listings in This PC.** Native Windows search metadata now supplies file and directory information when the CRT metadata query fails on protected folders, junctions, locked system files, or oversized files. This resolves the premature endings observed on host drives.
- **Handled oversized host files consistently.** Added an overflow fallback for metadata queries on opened files, and kept logical and allocated sizes within the signed 32-bit range used by the classic Mac file APIs. Oversized files remain visible, with their Mac-reported logical length capped at 2,147,467,263 bytes, just below 2 GiB. Classic Mac file access retains its size limit.
- **Moved Windows Finder metadata into a session-only cache.** Browsing folders no longer creates `.finf` directories. Finder information and extended Finder information stay in memory until the emulator exits, follow renamed files and directories, and are discarded after successful deletion. Path matching follows Windows' case-insensitive behavior. Existing `.finf` files are ignored and left in place; resource forks continue to use persistent `.rsrc` helper files.
- **Fixed This PC icon initialization in 64-bit builds.** Startup initializes the icon's Finder metadata directly in the host-side cache, avoiding an invalid conversion of a Windows buffer into a 32-bit Mac address. Custom icon flags are restored for each session.
- **Fixed the Finder Get Info freeze caused by clipboard import.** The Windows clipboard helper now copies its 68k procedure into allocated Mac memory before executing it, and allocates and frees the procedure and text together. This removes another invalid host-to-Mac pointer conversion, handles native clipboard lengths safely, and resolves the freeze seen in Mac OS 8.6 on both host drives and Mac HD.

### Original readme

This repository contains the Basilisk II 68k Macintosh emulator and the SheepShaver PowerPC Mac OS runtime environment. Both require a copy of Mac OS and a compatible Macintosh ROM image.

Releases and support are available from the [Emaculation community](https://www.emaculation.com/). See the [detailed Basilisk II README](BasiliskII/README.md) for features and configuration information.

#### BasiliskII
```
macOS     x86_64 JIT / arm64 non-JIT
Linux x86 x86_64 JIT / arm64 non-JIT
MinGW x86        JIT
```
#### SheepShaver
```
macOS     x86_64 JIT / arm64 non-JIT
Linux x86 x86_64 JIT / arm64 non-JIT
MinGW x86        JIT
```
### How To Build
These builds need to be installed SDL2.0.14+ framework/library.

https://www.libsdl.org
#### BasiliskII
##### macOS
preparation:

Download gmp-6.2.1.tar.xz from https://gmplib.org.
```
$ cd ~/Downloads
$ tar xf gmp-6.2.1.tar.xz
$ cd gmp-6.2.1
$ ./configure --disable-shared
$ make
$ make check
$ sudo make install
```
Download mpfr-4.2.0.tar.xz from https://www.mpfr.org.
```
$ cd ~/Downloads
$ tar xf mpfr-4.2.0.tar.xz
$ cd mpfr-4.2.0
$ ./configure --disable-shared
$ make
$ make check
$ sudo make install
```
On an Intel Mac, the libraries should be cross-built.  
Change the `configure` command for both GMP and MPFR as follows, and ignore the `make check` command:
```
$ CFLAGS="-arch arm64" CXXFLAGS="$CFLAGS" ./configure -host=aarch64-apple-darwin --disable-shared 
```
(from https://github.com/kanjitalk755/macemu/pull/96)

about changing Deployment Target:  
If you build with an older version of Xcode, you can change Deployment Target to the minimum it supports or 10.7, whichever is greater.

build:
```
$ cd macemu/BasiliskII/src/MacOSX
$ xcodebuild build -project BasiliskII.xcodeproj -configuration Release
```

##### Linux
preparation (arm64 only): Install GMP and MPFR.
```
$ cd macemu/BasiliskII/src/Unix
$ ./autogen.sh
$ make
```
##### MinGW32/MSYS2
preparation:
```
$ pacman -S base-devel mingw-w64-i686-toolchain autoconf automake mingw-w64-i686-SDL2
```
note: MinGW32 dropped GTK2 package.
See msys2/MINGW-packages#24490

build (from a mingw32.exe prompt):
```
$ cd macemu/BasiliskII/src/Windows
$ ../Unix/autogen.sh
$ make
```
#### SheepShaver
##### macOS
about changing Deployment Target: see BasiliskII
```
$ cd macemu/SheepShaver/src/MacOSX
$ xcodebuild build -project SheepShaver_Xcode8.xcodeproj -configuration Release
```

##### Linux
```
$ cd macemu/SheepShaver/src/Unix
$ ./autogen.sh
$ make
```
For Raspberry Pi:
https://github.com/vaccinemedia/macemu

##### MinGW32/MSYS2
preparation: same as BasiliskII  
  
build (from a mingw32.exe prompt):
```
$ cd macemu/SheepShaver
$ make links
$ cd src/Windows
$ ../Unix/autogen.sh
$ make
```
### Recommended key bindings for gnome
https://github.com/kanjitalk755/macemu/blob/master/SheepShaver/doc/Linux/gnome_keybindings.txt

(from https://github.com/kanjitalk755/macemu/issues/59)
