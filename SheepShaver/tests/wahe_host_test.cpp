/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <atomic>
#include <stdexcept>

using Callback = char *(*)(char *);
struct File {
    FILE *source = nullptr;
    std::vector<uint8_t> memory;
    std::map<uint64_t, std::array<uint8_t, 512>> overlay;
    bool disk = false;
};
struct Host {
    HMODULE library;
    Callback input, draw, sound;
    void *(*allocate)(size_t);
    void (*release)(void *);
    std::recursive_mutex mutex;
    std::map<size_t, File> files;
    std::string rom, disk, last_log;
    size_t next_handle = 1, reads = 0, writes = 0, time_queries = 0;
    size_t mouse_grabs = 0, mouse_releases = 0;
    bool mouse_captured = false;

    explicit Host(const char *path)
    {
        // Load only the module and its six ordinary native WAHE exports
        library = LoadLibraryA(path);
        if (!library) throw std::runtime_error("Cannot load native WAHE library");
        input = (Callback)GetProcAddress(library, "module_message_input");
        draw = (Callback)GetProcAddress(library, "module_draw");
        sound = (Callback)GetProcAddress(library, "module_proc_sound");
        allocate = (void *(*)(size_t))GetProcAddress(library, "module_malloc");
        release = (void (*)(void *))GetProcAddress(library, "module_free");
        assert(input && draw && sound && allocate && release);
        assert(GetProcAddress(library, "module_realloc"));
        assert(!GetProcAddress(library, "main") && !GetProcAddress(library, "SDL_main"));
    }
    std::string send(const std::string &message)
    {
        // Copy borrowed callback replies before another callback can reuse thread-local storage
        char *reply = input(const_cast<char *>(message.c_str()));
        return reply ? reply : "";
    }
    char *response(const std::string &message)
    {
        // Allocate provider replies with the module's allocator just like the native host
        char *result = (char *)allocate(message.size() + 1);
        assert(result);
        memcpy(result, message.c_str(), message.size() + 1);
        return result;
    }
    void bind()
    {
        // Exercise the native callback-pointer setup protocol
        char message[256];
        snprintf(message, sizeof(message), "module_ctx = %zx\nwahe_run_command_with_id() = %zx", (size_t)this, (size_t)command);
        assert(send(message).empty());
    }
    static char *command(void *context, char *message)
    {
        // Process provider calls from CPU, timing and callback threads without creating GUI or audio devices
        auto &host = *(Host *)context;
        std::lock_guard<std::recursive_mutex> lock(host.mutex);
        std::string text = message;
        if (text == "Get raw time") {
            ++host.time_queries;
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            char reply[128];
            snprintf(reply, sizeof(reply), "Raw time %.9f", seconds);
            return host.response(reply);
        }
        if (text == "Get audio frame specs") {
            // Exercise synchronous host reentrancy while drawing and CPU file access run on other threads
            assert(!host.send("Status").empty());
            return host.response("Sample count: 512\nSample rate: 44100\nChannels: 2");
        }
        if (text.compare(0, 6, "Print ") == 0) { host.last_log = text; return nullptr; }
        if (text == "Mouse grab" || text == "Mouse release") {
            // Model SDLwindow capture and reenter Status to detect API-lock and host-lock inversions
            assert(!host.send("Status").empty());
            host.mouse_captured = text == "Mouse grab";
            if (host.mouse_captured) ++host.mouse_grabs; else ++host.mouse_releases;
            return nullptr;
        }
        if (text.compare(0, 11, "Open file (") == 0) {
            size_t end = text.find(") at ");
            if (end == std::string::npos) return nullptr;
            std::string mode = text.substr(11, end - 11), path = text.substr(end + 5);
            File file;
            if (path == "test.rom" && !host.rom.empty()) file.source = fopen(host.rom.c_str(), "rb");
            else if (path == "test.hfv" && !host.disk.empty()) {
                file.source = fopen(host.disk.c_str(), "rb");
                file.disk = true;
            } else if (path == "invalid.rom") file.memory.resize(64);
            else if (mode[0] != 'w') return nullptr;
            if ((path == "test.rom" || path == "test.hfv") && !file.source) return nullptr;
            size_t handle = host.next_handle++;
            host.files.emplace(handle, std::move(file));
            return host.response("File handle " + std::to_string(handle));
        }
        size_t handle = 0, bytes = 0, address = 0;
        unsigned long long offset = 0;
        if (sscanf(message, "Load from file %zu at %llx to location: %zu bytes at %zx", &handle, &offset, &bytes, &address) == 4) {
            // Read user fixtures through read-only native handles and apply only in-memory guest writes
            auto found = host.files.find(handle);
            if (found == host.files.end()) return nullptr;
            File &file = found->second;
            size_t loaded = 0;
            if (file.source) {
                assert(_fseeki64(file.source, offset, SEEK_SET) == 0);
                loaded = fread((void *)address, 1, bytes, file.source);
                for (size_t i = 0; i < loaded;) {
                    uint64_t sector = (offset + i) / 512;
                    size_t within = (offset + i) % 512;
                    size_t count = std::min(loaded - i, 512 - within);
                    auto overlay = file.overlay.find(sector);
                    if (overlay != file.overlay.end()) memcpy((uint8_t *)address + i, overlay->second.data() + within, count);
                    i += count;
                }
            } else if (offset < file.memory.size()) {
                loaded = std::min(bytes, file.memory.size() - (size_t)offset);
                memcpy((void *)address, file.memory.data() + offset, loaded);
            }
            ++host.reads;
            return host.response("Loaded " + std::to_string(loaded) + " bytes");
        }
        if (sscanf(message, "Write to file %zu at %llx from location: %zu bytes at %zx", &handle, &offset, &bytes, &address) == 4) {
            // Preserve real disk images by storing every guest disk write in a sector overlay
            auto found = host.files.find(handle);
            if (found == host.files.end() || bytes > 1024 * 1024) return nullptr;
            File &file = found->second;
            if (file.disk) {
                for (size_t i = 0; i < bytes;) {
                    uint64_t sector = (offset + i) / 512;
                    size_t within = (offset + i) % 512;
                    size_t count = std::min(bytes - i, 512 - within);
                    auto inserted = file.overlay.try_emplace(sector);
                    if (inserted.second) {
                        assert(_fseeki64(file.source, sector * 512, SEEK_SET) == 0);
                        fread(inserted.first->second.data(), 1, 512, file.source);
                    }
                    memcpy(inserted.first->second.data() + within, (uint8_t *)address + i, count);
                    i += count;
                }
            } else {
                if (offset > 1024 * 1024) return nullptr;
                file.memory.resize((size_t)offset + bytes);
                memcpy(file.memory.data() + offset, (void *)address, bytes);
            }
            ++host.writes;
            char reply[160];
            snprintf(reply, sizeof(reply), "Wrote %zu bytes, position 0x%llx", bytes, offset + bytes);
            return host.response(reply);
        }
        if (sscanf(message, "Get size of file %zu", &handle) == 1) {
            // Report 64-bit image lengths through the same provider interface used by the module
            auto found = host.files.find(handle);
            if (found == host.files.end()) return nullptr;
            uint64_t size = found->second.memory.size();
            if (found->second.source) {
                _fseeki64(found->second.source, 0, SEEK_END);
                size = _ftelli64(found->second.source);
            }
            return host.response("File size " + std::to_string(size) + " bytes");
        }
        if (sscanf(message, "Close file %zu", &handle) == 1) {
            // Close and erase every provider handle before testing DLL unload
            auto found = host.files.find(handle);
            if (found == host.files.end()) return nullptr;
            if (found->second.source) fclose(found->second.source);
            host.files.erase(found);
            return host.response("Closed file " + std::to_string(handle));
        }
        return nullptr;
    }
    ~Host()
    {
        // Stop both workers before unloading code referenced by callbacks or thread-local state
        send("Deinit");
        assert(files.empty());
        FreeLibrary(library);
    }
};

int main(int argc, char **argv)
{
    // Keep all tests headless and suppress operating-system crash-report dialogs
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    assert(argc >= 2);
    {
        Host host(argv[1]);
        assert(host.send("Init").find("Bind the native WAHE") != std::string::npos);
        host.bind();
        // Toggle capture once per F12 press and preserve replies across a reentrant window provider
        assert(host.send("SheepShaver mouse capture status") == "Mouse capture: off");
        assert(host.send("Key down: 69 / \"F12\" / \"F12\"") == "Mouse capture: on");
        assert(host.mouse_captured && host.mouse_grabs == 1);
        host.send("Key repeat: 69\nKey down: 69");
        assert(host.mouse_grabs == 1 && host.mouse_releases == 0);
        host.send("Key up: 69\nKey down: 69\nKey up: 69");
        assert(!host.mouse_captured && host.mouse_releases == 1);
        assert(host.send("SheepShaver mouse capture on") == "Mouse capture: on");
        assert(host.send("SheepShaver mouse capture on") == "Mouse capture: on");
        assert(host.mouse_grabs == 2);
        host.send("Key down: 69");
        host.send("Mouse position (pixels) NAN NAN");
        host.send("Key down: 69\nKey up: 69");
        assert(host.mouse_captured && host.mouse_grabs == 3 && host.mouse_releases == 2);
        host.send("SheepShaver mouse capture off");
        assert(!host.mouse_captured);
        host.send("Text input 1 lines\nSheepShaver mouse capture on");
        assert(!host.mouse_captured && host.mouse_grabs == 3);
        assert(host.send("Documentation").find("SDLwindow") != std::string::npos);
        // Validate monitor configuration before workers can consume the immutable response table
        assert(host.send("Documentation").find("RGBA float linear") != std::string::npos);
        // Validate the optional persistent identity override and its default guest-controlled mode
        assert(host.send("Documentation").find("gamma table <guest|identity; default guest>") != std::string::npos);
        for (const char *mode : {"identity", "guest"})
            assert(host.send(std::string("SheepShaver gamma table ") + mode).empty());
        for (const char *mode : {"", "off", "identity extra", "guestx"})
            assert(host.send(std::string("SheepShaver gamma table ") + mode).find("Error:") == 0);
        for (const char *gamma : {"1.0", "1.8", "2.2", "3.0"})
            assert(host.send(std::string("SheepShaver monitor gamma ") + gamma).empty());
        for (const char *gamma : {"0", "-1", "3.1", "nan", "inf", "", "2.2x", "2.2 extra"})
            assert(host.send(std::string("SheepShaver monitor gamma ") + gamma).find("Error:") == 0);
        assert(host.send("Text input 1 lines\nDeinit\nStatus") == "State: configured");
        assert(host.send("SheepShaver RAM 1").find("Error:") == 0);
        // Require configuration errors to reach the console even when the host discards replies
        assert(host.last_log.find("Error:") != std::string::npos);
        assert(host.send("SheepShaver ROM path invalid.rom").empty());
        // Release persistent capture when shutdown follows an asynchronous startup failure
        host.send("SheepShaver mouse capture on");
        host.send("Init");
        std::string status;
        for (int i = 0; i < 500; ++i) {
            status = host.send("Status");
            if (status.find("failed") != std::string::npos) break;
            Sleep(10);
        }
        assert(status.find("failed") != std::string::npos);
        host.send("Stop threads");
        assert(!host.mouse_captured);
        // Require asynchronous boot failures to be visible without a separate Status query
        assert(host.last_log.find("Invalid or unreadable Macintosh ROM") != std::string::npos);
        // Reject monitor changes after initialization to keep CPU snapshots free of configuration races
        assert(host.send("SheepShaver monitor gamma 1.8").find("Error:") == 0);
        // Keep gamma-table policy immutable while the CPU may use guest or cached correction data
        assert(host.send("SheepShaver gamma table identity").find("Error:") == 0);
        assert(host.files.empty() && host.time_queries && host.reads);
        assert(!host.draw(nullptr) && !host.sound(nullptr));
    }
    puts("PASS: exports, callback setup, invalid configuration, asynchronous startup failure, cleanup and unload");
    if (argc < 4) return 0;

    // Boot the actual core using existing read-only fixtures and copy-on-write disk callbacks
    Host host(argv[1]);
    host.rom = argv[2];
    host.disk = argv[3];
    host.bind();
    assert(host.send("SheepShaver ROM path test.rom\nSheepShaver disk path test.hfv\nSheepShaver RAM 64\nSheepShaver resolution 640x480").empty());
    // Boot a configured 2.2 monitor while the real-provider integration check exercises the 1.8 default
    assert(host.send("SheepShaver monitor gamma 2.2").empty());
    // Allow an optional real-ROM run to exercise guest boot attempts under the identity override
    if (argc >= 5 && std::string(argv[4]) == "identity")
        assert(host.send("SheepShaver gamma table identity\nSheepShaver monitor gamma 1.8").empty());
    host.send("Init");
    // Exercise capture callbacks concurrently with real CPU, disk, frame and audio work
    assert(host.send("SheepShaver mouse capture on") == "Mouse capture: on");
    std::atomic<bool> audio_stop{false};
    std::atomic<size_t> audio_calls{0};
    std::thread audio_worker([&] {
        // Pull sound on a different host callback thread while preserving descriptor ownership
        while (!audio_stop.load()) {
            char *sound = host.sound(nullptr);
            if (sound) {
                size_t bytes = 0, address = 0;
                const char *location = strstr(sound, "Sound location:");
                assert(location && sscanf(location, "Sound location: %zu bytes at %zx", &bytes, &address) == 2);
                assert(bytes == 512 * 2 * sizeof(float));
                for (size_t p = 0; p < bytes / sizeof(float); ++p) assert(std::isfinite(((float *)address)[p]));
                audio_calls.fetch_add(1);
            }
            Sleep(10);
        }
    });
    bool image_seen = false, nonblank_seen = false;
    uint64_t frames_seen = 0;
    int last_width = 0, last_height = 0;
    for (int i = 0; i < 3000; ++i) {
        std::string status = host.send("Status");
        if (status.find("failed") != std::string::npos) {
            fprintf(stderr, "%s\n%s\n", status.c_str(), host.last_log.c_str());
            audio_stop.store(true);
            audio_worker.join();
            return 1;
        }
        char *frame = host.draw(nullptr);
        if (frame) {
            size_t bytes = 0, address = 0;
            const char *location = strstr(frame, "Framebuffer location:");
            assert(location && sscanf(location, "Framebuffer location: %zu bytes at %zx", &bytes, &address) == 2);
            assert(strstr(frame, "Pixel format: RGBA float linear\n"));
            // Accept guest-selected resolutions now that Mac OS can restore or switch its display mode
            int width = 0, height = 0;
            const char *resolution = strstr(frame, "Framebuffer resolution ");
            assert(resolution && sscanf(resolution, "Framebuffer resolution %dx%d", &width, &height) == 2);
            assert((width == 640 && height == 480) || (width == 800 && height == 600) ||
                (width == 832 && height == 624) || (width == 1024 && height == 768));
            assert(bytes == size_t(width) * height * 4 * sizeof(float));
            if (width != last_width || height != last_height) {
                // Record guest-driven mode changes while the real CPU and host audio callbacks run
                printf("Guest framebuffer: %dx%d\n", width, height);
                last_width = width; last_height = height;
            }
            float *pixels = (float *)address;
            size_t components = bytes / sizeof(float);
            for (size_t p = 0; p < components; ++p) {
                // Verify the public descriptor's payload contains finite linear RGB and opaque alpha
                assert(std::isfinite(pixels[p]) && pixels[p] >= 0 && pixels[p] <= 1);
                if (p % 4 == 3) assert(pixels[p] == 1);
            }
            for (size_t p = 4; p + 3 < components; p += 4)
                if (memcmp(pixels, pixels + p, 3 * sizeof(float))) { nonblank_seen = true; break; }
            image_seen = true;
            ++frames_seen;
        }
        if (i == 50) host.send("Mouse position (pixels) 123 234\nKey down: 4\nKey up: 4\nMouse left button down\nMouse left button up\nRelease input\nText input 1 lines\nDeinit");
        if (i % 500 == 0) printf("Boot poll %d: %s; frames=%llu\n", i, status.c_str(), (unsigned long long)frames_seen);
        if (nonblank_seen && i >= 1000) break;
        Sleep(10);
    }
    assert(image_seen && nonblank_seen && frames_seen > 10);
    host.send("Stop threads");
    assert(!host.mouse_captured);
    audio_stop.store(true);
    audio_worker.join();
    assert(audio_calls.load() > 10);
    assert(host.files.empty() && host.reads > 10);
    printf("PASS: actual Macintosh ROM, nonblank RGBA frames, audio callbacks, input, disk I/O, joined shutdown (%zu reads, %zu writes)\n", host.reads, host.writes);
}
