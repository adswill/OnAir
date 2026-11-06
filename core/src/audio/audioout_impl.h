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
    std::vector<float> ring;          // interleaved stereo
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
    // Fills `out` (interleaved stereo float) with `nFrames` frames: queued audio, or silence while (re)buffering.
    void render(float* out, uint32_t nFrames);
};

std::vector<std::string> audioListOutputs();   // the platform backend's output device names
bool audioBackendStart(AudioOut::Impl* impl, int sampleRate);   // false: no device
bool audioMiniaudioStart(AudioOut::Impl* impl, int sampleRate);   // the miniaudio fallback (Windows, Linux)
void audioMiniaudioStop(AudioOut::Impl* impl);
void audioBackendStop(AudioOut::Impl* impl);

} // namespace dect2
