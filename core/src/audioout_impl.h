// Shared state of the audio output: the lock-free ring buffer and the playback bookkeeping. A platform backend (CoreAudio, miniaudio)
// calls render() from its real-time callback.
#pragma once
#include "dect2/audioout.h"
#include <atomic>
#include <vector>
#include <cstdint>

namespace dect2 {

struct AudioOut::Impl {
    void* backend = nullptr;          // owned by the backend implementation
    int backendKind = 0;              // 1 our own backend, 2 the miniaudio fallback (Windows, Linux)
    std::vector<float> ring;          // interleaved stereo
    size_t frames = 0;                // ring capacity in frames (power of two)
    std::atomic<uint64_t> w{0}, r{0}; // frame counters
    std::atomic<uint64_t> written{0}, played{0};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false};
    std::atomic<int> underruns{0};
    std::atomic<int> startThreshold{14400};
    std::atomic<bool> playing{false};
    std::atomic<bool> flushReq{false};
    // Fills `out` (interleaved stereo float) with `nFrames` frames: queued audio, or silence while (re)buffering.
    void render(float* out, uint32_t nFrames);
};

bool audioBackendStart(AudioOut::Impl* impl, int sampleRate);   // false: no device
bool audioMiniaudioStart(AudioOut::Impl* impl, int sampleRate);   // the miniaudio fallback (Windows, Linux)
void audioMiniaudioStop(AudioOut::Impl* impl);
void audioBackendStop(AudioOut::Impl* impl);

} // namespace dect2
