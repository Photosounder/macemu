/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stdio_impl.h"
#include <stdlib.h>
#include <string.h>
#include <wahe_imports.h>

static _Thread_local unsigned char console_marker;
int __lockfile(FILE *stream)
{
    // Identify console ownership through the WAHE runner's private TLS instead of a pthread descriptor
    int token = (int)(uintptr_t)&console_marker;
    if (__atomic_load_n(&stream->lock, __ATOMIC_RELAXED) == token) return 0;
    int expected = 0;
    while (!__atomic_compare_exchange_n(&stream->lock, &expected, token, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) expected = 0;
    return 1;
}
void __unlockfile(FILE *stream)
{
    // Publish completed formatting before another runner writes to the console stream
    __atomic_store_n(&stream->lock, 0, __ATOMIC_RELEASE);
}

static void print_bytes(const unsigned char *bytes, size_t length)
{
    // Forward bounded runtime console chunks without importing file descriptors or a process console
    while (length) {
        char command[2048];
        size_t count = length < sizeof(command) - 7 ? length : sizeof(command) - 7;
        memcpy(command, "Print ", 6);
        memcpy(command + 6, bytes, count);
        command[count + 6] = 0;
        free(wahe_run_command(command));
        bytes += count;
        length -= count;
    }
}
size_t __stdio_write(FILE *stream, const unsigned char *bytes, size_t length)
{
    // Flush musl's formatting buffer through the host before acknowledging its new output bytes
    print_bytes(stream->wbase, stream->wpos - stream->wbase);
    print_bytes(bytes, length);
    stream->wend = stream->buf + stream->buf_size;
    stream->wpos = stream->wbase = stream->buf;
    return length;
}
size_t __stdout_write(FILE *stream, const unsigned char *bytes, size_t length)
{
    // Avoid terminal probing when the C++ runtime first writes a diagnostic
    stream->write = __stdio_write;
    return __stdio_write(stream, bytes, length);
}
int __stdio_close(FILE *stream) { /* Console streams own no host file descriptors */ (void)stream; return 0; }
off_t __stdio_seek(FILE *stream, off_t offset, int origin)
{
    // Reject console seeks locally while emulator disk offsets use the WAHE file provider
    (void)stream; (void)offset; (void)origin;
    errno = ESPIPE;
    return -1;
}
size_t __stdio_read(FILE *stream, unsigned char *bytes, size_t length)
{
    // Receive user input through module messages rather than reading a process stdin
    (void)stream; (void)bytes; (void)length;
    return 0;
}
