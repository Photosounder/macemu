/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <wahelib/wahelib.h>
#include <wahelib/wahelib.c>

int main(int argc, char **argv)
{
    // Load the actual WAHE engine and file provider without window or audio modules
    if (argc != 2) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    static wahe_group_t group;
    buffer_t errors = {0};
    wahe_file_parse(&group, argv[1], &errors);
    if (errors.buf) {
        fprintf(stderr, "%s\n", errors.buf);
        return 1;
    }
    if (group.module_count != 2) return 1;
    wahe_group_init(&group);
    wahe_module_t *mac = &group.module[1];
    int nonblank_seen = 0, failed = 0;

    // Require real provider reads to boot far enough to produce a nonuniform guest image
    for (int i = 0; i < 12000 && !nonblank_seen; ++i) {
        char *status = wahe_send_input(mac, "Status");
        if (!status || strstr(status, "failed")) {
            fprintf(stderr, "%s\n", status ? status : "Missing Status reply");
            failed = 1;
            break;
        }
        char *frame = call_module_func(mac, 0, WAHE_FUNC_DRAW, 0);
        if (frame) {
            const char *location = strstr(frame, "Framebuffer location:");
            // Validate the active guest mode rather than assuming Mac OS keeps its startup resolution
            const char *resolution = strstr(frame, "Framebuffer resolution ");
            int width = 0, height = 0;
            int valid_resolution = resolution && sscanf(resolution, "Framebuffer resolution %dx%d", &width, &height) == 2 &&
                ((width == 640 && height == 480) || (width == 800 && height == 600) ||
                (width == 832 && height == 624) || (width == 1024 && height == 768));
            size_t bytes = 0, address = 0;
            if (!location || sscanf(location, "Framebuffer location: %zu bytes at %zx", &bytes, &address) != 2 ||
                !strstr(frame, "Pixel format: RGBA float linear\n") ||
                !valid_resolution || !address || bytes != (size_t)width * height * 4 * sizeof(float)) {
                failed = 1;
                break;
            }
            // Resolve Wasm offsets through the host's shared memory before inspecting the image
            const float *pixels = mac->type == WAHE_MODULE_NATIVE ? (const float *)address :
                (const float *)(mac->memory_ptr + address);
            size_t components = bytes / sizeof(float);
            // Check the actual host receives normalized linear float components and opaque alpha
            for (size_t p = 0; p < components; ++p)
                if (!isfinite(pixels[p]) || pixels[p] < 0 || pixels[p] > 1 || (p % 4 == 3 && pixels[p] != 1)) failed = 1;
            for (size_t p = 4; p + 3 < components; p += 4) {
                if (memcmp(pixels, pixels + p, 3 * sizeof(float))) {
                    nonblank_seen = 1;
                    break;
                }
            }
        }
        Sleep(10);
    }

    // Join the emulator before deinitializing the provider used by its cleanup callbacks
    wahe_send_input(mac, "Deinit");
    wahe_send_input(&group.module[0], "Deinit");
    if (failed || !nonblank_seen) {
        fprintf(stderr, "FAIL: real WAHE host did not receive a nonblank Macintosh framebuffer\n");
        return 1;
    }
    puts("PASS: real WAHE engine, OS_Basics ROM/disk reads, nonblank framebuffer and joined shutdown");
    return 0;
}
