# Shared emulator code

This directory contains the source files used by both Basilisk II and SheepShaver:
common Macintosh device emulation and headers, host backends, SDL support, networking,
memory helpers, and build support. Platform subdirectories retain their original names.

Both emulators compile these files directly. Do not copy them into an emulator's
`src/` directory or run the former `make links` target. Keep `BasiliskII/`,
`SheepShaver/`, and `shared/` as siblings when building from a checkout. Source
archives include the selected emulator alongside `shared/`.

Emulator-specific headers and configuration stay in each emulator's source tree
and take precedence over the common header search paths. The two CPU cores,
video drivers, and exception handler implementations remain separate.
