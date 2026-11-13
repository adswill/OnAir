// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Regression test for the sound rule of the whole-span banks (core/include/dect2/engine/wide_bank.h): with no carrier chosen the best locked carrier
// plays (it used to stay silent until a row was clicked); it keeps playing while it is as good as the best (no flapping between equal carriers);
// a better locked carrier takes over; a chosen carrier plays whatever the others do; clearing the choice goes back to the automatic one; mute silences all.
// With the voice hook (WideOps::voice) a carrier that carries voice wins over one that is only locked (a control channel never goes idle) and keeps
// the sound while its call goes on.
// A fake receiver stands in for the decoders: it records the sound flags the bank gives it and reports a state set by the test.
#include "dect2/engine/wide_bank.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct FakeTel { uint64_t seq = 0; int state = 0; bool voice = false; };
static std::vector<struct FakeRx*> gAll;
static int gState[4] = {0, 0, 0, 0};
static bool gVoice[4] = {false, false, false, false};

struct FakeRx {
    int idx;
    bool play = false;
    uint64_t seq = 0;
    FakeRx() : idx((int)gAll.size()) { gAll.push_back(this); }
    ~FakeRx() { for (auto& p : gAll) if (p == this) p = nullptr; }
    void configure(double) {}
    void feed(const cf32*, size_t) {}
    bool telemetry(FakeTel& t, uint64_t) { t.seq = ++seq; t.state = gState[idx < 4 ? idx : 3]; t.voice = gVoice[idx < 4 ? idx : 3]; return true; }
};

static int playing() {   // how many fake receivers have their sound on, and which (the last one)
    int n = 0, which = -1;
    for (FakeRx* r : gAll) if (r && r->play) { n++; which = r->idx; }
    return n == 1 ? which : (n == 0 ? -1 : -2);
}

int main() {
    const double fs = 1.2e6;
    WideOps<FakeRx, FakeTel> ops;
    ops.outRate = 48000; ops.occupiedHz = 20000;
    ops.audio = [](FakeRx& r, bool play) { r.play = play; };
    ops.voice = [](const FakeTel& t) { return t.voice; };
    WideBank<FakeRx, FakeTel> bank(ops);
    CarrierCfg cfg; cfg.stepHz = 12500; cfg.debounceSec = 0.1; cfg.holdSec = 8; cfg.avgSec = 0.05;
    bank.configure(fs, cfg);

    // two steady tones in the span, one 10 dB stronger
    const double f1 = 100e3, f2 = -150e3;
    std::vector<cf32> x(48000);
    double ph1 = 0, ph2 = 0;
    uint32_t rng = 12345;   // a fixed noise floor: with pure tones the floor is far below the window sidelobes and the finder reports them as carriers
    auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return ((float)(rng >> 8) / 16777216.f - 0.5f) * 0.01f; };
    for (int blk = 0; blk < 40; blk++) {
        for (auto& v : x) {
            ph1 += 2 * M_PI * f1 / fs; ph2 += 2 * M_PI * f2 / fs;
            v = cf32(0.1f * (float)std::cos(ph1) + 0.03f * (float)std::cos(ph2) + noise(), 0.1f * (float)std::sin(ph1) + 0.03f * (float)std::sin(ph2) + noise());
        }
        bank.feed(x.data(), x.size());
    }
    CHECK(bank.liveCount() == 2, "expected 2 carriers, found %zu", bank.liveCount());
    if (bank.liveCount() != 2) { printf("FAILED\n"); return 1; }

    FakeTel t; std::vector<WideCarrier> rows; uint64_t sq = 0;
    auto snap = [&]() { bank.snapshot(t, rows, 0, &sq); };
    auto selectedIdx = [&]() { for (const auto& r : rows) if (r.selected) return r.id; return -1; };

    // 1. nothing locked: no one is locked, so the strongest is the best; it plays (exactly one carrier is audible)
    gState[0] = gState[1] = 0;
    snap();
    CHECK(playing() >= 0, "no carrier plays with none chosen (silent until a row was clicked): %d", playing());

    // find which fake belongs to which row: lock instance 0 only and see which row becomes the best
    gState[0] = 2; gState[1] = 1;
    snap();
    CHECK(playing() == 0, "the locked carrier (instance 0) should play, got %d", playing());
    const int idOf0 = selectedIdx();
    int idOf1 = -1;
    for (const auto& r : rows) if (r.id != idOf0) idOf1 = r.id;

    // 2. equal state: the one that plays keeps playing (hysteresis)
    gState[0] = 2; gState[1] = 2;
    snap(); snap();
    CHECK(playing() == 0, "equal carriers: the playing one should stay, got %d", playing());

    // 3. the other one becomes better: the sound follows
    gState[0] = 1; gState[1] = 2;
    snap();
    CHECK(playing() == 1, "a better locked carrier should take over, got %d", playing());

    // 4. a chosen carrier plays whatever the states are
    bank.setSelected(idOf0);
    snap();
    CHECK(playing() == 0, "the chosen carrier should play, got %d", playing());
    gState[0] = 0; gState[1] = 2;
    snap();
    CHECK(playing() == 0, "the chosen carrier keeps playing though the other is better, got %d", playing());

    // 5. clearing the choice goes back to the best locked one
    bank.setSelected(0);
    snap();
    CHECK(playing() == 1, "none chosen again: the best locked carrier plays, got %d", playing());

    // 6. mute silences everything, unmute brings the sound back
    bank.setMuted(true);
    CHECK(playing() == -1, "muted: nothing should play, got %d", playing());
    bank.setMuted(false);
    snap();
    CHECK(playing() == 1, "unmuted: the sound is back, got %d", playing());
    (void)idOf1;

    // 7. a control channel: locked all the time, never voice. It plays while nothing talks, but a call on the other carrier takes the sound
    gState[0] = 2; gState[1] = 1; gVoice[0] = false; gVoice[1] = false;
    snap();   // instance 0 is the better one: it takes the sound
    gState[1] = 2;
    snap();   // now both are locked: 0 keeps the sound (hysteresis)
    CHECK(playing() == 0, "setup: the locked carrier 0 should play, got %d", playing());
    gVoice[1] = true;
    snap();
    CHECK(playing() == 1, "a carrier with voice should take the sound from a locked one without voice, got %d", playing());
    // 8. the call goes on although the other carrier is now as good or better: the sound stays with the call
    gState[1] = 1; gVoice[0] = false;
    snap();
    CHECK(playing() == 1, "the carrier with the call keeps the sound while the call goes on, got %d", playing());
    // 9. two calls: the one being played is not interrupted
    gState[1] = 2; gVoice[0] = true;
    snap();
    CHECK(playing() == 1, "a second call does not take the sound from the call being played, got %d", playing());
    // 10. the played call ends while the other goes on: the sound moves to the call
    gVoice[1] = false;
    snap();
    CHECK(playing() == 0, "when the played call ends the other call gets the sound, got %d", playing());
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
