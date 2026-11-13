// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM test signal: see flarm_gen.h.
#include "dect2/flarm/flarm_gen.h"
#include "dect2/flarm/flarm_proto.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kChipRate = flarm::kChipRate;

std::vector<uint8_t> chipsOf(const FlarmTx& tx) {
    uint8_t pay[flarm::kPayloadBytes];
    if (!tx.raw.empty()) { for (int i = 0; i < flarm::kPayloadBytes; i++) pay[i] = i < (int)tx.raw.size() ? tx.raw[i] : 0; }
    else flarm::encodeV6(tx.pkt, tx.keyTime, pay);
    std::vector<uint8_t> chips;
    for (int i = 0; i < 16; i++) chips.push_back((uint8_t)(i & 1));      // preamble 0101..., then the sync word and the frame
    std::vector<uint8_t> f = flarm::frameToChips(pay, true);
    if (tx.badCrc) { const size_t o = 64 + 40; if (o + 1 < f.size()) { f[o] ^= 1; f[o + 1] ^= 1; } }
    chips.insert(chips.end(), f.begin(), f.end());
    return chips;
}
}

std::vector<cf32> flarmBurst(const FlarmTx& tx, double rate, double clockPpm) {
    const std::vector<uint8_t> chips = chipsOf(tx);
    const double tchip = (1.0 + clockPpm * 1e-6) / kChipRate;
    const double sigma = 0.2650 * tchip;                      // Gaussian BT 0.5
    const size_t n = (size_t)((double)(chips.size() + 6) * tchip * rate);
    std::vector<float> fr(n, 0.f);
    // chip levels (+-1), the first and last chip held for 3 chips each side
    const int lead = 3;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate / tchip - lead;       // in chips
        const int c = std::max(0, std::min((int)chips.size() - 1, (int)std::floor(t)));
        fr[i] = chips[(size_t)c] ? 1.f : -1.f;
    }
    // Gaussian smoothing
    const int half = (int)std::ceil(3.0 * sigma * rate);
    std::vector<float> g((size_t)(2 * half + 1));
    float gs = 0;
    for (int k = -half; k <= half; k++) { const double x = (double)k / rate / sigma; g[(size_t)(k + half)] = (float)std::exp(-0.5 * x * x); gs += g[(size_t)(k + half)]; }
    for (float& v : g) v /= gs;
    std::vector<cf32> out(n);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        double s = 0;
        for (int k = -half; k <= half; k++) {
            const long j = (long)i + k;
            const float v = j < 0 ? fr[0] : j >= (long)n ? fr[n - 1] : fr[(size_t)j];
            s += g[(size_t)(k + half)] * v;
        }
        ph += 2 * kPi * (50000.0 * s + tx.cfoHz) / rate;
        out[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    // a short ramp at both ends
    const size_t ramp = std::min<size_t>(n / 2, (size_t)(rate * 4e-6) + 1);
    for (size_t i = 0; i < ramp; i++) { const float a = (float)i / (float)ramp; out[i] *= a; out[n - 1 - i] *= a; }
    const float amp = (float)std::pow(10.0, tx.gainDb / 20.0);
    for (auto& v : out) v *= amp;
    return out;
}

std::vector<cf32> flarmScene(double rate, double dur, const std::vector<FlarmTx>& txs, double snrDb, double clockPpm, unsigned seed) {
    const size_t N = (size_t)(dur * rate);
    std::vector<cf32> out(N, cf32(0.f, 0.f));
    for (const FlarmTx& tx : txs) {
        const std::vector<cf32> b = flarmBurst(tx, rate, clockPpm);
        const size_t s0 = (size_t)(tx.timeSec * rate);
        const double w = 2 * kPi * (tx.channel ? 100000.0 : -100000.0) / rate;
        for (size_t i = 0; i < b.size() && s0 + i < N; i++) {
            const double a = w * (double)(s0 + i);
            out[s0 + i] += b[i] * cf32((float)std::cos(a), (float)std::sin(a));
        }
    }
    if (snrDb > -100) {
        const double sigma = std::sqrt(rate / 200000.0 / std::pow(10.0, snrDb / 10.0) / 2.0);
        std::mt19937 rng(seed);
        genutil::PortableNormal nd((float)sigma);
        for (auto& v : out) { const float re = nd(rng); v += cf32(re, nd(rng)); }
    }
    return out;
}

std::vector<FlarmDemoAircraft> flarmDemoAircraft() {
    return {
        {0xDD1234, flarm::AddrFlarm, 1, false, false, false},     // glider
        {0xDDA5B6, flarm::AddrFlarm, 2, false, false, false},     // tow plane
        {0x3F7C11, flarm::AddrIcao, 8, false, false, false},      // powered aircraft with an ICAO address
        {0xDD9999, flarm::AddrFlarm, 1, true, false, false},      // no-track flag
        {0xDD8888, flarm::AddrFlarm, 7, false, true, false},      // stealth flag
        {0xDD7777, flarm::AddrFlarm, 1, false, false, true},      // the newer protocol
    };
}

namespace {

class FlarmSynth : public ModeSynth {
public:
    FlarmSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfo_(cfg.cfoHz), rng_(5), nd_(1.f) {
        lat0_ = cfg.modeVal[0] == 0 && cfg.modeVal[1] == 0 ? 54.70 : cfg.modeVal[0];
        lon0_ = cfg.modeVal[0] == 0 && cfg.modeVal[1] == 0 ? 25.30 : cfg.modeVal[1];
        unix0_ = (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
        const double snr = cfg.snrDb;
        const double sig1 = snr < -100 ? 0.0 : std::sqrt(rate / 200000.0 / std::pow(10.0, snr / 10.0) / 2.0);
        amp_ = sig1 > 0.4 ? (float)(0.4 / sig1) : 0.5f;
        sigma_ = (float)(sig1 * amp_);
        nd_ = genutil::PortableNormal(sigma_);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const int64_t end = pos_ + (int64_t)n;
        while (scheduledTo_ < end + (int64_t)(rate_ * 0.5)) scheduleSecond();
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        for (auto it = bursts_.begin(); it != bursts_.end();) {
            const int64_t s0 = it->start, s1 = it->start + (int64_t)it->s.size();
            if (s1 <= pos_) { it = bursts_.erase(it); continue; }
            const int64_t a = std::max(s0, pos_), b = std::min(s1, end);
            const double w = 2 * kPi * (it->ch ? 100000.0 : -100000.0) / rate_;
            for (int64_t k = a; k < b; k++) {
                const double ph = w * (double)k;
                out[(size_t)(k - pos_)] += it->s[(size_t)(k - s0)] * amp_ * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            ++it;
        }
        if (sigma_ > 0.f) for (size_t i = 0; i < n; i++) { const float re = nd_(rng_); out[i] += cf32(re, nd_(rng_)); }
        pos_ = end;
    }
private:
    struct Burst { int64_t start; int ch; std::vector<cf32> s; };
    void scheduleSecond() {
        const double sec0 = (double)scheduledTo_ / rate_;
        const std::vector<FlarmDemoAircraft> ac = flarmDemoAircraft();
        for (size_t i = 0; i < ac.size(); i++) {
            const double t = sec0 + 0.05 + 0.15 * (double)i + 0.04 * (double)(rng_() & 15) / 16.0;
            FlarmTx tx;
            tx.timeSec = t; tx.channel = (int)((i + (size_t)sec0) & 1);
            tx.cfoHz = cfo_;
            const uint32_t ts = (uint32_t)(unix0_ + t);
            tx.keyTime = ts;
            if (ac[i].newProtocol) {
                tx.raw.resize(flarm::kPayloadBytes);
                for (auto& b : tx.raw) b = (uint8_t)rng_();
                tx.raw[0] = (uint8_t)(ac[i].id); tx.raw[1] = (uint8_t)(ac[i].id >> 8); tx.raw[2] = (uint8_t)(ac[i].id >> 16); tx.raw[3] = 0x22;
            } else {
                const double r = 3000.0 + 1500.0 * (double)i, speed = i == 2 ? 38.0 : 24.0 + 2.0 * (double)i;
                const double om = speed / r, th = om * (unix0_ - 1.7e9 + t) * 1.0 + (double)i;
                const double dn = r * std::cos(th), de = r * std::sin(th);
                const double lat = lat0_ + dn / 111320.0, lon = lon0_ + de / (111320.0 * std::cos(lat0_ * kPi / 180.0));
                flarm::V6& p = tx.pkt;
                p.addr = ac[i].id; p.addrType = ac[i].addrType; p.aircraftType = ac[i].acType;
                p.noTrack = ac[i].noTrack; p.stealth = ac[i].stealth;
                flarm::packPosition(lat, lon, p.lat19, p.lon20);
                p.alt = (uint32_t)(900 + 120 * i + 100 * std::sin(0.1 * t));
                flarm::packVelocity(speed, std::fmod((th + kPi / 2) * 180.0 / kPi, 360.0) < 0 ? 0 : std::fmod((th + kPi / 2) * 180.0 / kPi, 360.0), i == 1 ? 3.2 : -0.6 + 0.4 * (double)i, p);
            }
            Burst b; b.start = (int64_t)(t * rate_); b.ch = tx.channel; b.s = flarmBurst(tx, rate_);
            bursts_.push_back(std::move(b));
        }
        scheduledTo_ += (int64_t)rate_;
    }
    double rate_, cfo_, lat0_, lon0_, unix0_;
    std::mt19937 rng_;
    genutil::PortableNormal nd_;
    float amp_ = 0.5f, sigma_ = 0.f;
    int64_t pos_ = 0, scheduledTo_ = 0;
    std::deque<Burst> bursts_;
};

} // namespace

std::unique_ptr<ModeSynth> makeFlarmSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < flarmTuning().minSampleRate - 1) return nullptr;
    return std::make_unique<FlarmSynth>(cfg, sampleRate);
}

} // namespace dect2
