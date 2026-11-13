// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/uplink/uplink_gen.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979;

// a pulse with half amplitude at s and s + w (us), 50 ns raised-cosine edges
inline float pe(double t, double s, double w) {
    const double e = 0.025;
    if (t <= s - e || t >= s + w + e) return 0.f;
    if (t < s + e) return (float)(0.5 - 0.5 * std::cos(kPi * (t - s + e) / (2 * e)));
    if (t > s + w - e) return (float)(0.5 - 0.5 * std::cos(kPi * (s + w + e - t) / (2 * e)));
    return 1.f;
}
} // namespace

UplinkGenerator::UplinkGenerator(double rate, double noiseRms, double cfoHz, uint32_t seed) : rate_(rate), noise_(noiseRms), cfo_(cfoHz), rng_(seed) {}

void UplinkGenerator::add(const UplinkTx& t) {
    Ev e;
    e.tx = t;
    e.t0 = t.sec - 1e-6;
    e.a = std::pow(10.f, t.levelDb / 20.f);
    e.a2 = t.p2Db > -90 ? std::pow(10.f, t.p2Db / 20.f) : 0.f;
    e.a5 = std::pow(10.f, t.p5Db / 20.f);
    if (t.kind == 0) e.t1 = t.sec + ((t.acMode == 'C' ? 21 : 8) + 2 + 1.6 + 0.2) * 1e-6;
    else {
        e.t1 = t.sec + (3.5 + (t.frame.nbits == 56 ? 16.25 : 30.25) + 0.2) * 1e-6;
        e.flips.push_back(3.5 + 1.25);
        for (int k = 1; k <= t.frame.nbits; k++)
            if (t.frame.b[(k - 1) / 8] & (0x80 >> ((k - 1) % 8))) e.flips.push_back(3.5 + 1.25 + 0.25 * (k + 1));
    }
    ev_.push_back(std::move(e));
}

void UplinkGenerator::generate(cf32* out, size_t n) {
    const double tA = pos_ / rate_, tB = (pos_ + n) / rate_;
    std::vector<const Ev*> act;
    for (const Ev& e : ev_) if (e.t1 > tA && e.t0 < tB) act.push_back(&e);
    for (size_t i = 0; i < n; i++) {
        const double t = (pos_ + i) / rate_;
        cf32 s;
        for (const Ev* e : act) {
            if (t < e->t0 || t > e->t1) continue;
            const UplinkTx& x = e->tx;
            const double tau = (t - x.sec) * 1e6;
            const float a = e->a;
            const double ph = x.phase + 2 * kPi * (cfo_ + x.cfoHz) * t;
            if (x.kind == 0) {
                float v = pe(tau, 0, 0.8);
                const double d3 = x.acMode == 'C' ? 21 : 8;
                v += pe(tau, d3, 0.8);
                if (x.p4) v += pe(tau, d3 + 2, x.p4 == 2 ? 1.6 : 0.8);
                if (e->a2 > 0) v += pe(tau, 2, 0.8) * e->a2;
                s += std::polar(a * v, (float)ph);
            } else {
                const float v = pe(tau, 0, 0.8) + pe(tau, 2, 0.8);
                const float v6 = pe(tau, 3.5, x.frame.nbits == 56 ? 16.25 : 30.25);
                int f = 0;
                for (double ft : e->flips) { if (tau >= ft - 1e-6) f++; else break; }   // a reversal on a sample instant: always the same side, whatever the rounding
                s += std::polar(a * v, (float)ph) + std::polar(a * v6, (float)(ph + kPi * (f & 1)));
                if (x.p5) s += std::polar(a * e->a5 * pe(tau, 3.5 + 1.25 - 0.4, 0.8), (float)(ph + 1.7));
            }
        }
        const cf32 y = 0.25f * h2_ + 0.5f * h1_ + 0.25f * s;
        h2_ = h1_; h1_ = s;
        // a transmitter asks for no noise (rms about 1e-10): the two Gaussian draws per sample are then not made at all
        if (noise_ < 1e-8) out[i] = y;
        else out[i] = y + cf32(nd_(rng_), nd_(rng_)) * (float)(noise_ * 0.70710678);
    }
    pos_ += n;
    ev_.erase(std::remove_if(ev_.begin(), ev_.end(), [&](const Ev& e) { return e.t1 < tB; }), ev_.end());
}

// ------------------------------------------------------------------ the test scene

namespace {
void periodic(double t0, double t1, double period, double offset, std::vector<double>& out) {
    for (double k = std::ceil((t0 - offset) / period); k * period + offset < t1; k++) out.push_back(k * period + offset);
}
const uint32_t kNear[] = {0x4CA2B1, 0x3C6586, 0x471F02};
}

void uplinkScene(double t0, double t1, std::vector<UplinkTx>& out, float mainDb) {
    using namespace uplink;
    std::vector<double> ts;
    uint32_t ph = 7;
    auto phase = [&](double t) { return (float)std::fmod(t * 1e5 + (ph += 13), 6.283); };
    // radar 1: II 3, 4.8 s, 40 ms dwell at 150 Hz
    for (double scan = std::floor(t0 / 4.8) - 1; scan * 4.8 < t1 + 1; scan++) {
        const double c = scan * 4.8 + 0.5;
        for (int k = -3; k <= 3; k++) {
            const double t = c + k / 150.0;
            if (t < t0 || t >= t1) continue;
            UplinkTx x; x.sec = t; x.levelDb = mainDb - 1.5f * std::fabs((float)k); x.phase = phase(t);
            const int slot = (k + 3) % 3;
            if (slot == 0) { x.kind = 1; x.p2Db = 0; x.frame = uf11(0, 3, 0); }
            else if (slot == 1) { x.kind = 0; x.acMode = (k & 2) ? 'C' : 'A'; x.p4 = 2; x.p2Db = -12; }
            else {
                x.kind = 1; x.p2Db = 0;
                const uint32_t a = kNear[(unsigned)(scan + k + 3) % 3];
                const int sel = (int)((unsigned)(scan * 7 + k + 3) % 4);
                const int bds1 = sel == 0 ? 2 : sel == 1 ? 4 : sel == 2 ? 5 : 6;
                const uint8_t ma[7] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE};
                if (sel == 0) x.frame = uf45(4, a, 0, 16 + bds1, 7, sdIis(3, 0));
                else if (sel == 1) x.frame = uf45(5, a, 0, 16 + bds1, 7, sdIis(3, 0));
                else if (sel == 2) x.frame = uf2021(20, a, 0, 16 + bds1, 7, sdIis(3, 0), ma);
                else x.frame = uf2021(21, a, 0, 16 + bds1, 7, sdIis(3, 0), ma);
            }
            out.push_back(x);
        }
        // roll-calls to aircraft elsewhere, through a sidelobe
        const uint32_t farBy[] = {0x40621D, 0x506C3F};
        for (int j = 0; j < 2; j++) {
            const double t = c + 4.8 * (j ? 250 : 120) / 360.0;
            if (t < t0 || t >= t1) continue;
            UplinkTx x; x.sec = t; x.kind = 1; x.p2Db = 0; x.levelDb = mainDb - 18; x.phase = phase(t);
            x.frame = uf45(4, farBy[j], 0, 0, 0, sdIis(3, 0));
            out.push_back(x);
        }
    }
    // radar 2: SI 17, 10 s, 60 ms dwell at 100 Hz, DI 3 roll-calls, its own 15 kHz offset
    for (double scan = std::floor(t0 / 10) - 1; scan * 10 < t1 + 1; scan++) {
        const double c = scan * 10 + 2.3;
        for (int k = -3; k <= 3; k++) {
            const double t = c + k / 100.0;
            if (t < t0 || t >= t1) continue;
            UplinkTx x; x.sec = t; x.kind = 1; x.p2Db = 0; x.levelDb = mainDb - 4 - 1.5f * std::fabs((float)k); x.cfoHz = 15e3; x.phase = phase(t);
            if (k % 2 == 0) x.frame = uf11(1, 1, 2);
            else if (k == -1) x.frame = uf45(5, kNear[0], 0, 16 + 6, 3, sdSis(17, 0, 0));
            else { const uint8_t ma[7] = {}; x.frame = uf2021(20, kNear[1], 0, 16 + 5, 3, sdSis(17, 0, 0), ma); }
            out.push_back(x);
        }
    }
    // Mode A/C SSR: 6 s, 50 ms dwell at 300 Hz, sidelobe interrogations around it
    for (double scan = std::floor(t0 / 6) - 1; scan * 6 < t1 + 1; scan++) {
        const double c = scan * 6 + 1.1;
        for (int k = -7; k <= 7; k++) {
            const double t = c + k / 300.0;
            if (t < t0 || t >= t1) continue;
            UplinkTx x; x.sec = t; x.kind = 0; x.acMode = (k & 1) ? 'C' : 'A'; x.p2Db = -12; x.levelDb = mainDb - 2; x.phase = phase(t);
            out.push_back(x);
        }
        for (int k = -20; k <= 20; k++) {
            if (std::abs(k) < 3) continue;
            const double t = c + k * 0.02 + 0.001;
            if (t < t0 || t >= t1) continue;
            UplinkTx x; x.sec = t; x.kind = 0; x.acMode = 'A'; x.p2Db = 6; x.levelDb = mainDb - 16; x.phase = phase(t);
            out.push_back(x);
        }
    }
    // ACAS aircraft 4CA2B1
    ts.clear(); periodic(t0, t1, 1.0, 0.37, ts);
    for (double t : ts) { UplinkTx x; x.sec = t; x.kind = 1; x.p2Db = 0; x.levelDb = mainDb - 8; x.cfoHz = -40e3; x.phase = phase(t); x.frame = uf0(0x3C6586, 0, 1, 0); out.push_back(x); }
    ts.clear(); periodic(t0, t1, 5.0, 2.71, ts);
    for (double t : ts) {
        UplinkTx x; x.sec = t; x.kind = 1; x.p2Db = 0; x.levelDb = mainDb - 8; x.cfoHz = -40e3; x.phase = phase(t);
        uint8_t mu[7];
        raMu(mu, 0x4CA2B1, 0, 0, ((int)(t / 5) & 1) ? 1 : 2, 0, 0, 0, 0);
        x.frame = uf16(0x3C6586, 0, 0, mu);
        out.push_back(x);
    }
}

namespace {
class UplinkSynth : public ModeSynth {
public:
    UplinkSynth(const SynthConfig& s, double rate) : gen_(rate, std::pow(10.0, (kMain - s.snrDb) / 20.0), s.cfoHz, 5) {}
    double sampleRate() const override { return gen_.rate(); }
    void generate(cf32* out, size_t n) override {
        const double until = gen_.timeSec() + n / gen_.rate() + 0.001;
        if (until > done_) {
            std::vector<UplinkTx> v;
            uplinkScene(done_, until + 0.05, v, (float)kMain);
            for (const auto& x : v) gen_.add(x);
            done_ = until + 0.05;
        }
        gen_.generate(out, n);
    }
private:
    static constexpr double kMain = -12;
    UplinkGenerator gen_;
    double done_ = 0;
};
} // namespace

std::unique_ptr<ModeSynth> makeUplinkSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<UplinkSynth>(cfg, sampleRate);
}

} // namespace dect2
