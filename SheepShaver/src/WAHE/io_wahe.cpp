/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "sys.h"
#include "xpram.h"
#include "clip.h"
#include "serial.h"
#include "extfs.h"
#include "extfs_defs.h"
#include <cstdarg>
#include <limits>
#include <sstream>

WaheFile *wahe_file_open(const std::string &path, const char *mode)
{
    // Keep the provider's opaque handle separate from native operating-system handles
    if (path.empty() || path.find_first_of("\r\n") != std::string::npos) return nullptr;
    char *response = wahe_request("Open file (" + std::string(mode) + ") at " + path);
    size_t handle = 0;
    int used = 0;
    bool valid = response && sscanf(response, "File handle %zu%n", &handle, &used) == 1 &&
        handle && !response[used];
    free(response);
    return valid ? new WaheFile{handle, 0, !strchr(mode, '+') && mode[0] == 'r'} : nullptr;
}

void wahe_file_close(WaheFile *file)
{
    // Close only the callback-owned file and release its module-side wrapper
    if (!file) return;
    free(wahe_request("Close file " + std::to_string(file->handle)));
    delete file;
}

size_t wahe_file_read(WaheFile *file, void *buffer, uint64 offset, size_t bytes)
{
    // Read bounded ranges directly into module-owned memory using the standard WAHE file service
    if (!file || (!buffer && bytes) || bytes > UINT64_MAX - offset) return 0;
    size_t total = 0;
    while (total < bytes) {
        size_t count = std::min(bytes - total, size_t(1024 * 1024));
        char command[256];
        snprintf(command, sizeof(command), "Load from file %zu at 0x%llx to location: %zu bytes at 0x%zx",
            file->handle, (unsigned long long)(offset + total), count, (size_t)((uint8 *)buffer + total));
        char *reply = wahe_request(command);
        size_t loaded = 0;
        bool valid = reply && sscanf(reply, "Loaded %zu bytes", &loaded) == 1 && loaded <= count;
        bool failed = reply && strstr(reply, "read error");
        free(reply);
        if (!valid || failed) break;
        total += loaded;
        if (loaded < count) break;
    }
    file->position = offset + total;
    return total;
}

size_t wahe_file_write(WaheFile *file, const void *buffer, uint64 offset, size_t bytes)
{
    // Expose writes only through the provider and preserve read-only disk mounts
    if (!file || file->read_only || (!buffer && bytes) || bytes > UINT64_MAX - offset) return 0;
    size_t total = 0;
    while (total < bytes) {
        size_t count = std::min(bytes - total, size_t(1024 * 1024));
        char command[256];
        snprintf(command, sizeof(command), "Write to file %zu at 0x%llx from location: %zu bytes at 0x%zx",
            file->handle, (unsigned long long)(offset + total), count, (size_t)((const uint8 *)buffer + total));
        char *reply = wahe_request(command);
        size_t written = 0;
        unsigned long long position = 0;
        bool valid = reply && sscanf(reply, "Wrote %zu bytes, position %llx", &written, &position) == 2 && written <= count;
        bool failed = reply && strstr(reply, "write error");
        free(reply);
        if (!valid || failed) break;
        total += written;
        if (written < count) break;
    }
    file->position = offset + total;
    return total;
}

uint64 wahe_file_size(WaheFile *file)
{
    // Query an exact 64-bit size without seeking a native file descriptor
    if (!file) return 0;
    char *reply = wahe_request("Get size of file " + std::to_string(file->handle));
    unsigned long long size = 0;
    if (reply) sscanf(reply, "File size %llu bytes", &size);
    free(reply);
    return size;
}

static int print_message(FILE *stream, const char *format, va_list args)
{
    // Format once and deliver console output or legacy stream output through callbacks
    va_list copy;
    va_copy(copy, args);
    int size = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (size < 0) return size;
    std::vector<char> message(size_t(size) + 1);
    vsnprintf(message.data(), message.size(), format, args);
    if (stream == stdout || stream == stderr || !stream) wahe_log(message.data());
    else {
        WaheFile *file = reinterpret_cast<WaheFile *>(stream);
        if (wahe_file_write(file, message.data(), file->position, size) != size_t(size)) return -1;
    }
    return size;
}

int wahe_printf(const char *format, ...)
{
    // Route legacy core diagnostics to the host's Print command
    va_list args;
    va_start(args, format);
    int result = print_message(stdout, format, args);
    va_end(args);
    return result;
}

int wahe_fprintf(FILE *stream, const char *format, ...)
{
    // Preserve formatted output while preventing direct CRT file access
    va_list args;
    va_start(args, format);
    int result = print_message(stream, format, args);
    va_end(args);
    return result;
}

int wahe_fflush(FILE *)
{
    // Provider writes are synchronous and already flushed before acknowledging completion
    return 0;
}

FILE *wahe_fopen(const char *path, const char *mode)
{
    // Represent legacy FILE pointers using opaque callback-backed streams
    return reinterpret_cast<FILE *>(wahe_file_open(path, mode));
}

int wahe_fclose(FILE *stream)
{
    // Close callback-backed legacy streams without passing them to the CRT
    wahe_file_close(reinterpret_cast<WaheFile *>(stream));
    return 0;
}

char *wahe_fgets(char *text, int capacity, FILE *stream)
{
    // Read preference lines through the same provider as disk and ROM data
    if (!stream || !text || capacity <= 1) return nullptr;
    WaheFile *file = reinterpret_cast<WaheFile *>(stream);
    int count = 0;
    while (count + 1 < capacity) {
        char c;
        if (wahe_file_read(file, &c, file->position, 1) != 1) break;
        text[count++] = c;
        if (c == '\n') break;
    }
    text[count] = 0;
    return count ? text : nullptr;
}

void SysInit() { /* Keep device ownership in the host */ }
void SysExit() { /* Disk drivers close their callback handles during ExitAll */ }
void SysAddFloppyPrefs() { /* Require explicit host configuration */ }
void SysAddDiskPrefs() { /* Require explicit host configuration */ }
void SysAddCDROMPrefs() { /* Require explicit host configuration */ }
void SysAddSerialPrefs() { /* Do not enumerate host serial devices */ }
void ether_reset() { /* No native networking backend is installed */ }
void *Sys_open(const char *path, bool read_only, bool cdrom)
{
    // Open disk images through the provider without falling back to raw devices
    return wahe_file_open(path, read_only || cdrom ? "rb" : "r+b");
}
void Sys_close(void *file) { /* Release the provider handle */ wahe_file_close((WaheFile *)file); }
size_t Sys_read(void *f, void *b, loff_t offset, size_t n)
{
    // Reject negative disk offsets before serializing the provider request
    return offset < 0 ? 0 : wahe_file_read((WaheFile *)f, b, offset, n);
}
size_t Sys_write(void *f, void *b, loff_t offset, size_t n)
{
    // Reject negative disk offsets before serializing the provider request
    return offset < 0 ? 0 : wahe_file_write((WaheFile *)f, b, offset, n);
}
loff_t SysGetFileSize(void *f) { /* Preserve image sizes beyond 4 GiB */ return wahe_file_size((WaheFile *)f); }
bool SysIsReadOnly(void *f) { /* Preserve the mount's write capability */ return !f || ((WaheFile *)f)->read_only; }
bool SysIsFixedDisk(void *) { /* Mounted images are fixed media */ return true; }
bool SysIsDiskInserted(void *f) { /* A live provider handle represents mounted media */ return f != nullptr; }
void SysEject(void *) { /* Guest ejection does not eject a host device */ }
bool SysFormat(void *) { /* Raw-device formatting is unavailable */ return false; }
void SysPreventRemoval(void *) { /* The host owns removable devices */ }
void SysAllowRemoval(void *) { /* The host owns removable devices */ }
bool SysCDReadTOC(void *, uint8 *) { /* Raw CD controls are unavailable */ return false; }
bool SysCDGetPosition(void *, uint8 *) { /* Raw CD controls are unavailable */ return false; }
bool SysCDPlay(void *, uint8, uint8, uint8, uint8, uint8, uint8) { /* Raw CD audio is unavailable */ return false; }
bool SysCDPause(void *) { /* Raw CD audio is unavailable */ return false; }
bool SysCDResume(void *) { /* Raw CD audio is unavailable */ return false; }
bool SysCDStop(void *, uint8, uint8, uint8) { /* Raw CD audio is unavailable */ return false; }
bool SysCDScan(void *, uint8, uint8, uint8, bool) { /* Raw CD audio is unavailable */ return false; }
void SysCDSetVolume(void *, uint8, uint8) { /* The audio callback owns output gain */ }
void SysCDGetVolume(void *, uint8 &a, uint8 &b) { /* Return a deterministic unsupported-device value */ a = b = 0; }

void LoadXPRAM(const char *)
{
    // Load NVRAM only through a host-provided file handle
    WaheFile *file = wahe_file_open(wahe_nvram_path, "rb");
    if (file) wahe_file_read(file, XPRAM, 0, XPRAM_SIZE);
    wahe_file_close(file);
}
void SaveXPRAM()
{
    // Persist NVRAM through a host write after the CPU has stopped or at a checkpoint
    WaheFile *file = wahe_file_open(wahe_nvram_path, "wb");
    if (!file) {
        // Make provider permission and persistence failures visible to the host
        wahe_log("Cannot open NVRAM for writing through the host provider");
        return;
    }
    if (file && wahe_file_write(file, XPRAM, 0, XPRAM_SIZE) != XPRAM_SIZE) wahe_log("NVRAM write failed");
    wahe_file_close(file);
}
void ZapPRAM() { /* Reset the emulated bytes without deleting host files */ memset(XPRAM, 0, XPRAM_SIZE); }

void SerialInit() { /* No serial device is opened by this module */ }
void SerialExit() { /* There are no native serial workers to stop */ }
void SerialInterrupt() { /* Serial devices are not advertised */ }
int16 SerialOpen(uint32, uint32) { /* Report an unavailable device */ return openErr; }
int16 SerialPrimeIn(uint32, uint32) { /* Report an unavailable device */ return notOpenErr; }
int16 SerialPrimeOut(uint32, uint32) { /* Report an unavailable device */ return notOpenErr; }
int16 SerialControl(uint32, uint32) { /* Report an unavailable device */ return notOpenErr; }
int16 SerialStatus(uint32, uint32) { /* Report an unavailable device */ return notOpenErr; }
int16 SerialClose(uint32, uint32) { /* Report an unavailable device */ return notOpenErr; }
int16 SerialNothing(uint32, uint32) { /* Preserve the driver's no-op operation */ return noErr; }

void ExtFSInit() { /* Do not expose the host filesystem as This PC */ }
void ExtFSExit() { /* No host catalog state is retained */ }
void InstallExtFS() { /* Only explicitly mounted image files are exposed */ }
int16 ExtFSComm(uint16, uint32, uint32) { /* Report an uninstalled external filesystem */ return paramErr; }
int16 ExtFSHFS(uint32, uint16, uint32, uint32, int16) { /* Report an uninstalled external filesystem */ return paramErr; }

void ClipInit() { /* The host may send clipboard data through messages */ }
void ClipExit() { /* No operating-system clipboard is owned */ }
void ZeroScrap() { /* Guest clipboard state remains owned by Mac OS */ }
void GetScrap(void **, uint32, int32) { /* Leave the guest clipboard intact when no provider is configured */ }
void PutScrap(uint32 type, void *data, int32 length)
{
    // Publish guest clipboard bytes as a callback descriptor without accessing the Windows clipboard
    if (length < 0 || length > 16 * 1024 * 1024) return;
    char command[256];
    snprintf(command, sizeof(command), "SheepShaver clipboard type %08x location: %d bytes at 0x%zx", type, length, (size_t)data);
    free(wahe_request(command));
}
