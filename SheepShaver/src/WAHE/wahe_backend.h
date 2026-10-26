/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHEEPSHAVER_WAHE_BACKEND_H
#define SHEEPSHAVER_WAHE_BACKEND_H
#include "sysdeps.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct WaheStop {};
extern std::atomic<bool> wahe_stopping, wahe_running, wahe_paused;
extern std::atomic<uint64> wahe_clock_usec;
extern uint64 wahe_clock_origin, wahe_epoch;
extern int wahe_width, wahe_height, wahe_sample_rate;
extern double wahe_monitor_gamma;
extern bool wahe_force_identity_gamma;
extern std::string wahe_error, wahe_nvram_path;
extern std::mutex wahe_error_mutex;

char *wahe_request(const std::string &command);
void wahe_log(const std::string &message) noexcept;
void wahe_fail(const std::string &message);
void wahe_check_stop();
void wahe_wake();
void wahe_input(const std::string &line, bool allow_relative = true);
void wahe_dispatch_input();
bool wahe_frame(std::vector<float> &pixels, int &width, int &height);
void wahe_audio_pull(float *output, size_t frames);
bool wahe_audio_needed();
uint64 wahe_host_time();

struct WaheFile { size_t handle; uint64 position; bool read_only; };
WaheFile *wahe_file_open(const std::string &path, const char *mode);
void wahe_file_close(WaheFile *file);
size_t wahe_file_read(WaheFile *file, void *buffer, uint64 offset, size_t bytes);
size_t wahe_file_write(WaheFile *file, const void *buffer, uint64 offset, size_t bytes);
uint64 wahe_file_size(WaheFile *file);
void wahe_start();
void wahe_stop();
std::string wahe_status();
void wahe_configure(const std::string &line);
#endif
