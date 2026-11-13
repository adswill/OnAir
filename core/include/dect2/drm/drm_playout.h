// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM playout: from the decoded audio super frames to a steady 48 kHz stream for the sound device, without dropouts.
//
//   decoded super frames (every 400 ms, mode E 200 ms, with placeholders where audio frames were lost)
//     -> queue (one super frame plus a margin of 250 ms)   emptied at exactly 48000 frames per second of signal time
//     -> concealment                                      lost frames and an empty queue are filled from the last sound: its pitch period repeated
//                                                         (overlap-add at the joins), full level for 20 ms, then faded out over 200 ms; real sound
//                                                         comes back through a 20 ms crossfade, so there is no click
//     -> clock drift                                      a resampler (windowed sinc) at a ratio that keeps the sound device's queue at its target:
//                                                         the radio's sample clock and the sound card's never run alike
//     -> sound device
//
// After the queue ran empty (a fade, the signal lost) the next super frame is placed so that the queue is back at its nominal level: the latency
// stays the same whatever was lost. Everything runs on the receiver thread (feed()); nothing blocks.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace dect2 { namespace drm {

class Playout {
public:
    static constexpr int kRate = 48000;
    static constexpr double kMarginSecs = 0.25;          // queue left when the next super frame is due
    static constexpr int kDeviceStartFrames = 7200;      // the sound device starts with 150 ms queued (AudioOut::setStartThreshold)
    static constexpr int kDeviceTargetFrames = 9600;     // and is kept at 200 ms
    static constexpr int kDeviceFlushFrames = 72000;     // more than 1.5 s queued in the device: something went wrong, flush it

    Playout();
    ~Playout();
    void reset();                                        // a retune: forget everything, play nothing until the next super frame
    void setSuperFrameSecs(double s);                    // 0.4 (DRM30), 0.2 (DRM+)

    // One decoded audio super frame: interleaved stereo, `frames` frames; gaps are spans (first frame, frames) of placeholders for lost audio
    // frames (their samples are ignored).
    void decoded(const float* stereo, size_t frames, const std::vector<std::pair<size_t, size_t>>& gaps);
    // The signal has advanced to sigSecs (seconds since the start of the stream): appends the sound for that time to out (interleaved stereo).
    // deviceQueued: frames the sound device has queued (-1: no device, the ratio stays 1).
    void advance(double sigSecs, int deviceQueued, std::vector<float>& out);

    bool started() const;
    // statistics since the start
    double concealedSecs() const;      // made up (lost frames, empty queue) at a level above silence
    double mutedSecs() const;          // made up and faded to silence (long losses)
    double realSecs() const;           // decoded sound played
    uint64_t events() const;           // concealments started
    double trimmedSecs() const;        // sound dropped because the queue was too full
    double queueSecs() const;          // frames in the queue
    double ratio() const;              // output frames per frame of the queue (1 + clock drift)

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}} // namespace dect2::drm
