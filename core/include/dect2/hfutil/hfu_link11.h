// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Link 11 CLEW (the conventional parallel-tone waveform): message detector and frame-structure reader, and a test generator.
// The normative document (MIL-STD-188-203-1A) is not public, so this follows the published summaries (Wavecom's Link 11 notes, sigidwiki):
// a 605 Hz Doppler tone that is never modulated, 14 data tones from 935 Hz spaced 110 Hz and a data / sync tone at 2915 Hz (15 data tones,
// 2 bits each by differential QPSK = 30 bits per frame), fast frames of 13.33 ms (75 per second) and slow frames of 22 ms (45.45 per second),
// a preamble of the Doppler and sync tones alone, then a phase reference frame and the data frames.
// What is read: the start and end of each message, its frame rate, the Doppler offset, the number of frames, and each frame's 15 phase
// changes as raw quarter-turn numbers (0..3). The mapping of phase changes to bits, the Hamming (30, 24) layout, the control and address
// codes are not public, so no bits, words or addresses are shown. Link 11 traffic is normally encrypted anyway. Receive only.
// SLEW (the single-tone 2400 Bd variant) is not covered. Input: complex baseband at 24 kHz, audio tones at positive frequencies (USB).
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfu {

constexpr double kL11Doppler = 605.0, kL11Sync = 2915.0;
inline double l11DataTone(int k) { return k < 14 ? 935.0 + 110.0 * k : kL11Sync; }   // k = 0..14

struct Link11Msg {
    bool fast = true;                 // 75 frames/s (13.33 ms) or 45.45 (22 ms)
    double dopplerHz = 0;             // the 605 Hz tone's offset
    int preambleFrames = 0;
    int frames = 0;                   // data frames after the phase reference
    double startSec = 0, durationSec = 0;
    double snrDb = 0;
    double offsetDeg = 0;             // where the phase changes sit (0 or 45 degrees off the axes)
    std::vector<std::array<uint8_t, 15>> phases;   // per frame and tone: the phase change in quarter turns (0..3), raw
};

class Link11Rx {
public:
    Link11Rx();
    ~Link11Rx();
    void reset();
    void push(const cf32* x, size_t n);
    void setMessageSink(std::function<void(const Link11Msg&)> cb);   // called at the end of each message
    void setEventSink(std::function<void(const std::string&)> cb);
    int state() const;                // 0 searching, 1 preamble, 2 frames
    uint64_t messages() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct Link11GenCfg {
    bool fast = true;
    int preambleFrames = 5;
    double offsetDeg = 45;            // the phase-change constellation: 45 for +-45 / +-135 degrees, 0 for the axes
    double leadSec = 0.1, tailSec = 0.1;
};
// a message of the given phase changes (each 0..3); 24 kHz complex, no noise; nothing is transmitted
std::vector<cf32> genLink11(const Link11GenCfg& c, const std::vector<std::array<uint8_t, 15>>& frames);

}  // namespace dect2::hfu
