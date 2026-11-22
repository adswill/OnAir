// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/audio/sound_cues.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace cues {

std::vector<Cue> Tracker::update(const Input& in, double now) {
    std::vector<Cue> out;
    if (!started_) {   // the first reading is the baseline: no cue for what was already so
        started_ = true;
        locked_ = cand_ = in.locked; everLocked_ = in.locked; candSince_ = now;
        lastFound_ = in.foundCount; pass_ = in.passActive;
        return out;
    }
    // lock: a change counts after it has held for a second
    if (in.locked != cand_) { cand_ = in.locked; candSince_ = now; }
    if (cand_ != locked_ && now - candSince_ >= 1.0) {
        locked_ = cand_;
        if (locked_) { everLocked_ = true; if (s_.lock) out.push_back(Cue::Lock); }
        else if (everLocked_ && s_.lost) out.push_back(Cue::LostLock);
    }
    // signals found: the counter rises
    if (in.foundCount > lastFound_) foundPending_ = true;
    lastFound_ = in.foundCount;
    if (foundPending_ && now - lastFoundCue_ >= 3.0) {
        foundPending_ = false;
        if (s_.found) { out.push_back(Cue::SignalFound); lastFoundCue_ = now; }
    }
    if (!s_.found) foundPending_ = false;
    // pass ended
    if (pass_ && !in.passActive && s_.passEnd) out.push_back(Cue::PassEnded);
    pass_ = in.passActive;
    return out;
}

std::vector<float> tune(Cue c, int rate) {
    struct Note { float hz; int ms; };   // hz 0: silence
    std::vector<Note> notes;
    switch (c) {
    case Cue::Lock:        notes = {{660, 90}, {880, 130}}; break;
    case Cue::LostLock:    notes = {{660, 90}, {440, 150}}; break;
    case Cue::SignalFound: notes = {{1320, 70}, {0, 50}, {1320, 70}}; break;
    case Cue::PassEnded:   notes = {{880, 100}, {660, 100}, {440, 160}}; break;
    }
    std::vector<float> pcm;
    double phase = 0;
    for (const Note& n : notes) {
        const int len = rate * n.ms / 1000, ramp = std::max(1, rate / 200);   // 5 ms ramps
        for (int i = 0; i < len; i++) {
            const float env = n.hz > 0 ? std::min(1.f, (float)std::min(i, len - i) / (float)ramp) : 0.f;
            const float v = 0.4f * env * (float)std::sin(phase);
            phase += 2.0 * 3.14159265358979 * n.hz / rate;
            pcm.push_back(v); pcm.push_back(v);
        }
        if (phase > 1e6) phase = std::fmod(phase, 2 * 3.14159265358979);
    }
    return pcm;
}

}  // namespace cues
}  // namespace dect2
