// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Optional sound cues (docs/SOUND-CUES.md): short tones when a decoder locks, loses lock, the scanner finds a signal, or a satellite pass ends,
// so a session can be followed by ear with the screen off. Off by default. Pure logic: the Tracker turns state readings into cues (with
// debounce and a minimum gap), tune() makes the tones; the app plays them. No audio device, no globals.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace cues {

enum class Cue { Lock, LostLock, SignalFound, PassEnded };

struct Settings {
    bool lock = false, lost = false, found = false, passEnd = false;   // every cue off by default
    int volume = 50;                                                   // 0..100 percent of the tone's own level
    bool anyOn() const { return lock || lost || found || passEnd; }
};

struct Input { bool locked = false; uint64_t foundCount = 0; bool passActive = false; };   // foundCount: a counter that rises with every signal found

class Tracker {
public:
    Settings& settings() { return s_; }
    const Settings& settings() const { return s_; }
    // Call about every frame with the current readings and a steady clock in seconds. Returns the cues to play now (usually none).
    // A lock change counts once it has held for 1 s; "lost" needs a lock before it; signals found are at least 3 s apart (none is dropped,
    // a held-back one plays when the gap is over).
    std::vector<Cue> update(const Input& in, double now);
    void reset() { *this = Tracker(s_); }
    Tracker() = default;
    explicit Tracker(const Settings& s) : s_(s) {}
private:
    Settings s_;
    bool started_ = false;
    bool locked_ = false, everLocked_ = false, cand_ = false;
    double candSince_ = 0;
    uint64_t lastFound_ = 0;
    bool foundPending_ = false;
    double lastFoundCue_ = -1e9;
    bool pass_ = false;
};

// The tone of a cue: interleaved stereo float at `rate`, at most 0.6 s, peak 0.4, soft start and end (no click).
std::vector<float> tune(Cue c, int rate = 48000);

}  // namespace cues
}  // namespace dect2
