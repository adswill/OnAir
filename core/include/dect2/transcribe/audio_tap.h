// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A tap on the sound every mode plays (AudioOut::write() offers its frames here, as it does to the audio recorder, audio_rec.h): the live
// transcripts of the modes that have no channel of their own to read (marine, ham, FM, ...). The first output that writes owns the tap; others
// are ignored until it has been silent for 2 s. The sink gets mono floats at the output's rate; it must not block.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace dect2 {

class AudioTap {
public:
    using Sink = std::function<void(const float* mono, size_t n, int rate)>;
    static AudioTap& instance();
    void setSink(Sink s);                    // nullptr turns the tap off
    bool on() const { return on_.load(std::memory_order_relaxed); }
    // For AudioOut::write(): never blocks, allocates only when the block is larger than any before. Interleaved stereo.
    static void offer(const void* src, const float* stereo, int frames, int rate) {
        AudioTap& t = instance();
        if (t.on_.load(std::memory_order_relaxed)) t.push(src, stereo, frames, rate);
    }
private:
    void push(const void* src, const float* stereo, int frames, int rate);
    std::atomic<bool> on_{false};
    std::mutex mu_;
    Sink sink_;
    const void* owner_ = nullptr;
    int64_t lastMs_ = 0;
    std::vector<float> mono_;
};

}  // namespace dect2
