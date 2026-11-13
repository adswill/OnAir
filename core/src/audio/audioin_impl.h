// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include "dect2/audio/audioin.h"
#include "dect2/dsp/resampler.h"
#include <atomic>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct AudioFileStream;   // a long audio file decoded as it plays (audioin.cpp)

struct AudioIn::Impl {
    void* backend = nullptr;
    void* ctx = nullptr;              // a backend's own state
    std::atomic<uint64_t> renderErrors{0};
    std::vector<float> ring;          // interleaved stereo
    size_t frames = 0;
    std::atomic<uint64_t> w{0}, r{0};
    std::atomic<float> peak{0};
    std::atomic<uint64_t> overrun{0};
    std::atomic<float> gain{1.f};
    std::atomic<bool> muted{false};
    std::vector<float> scratch;
    std::vector<float> file;          // looped file, interleaved stereo at the session rate
    size_t filePos = 0;
    std::shared_ptr<AudioFileStream> stream;   // a file longer than the in-memory limit: decoded while it plays, looped (#471)
    std::vector<float> streamBuf;     // decoded and not yet read, interleaved stereo
    size_t streamPos = 0;             // samples (not frames) of streamBuf already read
    double toneHz = 1000, tph = 0;
    int actualRate = 0;               // set by a backend that cannot deliver the rate asked for (and did not resample to it)
    // A backend that has to capture at the device's own rate (CoreAudio's input unit does no rate conversion) gives that rate and resamples to the wanted one.
    bool setResample(double devRate, double wantRate);
    // Capture callback (real-time): nFrames of interleaved stereo float. A mono device is given by `chans` = 1 (copied to both channels).
    void push(const float* in, uint32_t nFrames, int chans = 2);
    void pushStereo(const float* in, uint32_t nFrames);
    bool useRs = false;
    RationalResampler rs;
    std::vector<cf32> rsIn, rsOut;
    std::vector<float> conv;          // push()'s stereo output when a mono feed or the resampler is in the way
};

bool audioInMicStart(AudioIn::Impl* impl, int sampleRate, const std::string& deviceId, std::string& err);
void audioInMicStop(AudioIn::Impl* impl);
std::vector<AudioInputDevice> audioInListMic();

} // namespace dect2
