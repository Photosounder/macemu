/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "main.h"
#include "audio.h"
#include "audio_defs.h"
#include "cpu_emulation.h"
#include "prefs.h"
#include <deque>

static WaheMutex audio_mutex;
static std::deque<float> audio_queue;
static std::atomic<bool> requested{false};
static bool main_muted, speaker_muted;
static uint32 main_volume = 0x01000100, speaker_volume = 0x01000100;

void AudioInit()
{
    // Advertise the common Mac sound component without opening a native audio device
    audio_sample_rates = {(uint32)wahe_sample_rate << 16};
    audio_sample_sizes = {8, 16};
    audio_channel_counts = {1, 2};
    AudioStatus.sample_rate = audio_sample_rates[0];
    AudioStatus.sample_size = 16;
    AudioStatus.channels = 2;
    AudioStatus.mixer = 0;
    AudioStatus.num_sources = 0;
    audio_frames_per_block = 1024;
    audio_component_flags = cmpWantsRegisterMessage | kStereoOut | k16BitOut;
    audio_open = !PrefsFindBool("nosound");
}
void AudioExit() { /* Release queued samples after joining the CPU */ std::lock_guard<WaheMutex> lock(audio_mutex); audio_queue.clear(); audio_open = false; }
void audio_enter_stream() { /* The host controls when audio is requested */ }
void audio_exit_stream() { /* Stop publishing samples when Mac OS removes its last source */ }

bool wahe_audio_needed()
{
    // Ask the CPU for another mixer block only after a consumer has requested audio
    return requested.exchange(false);
}

void wahe_audio_pull(float *output, size_t frames)
{
    // Return queued stereo samples immediately and use silence for underruns
    std::lock_guard<WaheMutex> lock(audio_mutex);
    for (size_t i = 0; i < frames * 2; ++i) {
        output[i] = audio_queue.empty() ? 0.f : audio_queue.front();
        if (!audio_queue.empty()) audio_queue.pop_front();
    }
    if (audio_queue.size() < 8192) requested.store(true);
}

void AudioInterrupt()
{
    // Execute Apple's mixer only on the emulation worker that owns Mac memory and registers
    if (!audio_open || !AudioStatus.num_sources || !AudioStatus.mixer) return;
    M68kRegisters registers{};
    registers.a[0] = audio_data + adatStreamInfo;
    registers.a[1] = AudioStatus.mixer;
    Execute68k(audio_data + adatGetSourceData, &registers);
    uint32 info = ReadMacInt32(audio_data + adatStreamInfo);
    if (!info) return;
    uint32 frames = ReadMacInt32(info + scd_sampleCount);
    uint16 channels = ReadMacInt16(info + scd_numChannels), bits = ReadMacInt16(info + scd_sampleSize);
    uint32 address = ReadMacInt32(info + scd_buffer);
    if (!address || frames > 65536 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16)) return;

    // Convert big-endian guest samples into bounded interleaved float storage
    std::lock_guard<WaheMutex> lock(audio_mutex);
    const uint8 *source = Mac2HostAddr(address);
    for (uint32 frame = 0; frame < frames && audio_queue.size() + 2 <= 131072; ++frame) for (int channel = 0; channel < 2; ++channel) {
        size_t index = (size_t)frame * channels + (channels == 1 ? 0 : channel);
        float sample = bits == 8 ? (source[index] - 128) / 128.f :
            (int16)((source[index * 2] << 8) | source[index * 2 + 1]) / 32768.f;
        uint32 shift = channel == 0 ? 16 : 0;
        float gain = ((main_volume >> shift) & 0xffff) / 256.f * ((speaker_volume >> shift) & 0xffff) / 256.f;
        audio_queue.push_back(main_muted || speaker_muted ? 0.f : sample * gain);
    }
}

bool audio_set_sample_rate(int index) { /* Only the configured host sample rate is exposed */ return index == 0; }
bool audio_set_sample_size(int index) { /* Apply Mac format changes on the CPU thread */ if (index < 0 || index > 1) return false; AudioStatus.sample_size = audio_sample_sizes[index]; return true; }
bool audio_set_channels(int index) { /* Apply Mac format changes on the CPU thread */ if (index < 0 || index > 1) return false; AudioStatus.channels = audio_channel_counts[index]; return true; }
bool audio_get_main_mute() { /* Return the emulated mixer setting */ return main_muted; }
uint32 audio_get_main_volume() { /* Return the emulated mixer setting */ return main_volume; }
bool audio_get_speaker_mute() { /* Return the emulated mixer setting */ return speaker_muted; }
uint32 audio_get_speaker_volume() { /* Return the emulated mixer setting */ return speaker_volume; }
void audio_set_main_mute(bool value) { /* Set emulated output muting */ main_muted = value; }
void audio_set_main_volume(uint32 value) { /* Set emulated per-channel output gain */ main_volume = value; }
void audio_set_speaker_mute(bool value) { /* Set emulated speaker muting */ speaker_muted = value; }
void audio_set_speaker_volume(uint32 value) { /* Set emulated per-channel speaker gain */ speaker_volume = value; }
void PlayStartupSound() { /* Startup audio is produced by the guest's mixer */ }
