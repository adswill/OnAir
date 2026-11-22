// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Optional sound cues: off by default, the four tones differ and stay short and quiet, state changes give one cue each, no chatter.
#include "dect2/audio/sound_cues.h"
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

using namespace dect2::cues;

static std::vector<Cue> run(Tracker& t, bool lock, uint64_t hits, bool pass, double now) { return t.update({lock, hits, pass}, now); }

int main() {
    // defaults_off: nothing enabled, nothing fires
    {
        Tracker t;
        CHECK(!t.settings().anyOn());
        for (double s = 0; s < 20; s += 0.5) CHECK(run(t, s > 3, (uint64_t)s, s > 10, s).empty());
    }
    // tones_distinct_and_bounded
    {
        const Cue all[] = {Cue::Lock, Cue::LostLock, Cue::SignalFound, Cue::PassEnded};
        std::vector<std::vector<float>> tone;
        for (Cue c : all) {
            tone.push_back(tune(c, 48000));
            const auto& v = tone.back();
            CHECK(v.size() % 2 == 0 && !v.empty());
            CHECK(v.size() / 2 <= 48000 * 6 / 10);   // at most 0.6 s
            float pk = 0; for (float x : v) pk = std::max(pk, std::fabs(x));
            CHECK(pk > 0.1f && pk <= 0.6f);
            CHECK(std::fabs(v.front()) < 0.01f && std::fabs(v.back()) < 0.01f);   // no click at the ends
        }
        for (size_t i = 0; i < tone.size(); i++) for (size_t j = i + 1; j < tone.size(); j++) CHECK(tone[i] != tone[j]);
    }
    // lock_edge_debounced + lost_lock_only_after_lock
    {
        Tracker t; t.settings().lock = t.settings().lost = true;
        CHECK(run(t, false, 0, false, 0).empty());
        CHECK(run(t, false, 0, false, 5).empty());          // never locked: no "lost"
        CHECK(run(t, true, 0, false, 6).empty());           // locked, not stable yet
        auto c = run(t, true, 0, false, 7.1);
        CHECK(c.size() == 1 && c[0] == Cue::Lock);
        CHECK(run(t, true, 0, false, 8).empty());           // once
        CHECK(run(t, false, 0, false, 9).empty());          // lost, not stable yet
        c = run(t, false, 0, false, 10.1);
        CHECK(c.size() == 1 && c[0] == Cue::LostLock);
        CHECK(run(t, false, 0, false, 12).empty());
    }
    // flicker gives no cue
    {
        Tracker t; t.settings().lock = t.settings().lost = true;
        bool l = false; int n = 0;
        for (double s = 0; s < 30; s += 0.3) { l = !l; n += (int)run(t, l, 0, false, s).size(); }
        CHECK(n == 0);
    }
    // signal found: once per new hit, rate limited
    {
        Tracker t; t.settings().found = true;
        CHECK(run(t, false, 5, false, 0).empty());          // the first reading only sets the baseline
        auto c = run(t, false, 6, false, 1);
        CHECK(c.size() == 1 && c[0] == Cue::SignalFound);
        CHECK(run(t, false, 7, false, 1.5).empty());        // within the minimum gap
        CHECK(run(t, false, 7, false, 5).size() == 1);      // the hit that was held back is not lost
        CHECK(run(t, false, 7, false, 9).empty());
    }
    // pass ended
    {
        Tracker t; t.settings().passEnd = true;
        CHECK(run(t, false, 0, true, 0).empty());
        CHECK(run(t, false, 0, true, 100).empty());
        auto c = run(t, false, 0, false, 101);
        CHECK(c.size() == 1 && c[0] == Cue::PassEnded);
        CHECK(run(t, false, 0, false, 102).empty());
    }
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
