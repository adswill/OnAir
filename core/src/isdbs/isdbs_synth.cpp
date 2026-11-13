// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The ISDB-S test signal for the built-in synthetic source, see isdbs_rx.h.
#include "dect2/isdbs/isdbs_rx.h"
#include "dect2/tv/demo_ts.h"
#include <memory>

namespace dect2 {

namespace {
class IsdbsSynth : public ModeSynth {
public:
    IsdbsSynth(const isdbs::GenConfig& c) : gen_(c), rate_(c.sampleRate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
private:
    isdbs::Generator gen_;
    double rate_;
};
}  // namespace

std::unique_ptr<ModeSynth> makeIsdbsSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 39.0e6) return nullptr;
    using namespace isdbs;
    GenConfig g;
    g.sampleRate = sampleRate;
    g.snrDb = cfg.snrDb;
    g.cfoHz = cfg.cfoHz;
    g.clockPpm = cfg.sroPpm + cfg.modeVal[1];
    if (cfg.modeVal[0] > 1e6) g.symbolRate = cfg.modeVal[0];
    const int layout = cfg.modeOpt[0];
    if (layout == 1 || layout == 2) {
        StreamCfg s;
        s.mod = layout == 1 ? kTc8psk23 : kQpsk12; s.slots = kSlots; s.tsId = 0x4010;
        s.src = demoTsSource(slotTsBitrate(s.mod) * s.slots * 0.9);
        g.streams.push_back(s);
    } else {
        StreamCfg a, b, c;
        a.mod = kTc8psk23; a.slots = 36;   // all 48 slots are allocated: 36 + 8 + 4 a.tsId = 0x4010; a.src = demoTsSource(slotTsBitrate(a.mod) * a.slots * 0.9);
        b.mod = kQpsk34; b.slots = 8; b.tsId = 0x4011;
        c.mod = kBpsk12; c.slots = 4; c.tsId = 0x4012;
        g.streams = {a, b, c};
    }
    return std::make_unique<IsdbsSynth>(g);
}

}  // namespace dect2
