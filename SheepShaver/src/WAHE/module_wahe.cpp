/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include <cerrno>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>
extern "C" {
#define WAHE_INCLUDE_IMPL
#include <wahe_imports.h>
}
#ifdef __wasm32__
static void *module_ctx;
static char *wasm_host_command(void *, char *command)
{
    // Call the sole Wasm host import through the same request interface as the native backend
    return wahe_run_command(command);
}
static auto wahe_run_command_with_id = wasm_host_command;
#endif

static WaheRecursiveMutex api_mutex, host_mutex;
static thread_local char reply[4096];
static std::vector<float> output_image;
static std::vector<float> output_audio;
static bool mouse_capture_enabled, capture_key_down;

char *wahe_request(const std::string &command)
{
    // Let Wasm runners make independent callbacks so joining never blocks the worker's cleanup requests
#ifndef __wasm32__
    // Serialize native host calls while allowing allocator callbacks to reenter the module
    std::lock_guard<WaheRecursiveMutex> lock(host_mutex);
#endif
    if (!wahe_run_command_with_id) return nullptr;
    return wahe_run_command_with_id(module_ctx, const_cast<char *>(command.c_str()));
}

void wahe_log(const std::string &text) noexcept
{
    // Send diagnostics to the host instead of a process-owned console or dialog
#ifdef __wasm32__
    free(wahe_request("Print SheepShaver: " + text));
#else
    try { free(wahe_request("Print SheepShaver: " + text)); }
    catch (...) { /* Keep diagnostics from escaping a failure or cleanup path */ }
#endif
}

uint64 wahe_host_time()
{
    // Obtain monotonic time only from the host callback and reject invalid responses
    char *response = wahe_request("Get raw time");
    double seconds = 0;
    bool valid = response && sscanf(response, "Raw time %lg", &seconds) == 1 &&
        std::isfinite(seconds) && seconds >= 0 && seconds < 1e12;
    free(response);
    if (!valid) {
        // Stop on an invalid clock response while preserving the last valid timestamp
#ifdef __wasm32__
        wahe_fail("Host did not provide Get raw time");
        return wahe_clock_usec.load();
#else
        throw std::runtime_error("Host did not provide Get raw time");
#endif
    }
    uint64 time = (uint64)(seconds * 1000000.0);
    if (time < wahe_clock_usec.load()) {
        // Prevent a clock regression from reaching guest timers
#ifdef __wasm32__
        wahe_fail("Host time moved backwards");
        return wahe_clock_usec.load();
#else
        throw std::runtime_error("Host time moved backwards");
#endif
    }
    return time;
}

extern "C" EXPORT("module_message_input") char *module_message_input(char *message)
{
    // Keep setup and lifecycle changes serialized independently from the emulation worker
    std::unique_lock<WaheRecursiveMutex> lock(api_mutex);
    reply[0] = 0;
    if (!message) return nullptr;
    // Defer native window commands until the module lock is released for host reentrancy
    int capture_request = -1;
    // Return rejected commands directly through the borrowed ABI reply
    auto reject = [&](const char *error) {
        // Keep validation failures local to the command being processed
        snprintf(reply, sizeof(reply), "Error: %s", error);
        return false;
    };
    auto set_capture = [&](bool enabled) {
        // Keep startup requests and runtime toggles idempotent while refusing capture after shutdown
        if (!wahe_run_command_with_id) return reject("Bind the native WAHE host callback before mouse capture");
        if (enabled && wahe_stopping) return reject("Cannot capture the mouse after shutdown");
        if (mouse_capture_enabled != enabled) capture_request = enabled;
        mouse_capture_enabled = enabled;
        snprintf(reply, sizeof(reply), "Mouse capture: %s", enabled ? "on" : "off");
        return true;
    };
#ifndef __wasm32__
    try
#endif
    {
        std::istringstream lines(message);
        std::string line;
        bool has_absolute_mouse = strstr(message, "Mouse position (pixels)") != nullptr;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            int text_lines = 0;
            if (sscanf(line.c_str(), "Text input %d lines", &text_lines) == 1) {
                // Consume SDLwindow text payloads as data rather than interpreting typed words as commands
                if (text_lines < 0 || text_lines > 65536) { reject("Invalid SDLwindow text input length"); break; }
                std::string text;
                for (int i = 0; i < text_lines && std::getline(lines, text); ++i) {}
                continue;
            }
            // Reserve F12 as a capture toggle without forwarding its press, release or repeat to Mac OS
            int scan = 0;
            char key_action[24];
            if (sscanf(line.c_str(), "Key %23[^:]: %d", key_action, &scan) == 2 && scan == 69) {
                if (!strcmp(key_action, "down")) {
                    // Toggle once per physical press even when a host repeats key-down messages
                    if (!capture_key_down && !set_capture(!mouse_capture_enabled)) break;
                    capture_key_down = true;
                } else if (!strcmp(key_action, "up")) capture_key_down = false;
                continue;
            }
            // Allow the toggle key to recover when focus changes suppress its release event
            double mouse_x = 0, mouse_y = 0;
            if (line == "Window focus lost" || line == "Release input" ||
                (sscanf(line.c_str(), "Mouse position (pixels) %lg %lg", &mouse_x, &mouse_y) == 2 &&
                 (!std::isfinite(mouse_x) || !std::isfinite(mouse_y)))) capture_key_down = false;
            size_t pointer = 0;
#ifndef __wasm32__
            if (sscanf(line.c_str(), "module_ctx = %zx", &pointer) == 1) {
                // Accept the native WAHE context before starting any callbacks on workers
                if (wahe_running || wahe_stopping) throw std::runtime_error("Host setup is immutable after Init");
                std::lock_guard<WaheRecursiveMutex> host_lock(host_mutex);
                module_ctx = reinterpret_cast<void *>(pointer);
            } else if (sscanf(line.c_str(), "wahe_run_command_with_id() = %zx", &pointer) == 1) {
                // Bind the ordinary native WAHE command callback without importing a host library
                if (wahe_running || wahe_stopping) throw std::runtime_error("Host setup is immutable after Init");
                std::lock_guard<WaheRecursiveMutex> host_lock(host_mutex);
                wahe_run_command_with_id = reinterpret_cast<char *(*)(void *, char *)>(pointer);
            } else
#endif
            if (line == "Init") {
                // Start the real CPU on a joinable worker and return control to the host
                if (!wahe_run_command_with_id) { reject("Bind the native WAHE host callback before Init"); break; }
                wahe_start();
                snprintf(reply, sizeof(reply), "%s", wahe_status().c_str());
            } else if (line == "Stop threads" || line == "Deinit" || line == "Window close requested") {
                // Return the host pointer when the user closes or stops the emulated machine
                if (mouse_capture_enabled && !set_capture(false)) break;
                capture_key_down = false;
                // Join workers before releasing callback-owned output storage or allowing DLL unload
                lock.unlock();
                wahe_stop();
                lock.lock();
                if (line == "Deinit" && !wahe_running.load()) {
                    // Retain borrowed output buffers through Stop threads until consumers are quiescent
                    std::vector<float>().swap(output_image);
                    std::vector<float>().swap(output_audio);
                }
                snprintf(reply, sizeof(reply), "%s", wahe_status().c_str());
            } else if (line == "SheepShaver mouse capture on" || line == "SheepShaver mouse capture off" ||
                       line == "SheepShaver mouse capture toggle") {
                // Route capture control through SDLwindow's registered commands on the host calling thread
                if (!set_capture(line == "SheepShaver mouse capture toggle" ? !mouse_capture_enabled :
                    line == "SheepShaver mouse capture on")) break;
            } else if (line == "SheepShaver mouse capture status") {
                // Report the requested capture state independently of temporary focus loss
                snprintf(reply, sizeof(reply), "Mouse capture: %s", mouse_capture_enabled ? "on" : "off");
            } else if (line == "Status") {
                // Report errors asynchronously encountered by the emulation worker
                snprintf(reply, sizeof(reply), "%s", wahe_status().c_str());
            } else if (line == "Documentation") {
                // Describe the native callback protocol and intentionally unavailable host devices
                snprintf(reply, sizeof(reply), "SheepShaver ROM path <path>\nSheepShaver disk path <path; prefix * for read-only>\n"
                    "SheepShaver RAM <MiB, 16..512>\nSheepShaver resolution <width>x<height>\n"
                    "SheepShaver monitor gamma <1.0..3.0; default 1.8>\n"
                    "SheepShaver gamma table <guest|identity; default guest>\n"
                    "SheepShaver mouse capture <on|off|toggle|status>\n"
                    "SheepShaver sample rate <Hz, 8000..48000>\nSheepShaver NVRAM path <path>\n"
                    "SheepShaver clock unix <seconds>\nSheepShaver preferences path <path>\n"
                    "Init\nStatus\nStop threads\nDeinit\n"
                    "SDLwindow mouse and keyboard messages are accepted directly.\n"
                    "F12 toggles mouse capture; captured movement uses relative ADB input.\n"
                    "module_draw: RGBA float linear. module_proc_sound: stereo float.\n"
                    "Raw devices, This PC/ExtFS, serial ports and networking are not exposed.\n"
                    "Deinit is terminal; reload the library to start another machine.");
            } else if (line.compare(0, 12, "SheepShaver ") == 0) {
                // Configure emulated devices before booting without reading native preferences
                if (const char *error = wahe_configure(line)) {
                    // Preserve the first invalid command and stop processing its remaining lines
                    reject(error);
                    break;
                }
            } else {
                // Deliver window events through the bounded CPU-owned input queue
                wahe_input(line, !has_absolute_mouse);
            }
        }
    }
#ifndef __wasm32__
    catch (const std::exception &error) {
        // Return configuration and provider errors through the callback instead of escaping the ABI
        snprintf(reply, sizeof(reply), "Error: %s", error.what());
    } catch (...) {
        // Keep the exported C ABI safe even if allocation or native initialization fails
        snprintf(reply, sizeof(reply), "Error: unexpected module exception");
    }
#endif
    // Preserve borrowed replies while the host may reenter this module during capture or logging callbacks
    char response[sizeof(reply)];
    memcpy(response, reply, sizeof(reply));
    lock.unlock();
#ifndef __wasm32__
    try
#endif
    {
        // Let SDLwindow own cursor confinement and relative input without adding SDL dependencies here
        if (capture_request >= 0) free(wahe_request(capture_request ? "Mouse grab" : "Mouse release"));
    }
#ifndef __wasm32__
    catch (...) {
        // Contain host callback failures at the exported C boundary
        snprintf(response, sizeof(response), "Error: mouse capture callback failed");
    }
#endif
    // Print rejected commands because archive launchers may discard their returned error messages
    if (strncmp(response, "Error:", 6) == 0) wahe_log(response);
    memcpy(reply, response, sizeof(reply));
    return reply[0] ? reply : nullptr;
}

#ifdef __wasm32__
extern "C" EXPORT("module_thread_entry") char *module_thread_entry(char *message)
{
    // Enter emulation only through the dedicated host-created worker callback
    if (message && !strcmp(message, "SheepShaver CPU runner")) wahe_wasm_run();
    return nullptr;
}
#endif

extern "C" EXPORT("module_draw") char *module_draw(char *message)
{
    // Process connected SDLwindow events and publish an independent stable frame snapshot
#ifndef __wasm32__
    try
#endif
    {
        if (message) module_message_input(message);
        std::lock_guard<WaheRecursiveMutex> lock(api_mutex);
        if (!wahe_running || wahe_stopping) return nullptr;
        int width, height;
        if (!wahe_frame(output_image, width, height)) return nullptr;
        snprintf(reply, sizeof(reply), "Pixel format: RGBA float linear\nFramebuffer location: %zu bytes at 0x%zx\n"
            "Framebuffer resolution %dx%d px", output_image.size() * sizeof(float), (size_t)output_image.data(), width, height);
        return reply;
    }
#ifndef __wasm32__
    catch (...) {
        // Report a recoverable drawing failure without throwing into the host
        return nullptr;
    }
#endif
}

extern "C" EXPORT("module_proc_sound") char *module_proc_sound(char *message)
{
    // Match the connected audio provider's request without opening a private audio device
#ifndef __wasm32__
    try
#endif
    {
        if (!wahe_running || wahe_stopping) return nullptr;
        char *specification = wahe_request("Get audio frame specs");
        std::istringstream lines(specification ? specification : message ? message : "");
        free(specification);

        // Acquire module state only after the host call so provider callbacks may reenter Status
        std::lock_guard<WaheRecursiveMutex> lock(api_mutex);
        if (!wahe_running || wahe_stopping) return nullptr;
        size_t frames = 1024;
        std::string line;
        while (std::getline(lines, line)) sscanf(line.c_str(), "Sample count: %zu", &frames);
        if (!frames || frames > 65536) return nullptr;
        output_audio.resize(frames * 2);
        wahe_audio_pull(output_audio.data(), frames);
        snprintf(reply, sizeof(reply), "Sound format: 32-bit float interleaved\nSample rate: %d\nSample count: %zu\n"
            "Channels: 2\nSound location: %zu bytes at 0x%zx", wahe_sample_rate, frames,
            output_audio.size() * sizeof(float), (size_t)output_audio.data());
        return reply;
    }
#ifndef __wasm32__
    catch (...) {
        // Preserve the callback boundary when an audio provider fails
        return nullptr;
    }
#endif
}
