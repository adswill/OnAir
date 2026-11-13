// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Radio clips": a rolling memory of the last seconds of the sound you hear (mono, 16-bit, at the output's rate), so a clip can be made after the
// interesting thing happened. AudioOut::write() offers every block; the audio thread only copies samples into a pre-allocated ring (no lock, no
// allocation). Nothing is allocated until a block has been seen and the UI thread asked for the ring (an unused audio output costs nothing).
// Time: the ring is contiguous up to "now"; a pause of audio longer than a second starts it over, so a clip never has a hole.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

class AudioClipRing {
public:
    static AudioClipRing& instance();
    // For AudioOut::write(): never blocks, never allocates. src identifies the output (the first one that writes owns the ring).
    static void offer(const void* src, const float* stereo, int frames, int rate) {
        AudioClipRing& r = instance();
        r.seen_.store(true, std::memory_order_relaxed);
        if (r.on_.load(std::memory_order_acquire)) r.push(src, stereo, frames, rate);
    }
    // UI thread: seconds <= 0 switches the ring off and frees it; otherwise it holds that many seconds at up to 48 kHz (more rate: fewer seconds).
    void configure(double seconds);
    bool audioSeen() const { return seen_.load(std::memory_order_relaxed); }   // a block has been offered since start
    bool enabled() const { return on_.load(std::memory_order_relaxed); }
    size_t bytes() const;                       // memory held now
    double availableSeconds() const;            // audio in the ring now
    int rate() const;                           // of the audio in the ring (0: none yet)
    // Any thread: the audio from `back0` to `back1` seconds before now (back0 >= back1 >= 0), as mono 16-bit. Does not stop the audio thread; a block
    // overwritten while copying is dropped (the copy then ends earlier). False when there is nothing in that range.
    bool snapshot(double back0, double back1, std::vector<int16_t>& out, int& rate) const;
    AudioClipRing();
    ~AudioClipRing();

private:
    void push(const void* src, const float* stereo, int frames, int rate);
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<bool> on_{false}, seen_{false};
};

}  // namespace dect2
