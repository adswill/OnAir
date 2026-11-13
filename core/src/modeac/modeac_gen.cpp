// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C test signal generator (see modeac_gen.h).
#include "dect2/modeac/modeac_gen.h"
#include "dect2/modeac/modeac_proto.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <random>

namespace dect2 {

using namespace modeac;

struct ModeAcGenerator::Impl {
    ModeAcGenConfig cfg;
    AdsbAirspace air;
    AdsbMixer mix;
    AdsbNoise noise;
    std::mt19937_64 rng;
    double horizon = 0;
    std::map<uint32_t, double> nextAt;       // per aircraft: when the next interrogation reaches it
    std::map<uint32_t, bool> nextC;          // and whether it is a Mode C one
    std::vector<ModeAcTrue> sent;
    std::vector<AdsbTx> tx;

    static AdsbAirspaceConfig airCfg(const ModeAcGenConfig& c) {
        AdsbAirspaceConfig a;
        a.aircraft = c.aircraft;
        a.seed = c.seed ? c.seed : 1;
        a.refLat = c.refLat; a.refLon = c.refLon;
        a.replies = c.modeS;
        a.emergencyDemo = c.special == 1;
        return a;
    }
    explicit Impl(const ModeAcGenConfig& c)
        : cfg(c), air(airCfg(c)), mix(c.rate, c.filterMHz), noise(c.rate, c.filterMHz, c.snrDb, 0.1, (uint64_t)(c.seed ? c.seed : 1) * 31 + 778), rng(c.seed * 977 + 5) {}

    double uni() { return std::uniform_real_distribution<double>(0, 1)(rng); }

    static std::vector<std::pair<double, double>> pulsesOf(uint16_t bits, bool spi) {
        std::vector<std::pair<double, double>> p;
        auto add = [&](int slot) { p.push_back({slot * kSlotUs, slot * kSlotUs + kPulseUs}); };
        add(0);
        for (int s = 1; s <= 13; s++) if (slotBit(bits, s)) add(s);
        add(kSlotF2);
        if (spi) add(kSlotSpi);
        return p;
    }

    void run(double until) {
        tx.clear();
        air.run(until + 0.05, tx);
        if (!cfg.modeS) tx.clear();     // the aircraft still fly
        const double lo = horizon;
        const double hi = until + 0.05;
        const auto planes = air.aircraft();
        size_t idx = 0;
        for (const auto& pl : planes) {
            idx++;
            auto it = nextAt.find(pl.icao);
            if (it == nextAt.end()) { it = nextAt.emplace(pl.icao, lo + uni() * 0.5).first; nextC[pl.icao] = uni() < 0.5; }
            const double d = std::max(pl.distNm, 12.0);
            while (it->second < hi) {
                if (it->second >= lo) {
                    const bool isC = nextC[pl.icao] && !pl.ground;
                    AdsbTx t;
                    t.t = it->second;
                    ModeAcTrue tr;
                    tr.t = t.t; tr.isC = isC; tr.icao = pl.icao;
                    tr.bits = isC ? bitsOfAltitude((int)pl.altFt) : bitsOfSquawk(pl.squawk);
                    tr.spi = !isC && cfg.special == 2 && idx == 2 && std::fmod(t.t, 60.0) < 18.0;
                    t.pulses = pulsesOf(tr.bits, tr.spi);
                    t.amp = (float)std::min(0.5, 0.1 * (50.0 / d));
                    t.cfoHz = cfg.cfoHz + (double)((pl.icao * 2654435761u) % 20001u) - 10000.0;
                    t.phase = (float)(uni() * 2 * M_PI);
                    tr.amp = t.amp;
                    sent.push_back(tr);
                    mix.add(t);
                }
                // interrogations come about twice a second together, alternating A and C with some mixing
                const double mean = 0.5 / std::max(0.05, cfg.replyRate);
                it->second += -mean * std::log(std::max(uni(), 1e-9));
                nextC[pl.icao] = uni() < 0.5 ? !nextC[pl.icao] : nextC[pl.icao];
            }
        }
        for (AdsbTx& t : tx) { t.cfoHz += cfg.cfoHz; mix.add(t); }
        horizon = hi;
    }
};

ModeAcGenerator::ModeAcGenerator(const ModeAcGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
ModeAcGenerator::~ModeAcGenerator() = default;
const std::vector<ModeAcTrue>& ModeAcGenerator::sent() const { return p_->sent; }
std::vector<ModeAcGenerator::Plane> ModeAcGenerator::planes() const {
    std::vector<Plane> v;
    for (const auto& i : p_->air.aircraft()) v.push_back({i.icao, i.callsign, i.squawk, i.altFt, i.ground});
    return v;
}

void ModeAcGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double until = m.mix.time() + (double)n / m.cfg.rate + 200e-6;
    if (until > m.horizon) m.run(until);
    m.noise.fill(out, n);
    m.mix.render(out, n, false);
    float* o = reinterpret_cast<float*>(out);
    for (size_t i = 0; i < 2 * n; i++) o[i] = std::clamp(o[i], -0.95f, 0.95f);
}

namespace {
class ModeAcSynth : public ModeSynth {
public:
    ModeAcSynth(const SynthConfig& c, double rate) : rate_(rate), gen_(config(c, rate)) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
private:
    static ModeAcGenConfig config(const SynthConfig& c, double rate) {
        ModeAcGenConfig g;
        g.rate = rate;
        g.snrDb = c.snrDb;
        g.cfoHz = c.cfoHz;
        g.filterMHz = c.modeVal[1];
        g.aircraft = c.modeOpt[0] > 0 ? std::min(c.modeOpt[0], 300) : (c.modeOpt[0] < 0 ? 0 : 12);
        g.modeS = c.modeOpt[1] == 0;
        g.seed = c.modeOpt[2] > 0 ? (uint32_t)c.modeOpt[2] : 1;
        g.special = c.modeOpt[3];
        g.replyRate = c.modeVal[0] > 0 ? c.modeVal[0] : 1.0;
        if (c.modeVal[2] != 0 || c.modeVal[3] != 0) { g.refLat = c.modeVal[2]; g.refLon = c.modeVal[3]; }
        return g;
    }
    double rate_;
    ModeAcGenerator gen_;
};
}  // namespace

std::unique_ptr<ModeSynth> makeModeAcSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 2e6 - 1) return nullptr;
    return std::make_unique<ModeAcSynth>(cfg, sampleRate);
}

} // namespace dect2
