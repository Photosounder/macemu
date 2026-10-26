# Native WAHE module

This backend builds SheepShaver as a Windows x86-64 native WAHE DLL. It runs the
existing PowerPC interpreter, ROM patches, Macintosh disk drivers, video driver,
Sound Manager component, ADB emulation, and Time Manager. It exports callbacks
instead of an application `main`, `WinMain`, or `SDL_main`.

SDLwindow owns the window, keyboard, mouse, fullscreen state, and presentation.
SheepShaver returns linear float RGBA framebuffer snapshots and interleaved float audio.
It does not initialize SDL or open its own window or audio device.

## Build

From an MSYS2 UCRT64/MinGW64 shell:

```sh
cd SheepShaver
bash build_wahe.sh
```

The output is `sheepshaver.dll`. The build uses GCC/G++, Perl, and the native WAHE
headers, with `WAHE_INCLUDE_DIR` defaulting to `/d/VC_libs/include`. The compiler
runtime is linked statically. Only Windows system DLLs are required at runtime.
The existing standalone build remains separate.

Common sources are compiled directly from the sibling `shared/` directory;
no source links or copies are needed. This backend has its own configuration
and generates its own PowerPC instruction implementation; running the desktop
`configure` is unnecessary.

Overrides: `CC`, `CXX`, `WAHE_INCLUDE_DIR`, `SHEEPSHAVER_WAHE_BUILD_DIR`, and
`SHEEPSHAVER_WAHE_OUTPUT`.

## Host setup

Edit `sheepshaver.wahe` to locate SDLwindow, a compatible Macintosh ROM, and your
disk image. Its main chain connects SDLwindow's input to SheepShaver and sends
SheepShaver's framebuffer back to SDLwindow. Its audio chain connects
`module_proc_sound` to the configured system audio provider. The host must supply
the standard file commands, usually through `os_basics.dll`.

All file paths are passed to that provider unchanged. Relative image and NVRAM
paths therefore use the host's working directory. Prefix a disk image path with
`*` for a read-only mount; otherwise guest disk writes are sent to the provider.
The sample archive uses a read-only disk mount.

Configuration messages must arrive before `Init`:

```text
SheepShaver ROM path C:\Mac\macos8.6.rom
SheepShaver disk path *C:\Mac\Mac HD.hfv
SheepShaver RAM 64
SheepShaver resolution 800x600
SheepShaver monitor gamma 2.7
SheepShaver sample rate 44100
SheepShaver NVRAM path SheepShaver_nvram.dat
SheepShaver preferences path C:\Mac\SheepShaver_prefs
SheepShaver clock unix <Unix epoch seconds at startup>
```

RAM accepts 16â€“512 MiB. The configured startup resolution accepts 320Ã—200 through
1920Ã—1200. Mac OS also offers 640Ã—480, 800Ã—600, 832Ã—624, and 1024Ã—768, each at
1-, 2-, 4-, 8-, 16-, and 32-bit colour depths. A configured standard size appears
once; any other configured size remains available as an additional mode.
Changing resolution in Mac OS updates the framebuffer dimensions sent to
SDLwindow, which scales the selected guest display into its host window. The audio
rate accepts 8000â€“48000 Hz and should match the connected output provider.
The optional preferences file is read through callbacks, with explicit module
settings taking precedence. Desktop-only settings cannot enable native devices.

`SheepShaver monitor gamma <value>` defines the virtual monitor's response and
accepts finite values from 1.0 through 3.0 before `Init`. Its default is 1.8,
which gives an overall 1.8 response only while the guest's correction table is
identity. The setting describes the monitor response after the guest table;
it does not specify the combined response of the table and monitor. This is an
explicit virtual display model; it does not identify or reproduce an unknown
physical monitor's calibration.

Mac OS can replace the identity table during boot. The tested Mac OS 8.6 image
installs a brightening table that maps 64 to 102, 128 to 161, and 192 to 212 in
each channel. Combining that table with a 1.8 monitor produces a much brighter
image than an overall 1.8 response. The sample archive therefore selects a 2.7
monitor response, which approximates an overall 1.8 response for this table.
Other guest calibrations may require a different setting.

To override all guest gamma tables with identity values, configure this before
`Init`:

```text
Send to SheepShaver: SheepShaver gamma table identity
Send to SheepShaver: SheepShaver monitor gamma 1.8
```

With identity enabled, the monitor setting directly specifies the overall
response. Use 2.2 instead of 1.8 if desired. The driver replaces each guest gamma
request, including later startup or ColorSync calibration updates, with a
256-entry identity table. Indexed palettes, direct-colour output, and guest
`GetGamma` queries all use that effective table. The guest's supplied source
data and disk image are not modified. Gamma-based guest fades or colour effects
are also overridden. `SheepShaver gamma table guest` restores the default policy
when configuring a new instance; both policies are fixed after `Init`. The sample
archive keeps guest-controlled gamma and its existing 2.7 monitor response.

Direct-colour pixels first pass through the guest's per-channel gamma tables.
Indexed colours use the palette already corrected by the common Mac video
driver. Both then become linear light using
`linear = pow(corrected_component / 255, monitor_gamma)`. A 256-entry lookup
table avoids per-pixel exponentiation. The host receives four 32-bit floats per
pixel in RGBA order, with normalized linear RGB and alpha fixed at 1.0, and
handles output encoding. No host display gamma ramp is changed. Calibrations
installed in the guest must target this virtual display to reproduce their
intended response.

The host drives elapsed time through `Get raw time`. Supply `SheepShaver clock
unix` to set the guest's wall-clock date; without it, the date starts at the Unix
epoch. The module does not query the operating system's wall clock.

## Callbacks and ownership

- `module_message_input`: native WAHE setup, configuration, lifecycle, and
  SDLwindow input messages
- `module_draw`: a stable `RGBA float linear` framebuffer descriptor
- `module_proc_sound`: a standard WAHE stereo `32-bit float interleaved` sound
  descriptor, using the requested `Sample count`
- `module_malloc`, `module_realloc`, `module_free`: native message allocation

Native setup follows the existing WAHE protocol: `module_ctx = <hex address>` and
`wahe_run_command_with_id() = <hex address>`. Returned descriptors are borrowed
thread-local strings. Consume them before another callback on that thread.
Image and audio buffers remain valid until their respective next callback or
`Deinit`; each callback kind has separate output storage. The CPU captures screen
pixels into a completed snapshot before a host callback can read them.

`Init` starts a joinable CPU worker and returns immediately. `Status` returns
`configured`, `starting`, `running`, `stopped`, or `failed`, with an error message
when startup or emulation fails. `Stop threads` cooperatively stops and joins the
CPU and timing workers. `Deinit` also releases callback output storage. Both are
idempotent. Stop callback consumers before `Deinit`, and call it before unloading
the DLL. Shutdown is terminal for a loaded instance; reload the DLL to start a
new machine. Each loaded library supports one machine.

Configuration errors and the first asynchronous startup or emulation failure
also print through the host's `Print` callback, so launchers that discard command
replies still show the reason for a black window in their console.

Guest quit and initialization failures return to the host rather than exiting
the host process. The module's vectored exception handler handles guest ROM and
hardware accesses on its CPU thread and leaves faults in other host modules to
their own handlers. It is removed during shutdown.

## I/O boundary

ROM, disk-image, optional preference-file, NVRAM, and diagnostic-log access use
the standard `Open file`, `Get size of file`, `Load from file`, `Write to file`,
and `Close file` commands. Console diagnostics use `Print`. The module has no
native file or device fallback. NVRAM is saved through the provider at shutdown.

Keyboard and mouse messages use SDLwindow's existing syntax. Key repeats are
handled by Mac OS. Absolute and grabbed relative mouse input are supported without
applying duplicate movement from a single input message. Pointer loss or
`Release input` releases held keys and buttons.
The Windows Option/Command swap preference is honored. Mac OS draws its software
cursor; the sample archive hides SDLwindow's extra host cursor.

Press **F12** to capture or release the mouse. Captured input uses SDLwindow's
persistent relative mouse mode and sends movement deltas to the guest's ADB
mouse, so movement continues at the edges of the host window. F12 is reserved
for this toggle and is not sent to Mac OS; key repeats do not toggle repeatedly.
SDLwindow temporarily releases physical capture when its window loses focus
and resumes capture when it regains focus. Stop, close, or `Deinit` releases the
requested capture state.

Capture starts off. To request it from startup, add this archive line:

```text
Send to SheepShaver: SheepShaver mouse capture on
```

`SheepShaver mouse capture off`, `toggle`, and `status` are also accepted before
or during emulation. Status reports the requested state, which can remain on
while SDLwindow is unfocused. The host must provide SDLwindow's registered
`Mouse grab` and `Mouse release` commands. These callbacks run outside the
module API lock so the host may reenter the module safely. Mouse capture does
not capture system keyboard shortcuts.

This backend does not expose This PC/ExtFS, host directory browsing, physical
disks, raw CD controls, serial ports, or networking. Disk images, including images
configured through a preference file, use the host file provider. The guest's own
clipboard works independently; guest clipboard output is offered to the host as
`SheepShaver clipboard type <fourcc> location: <bytes> bytes at <hex address>`.
This optional command must be consumed synchronously. Windows clipboard import
and native file dialogs are not used.

## Headless checks

```sh
bash tests/test_wahe.sh
bash tests/test_wahe.sh 'C:\Mac\macos8.6.rom' 'C:\Mac\Mac HD.hfv'
```

The first command checks framebuffer depths, row padding, input translation,
audio conversion, mute/underrun behavior, native exports, callback setup, failed
initialization, cleanup, and unload. It also checks 1.8/2.2 monitor decoding,
guest correction order, and linear float output in every framebuffer depth.
It also checks standard/custom resolution enumeration, duplicate suppression,
all resolution/depth switches, and 832Ã—624 timing and geometry. The shared
driver's identity override is exercised with one- and three-channel tables and
repeated guest calibration requests.
Capture checks cover F12 press/repeat/release, explicit commands, lost key-up
recovery, typed-command isolation, reentrant window callbacks, and shutdown.
The second also boots the real core and
checks nonblank framebuffer data, audio callbacks, input, and disk I/O. The test
host opens supplied files read-only and keeps all guest disk writes in an
in-memory overlay. No test opens a window or audio device.

To check integration with the real WAHE engine and OS_Basics file provider:

```sh
bash tests/test_wahe_real_host.sh 'C:\Mac\macos8.6.rom' 'C:\Mac\Mac HD.hfv'
```

This headless check requires the local `wahelib` sources and `os_basics.dll`.
Override `WAHELIB_DIR` and `WAHE_FILE_PROVIDER` for other locations. It mounts the
disk image read-only, saves test NVRAM under `build-wahe`, and requires nonblank
guest pixels after reading the ROM through the actual registered file commands.
The read protocol is `Load from file <handle> at <hex offset> to location:
<bytes> bytes at <hex address>`.
