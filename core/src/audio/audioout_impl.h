// Shared state of the audio output: the lock-free ring buffer and the playback bookkeeping. A platform backend (CoreAudio, miniaudio)
// calls render() from its real-time callback.
#pragma once
#include "dect2/audio/audioout.h"
#include "dect2/audio/audiolevel.h"
#include "dect2/audio/audio_cleanup.h"
#include "dect2/audio/audio_control.h"
#include "dect2/audio/voice_inversion.h"
#include <atomic>
#include <string>
#include <vector>
#include <cstdint>

namespace dect2 {

struct AudioOut::Impl {
    std::string device;               // chosen output device by name, "" = default (set before the backend starts)
    void* backend = nullptr;          // owned by the backend implementation
    void* backendAux = nullptr;       // CoreAudio: the rate-conversion state of the render callback
    int backendKind = 0;              // 1 our own backend, 2 the miniaudio fallback (Windows, Linux)
    int channels = 2;                 // per frame in `ring` and in render()'s output (2, 6 or 8)
    std::vector<float> ring;          // interleaved, `channels` per frame
    size_t frames = 0;                // ring capacity in frames (power of two)
    std::atomic<uint64_t> w{0}, r{0}; // frame counters
    std::atomic<uint64_t> written{0}, played{0};
    std::atomic<float> volume{1.f};   // the slider position 0..1; volumeTaper() makes the gain
    AudioLeveler leveler;             // render thread only
    AudioCleanup cleanup;             // the writer's thread (write()), prepared in start(); follows AudioCleanupConfig::global()
    float cleanBuf[AudioCleanup::kMaxChunk * 2];
    AudioControl control;             // after the cleanup (write() thread): filters, EQ, notch, AGC, squelch; follows AudioControlConfig::global()
    float ctlBuf[AudioControl::kMaxChunk * 2];
    VoiceInversion vinv;              // before the cleanup chain (write() thread); off by default, follows VoiceInversionConfig::global()
    float invBuf[VoiceInversion::kMaxChunk * 2];
    std::atomic<bool> muted{false};
    std::atomic<int> underruns{0};
    std::atomic<int> startThreshold{14400};
    std::atomic<bool> playing{false};
    std::atomic<bool> flushReq{false};
    // bitstream passthrough: IEC 61937 words queued by writeIec(), played by the bitstream thread (audioout.cpp) in step with the PCM clock
    void* bitstream = nullptr;
    std::vector<int16_t> iec;         // interleaved word pairs
    size_t iecFrames = 0;             // capacity, a power of two
    std::atomic<uint64_t> iw{0}, ir{0};
    std::atomic<bool> iecFlush{false};
    // Fills `out` (interleaved float, `channels` per frame) with `nFrames` frames: queued audio, or silence while (re)buffering.
    void render(float* out, uint32_t nFrames);
};

std::vector<std::string> audioListOutputs();   // the platform backend's output device names
bool audioBackendStart(AudioOut::Impl* impl, int sampleRate);   // false: no device; opens impl->channels channels
int audioDeviceChannels(const std::string& name);              // the platform backend's speaker count of a device, 0 unknown
bool audioMiniaudioStart(AudioOut::Impl* impl, int sampleRate);   // the miniaudio fallback (Windows, Linux)
void audioMiniaudioStop(AudioOut::Impl* impl);
void audioBackendStop(AudioOut::Impl* impl);

} // namespace dect2
