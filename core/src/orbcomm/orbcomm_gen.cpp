// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Orbcomm test transmitter (orbcomm_gen.h).
#include "dect2/orbcomm/orbcomm_gen.h"
#include "dect2/dsp/portable_rng.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>

namespace dect2::orbcomm {

namespace {

constexpr int kSpan = 6;          // pulse half length, symbols
constexpr int kTab = 512;         // table points per symbol

double rrc(double t) {   // root raised cosine, t in symbols, unit energy per symbol
    const double a = kRollOff;
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
    if (std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
    return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

const std::vector<float>& rrcTable() {
    static const std::vector<float> t = [] {
        std::vector<float> v(2 * kSpan * kTab + 2);
        for (size_t i = 0; i < v.size(); i++) v[i] = (float)rrc((double)i / kTab - kSpan);
        return v;
    }();
    return t;
}

} // namespace

void genOrbit(const GenChannel& c, double t, double& x, double& y, double& z, double& argLatDeg) {
    const double r = 6378.137 + 775, mu = 398600.4418, we = 7.2921159e-5, inc = 45 * M_PI / 180;
    const double T = 2 * M_PI * std::sqrt(r * r * r / mu);
    const double u = c.phaseDeg * M_PI / 180 + 2 * M_PI * t / T, O = c.raanDeg * M_PI / 180;
    const double xi = r * (std::cos(u) * std::cos(O) - std::sin(u) * std::cos(inc) * std::sin(O));
    const double yi = r * (std::cos(u) * std::sin(O) + std::sin(u) * std::cos(inc) * std::cos(O));
    const double zi = r * std::sin(u) * std::sin(inc);
    const double th = we * t;
    x = std::cos(th) * xi + std::sin(th) * yi;
    y = -std::sin(th) * xi + std::cos(th) * yi;
    z = zi;
    argLatDeg = std::fmod(u * 180 / M_PI, 360.0);
}

struct Signal::Impl {
    struct Ch {
        GenChannel g;
        std::deque<cf32> sym;     // symbols from index base on
        int64_t base = 0, next = 0;   // index of sym.front(), index of the next symbol to make
        double phase = 0;         // SDPSK phase
        double carrier = 0;
        std::deque<uint8_t> bits;
        int frame = 0, msgId = 0;
        uint64_t sent = 0, broken = 0, frames = 0;
    };
    Ch ch[2];
    int nch = 1;
    double fs = 250e3, rs = kSymbolRate, gain = 1, sigma = 0;
    int64_t n = 0;                // samples made
    PortableRng rng;   // the same noise on every platform
    double t0 = 600, badFraction = 0;

    void frameBits(Ch& c, double tSig) {
        std::vector<uint8_t> f(kFramePkts * kPktBytes, 0);
        const double t = t0 + tSig;
        double x, y, z, al;
        genOrbit(c.g, t, x, y, z, al);
        char msg[64];
        snprintf(msg, sizeof msg, "TEST %04d FROM SAT %d HELLO", c.msgId & 0xFFFF, c.g.satId);
        const int msgLen = (int)strlen(msg), parts = (msgLen + 6) / 7;
        for (int k = 0; k < kFramePkts; k++) {
            uint8_t* p = &f[k * kPktBytes];
            if (k == 0) { p[0] = kSync[0]; p[1] = kSync[1]; p[2] = kSync[2]; p[3] = (uint8_t)c.g.satId; p[4] = (uint8_t)c.frame; }
            else if (k == 1) { p[0] = 0x1F; packEphemeris(p, x, y, z, (int)std::fmod(t, 3600.0)); }
            else if (k == 2) { p[0] = 0x1C; p[1] = 2; putU16(p + 2, (unsigned)(100 + c.g.satId)); putU16(p + 4, (unsigned)(115 + c.g.satId)); }
            else if (k == 3) { p[0] = 0x1B; p[1] = 3; putU16(p + 2, 200 + c.g.satId); putU16(p + 4, 210 + c.g.satId); putU16(p + 6, 220 + c.g.satId); }
            else if (k == 4) { p[0] = 0x1D; p[1] = 7; p[2] = 0x81; }
            else if (k == 5 && c.frame % 4 == 0) {
                p[0] = 0x22; p[1] = (uint8_t)c.g.satId;
                const double r = 6378.137 + 775, T = 2 * M_PI * std::sqrt(r * r * r / 398600.4418) / 60;
                putU16(p + 2, 4500); putU16(p + 4, (unsigned)std::lround(std::fmod(c.g.raanDeg + 360, 360.0) * 100)); putU16(p + 6, (unsigned)std::lround(std::fmod(al + 360, 360.0) * 100)); putU16(p + 8, (unsigned)std::lround(T * 100));
            } else if (k >= 6 && k < 6 + parts) {
                const int part = k - 6;
                p[0] = 0x1A; p[1] = (uint8_t)c.msgId; p[2] = (uint8_t)(part | (part == parts - 1 ? 0x80 : 0));
                for (int i = 0; i < 7; i++) { const int q = part * 7 + i; p[3 + i] = q < msgLen ? (uint8_t)msg[q] : ' '; }
            } else { p[0] = 0x1E; for (int i = 1; i < 10; i++) p[i] = (uint8_t)(0x55 ^ i); }
            fletcherSet(p);
            c.sent++;
            if (k > 0 && badFraction > 0 && rng.uniform() < badFraction) {
                const int byte = 1 + (int)(rng.uniform() * 11) % 11, bit = (int)(rng.uniform() * 8) & 7;
                p[byte] ^= (uint8_t)(1 << bit);
                c.broken++;
            }
        }
        for (uint8_t b : f) for (int i = 0; i < 8; i++) c.bits.push_back((b >> i) & 1);
        c.frame = (c.frame + 1) & 255;
        c.msgId = (c.msgId + 1) & 255;
        c.frames++;
    }

    cf32 symbol(Ch& c) {
        if (c.bits.empty()) frameBits(c, (double)c.next / rs);
        const int b = c.bits.front(); c.bits.pop_front();
        c.phase += b ? M_PI / 2 : -M_PI / 2;
        c.phase = std::remainder(c.phase, 2 * M_PI);
        c.next++;
        return cf32((float)std::cos(c.phase), (float)std::sin(c.phase));
    }

    double doppler(const Ch& c, double t) const {
        if (c.g.rampSec <= 0) return c.g.dopplerHz;
        const double ph = std::fmod(t, 2 * c.g.rampSec);
        const double k = ph < c.g.rampSec ? ph / c.g.rampSec : 2 - ph / c.g.rampSec;
        return c.g.dopplerHz + (c.g.dopplerEndHz - c.g.dopplerHz) * k;
    }
};

Signal::Signal(const SignalConfig& c) : c_(c), p_(std::make_unique<Impl>()) {
    Impl& s = *p_;
    s.nch = std::clamp(c.channels, 1, 2);
    s.fs = c.sampleRate;
    s.rs = kSymbolRate * (1 + c.clockPpm * 1e-6);
    s.rng.seed(c.seed);
    s.t0 = c.startSec;
    s.badFraction = c.badFraction;
    for (int i = 0; i < s.nch; i++) {
        s.ch[i].g = c.ch[i];
        s.ch[i].frame = (int)(c.seed * 17 + i * 101) & 255;
        s.ch[i].msgId = (int)(c.seed * 7 + i * 50) & 255;
    }
    const double sps = s.fs / kSymbolRate;
    s.sigma = std::sqrt(sps / std::pow(10.0, c.snrDb / 10) / 2);   // per real dimension, Es/N0 of each downlink (unit power)
    s.gain = c.amplitude / std::sqrt(s.nch + 2 * s.sigma * s.sigma);
}
Signal::~Signal() = default;

void Signal::generate(cf32* out, size_t n) {
    Impl& s = *p_;
    const std::vector<float>& tab = rrcTable();
    for (size_t i = 0; i < n; i++) {
        const double t = (double)s.n / s.fs;
        cf32 acc(0, 0);
        for (int k = 0; k < s.nch; k++) {
            Impl::Ch& c = s.ch[k];
            const double u = t * s.rs;   // symbol time
            const int64_t lo = (int64_t)std::floor(u) - kSpan + 1, hi = (int64_t)std::floor(u) + kSpan;
            while (c.next <= hi) c.sym.push_back(s.symbol(c));
            while (c.base < lo - 1 && !c.sym.empty()) { c.sym.pop_front(); c.base++; }
            cf32 v(0, 0);
            for (int64_t m = std::max(lo, c.base); m <= hi; m++) {
                const double d = (u - (double)m + kSpan) * kTab;
                const int j = (int)d;
                if (j < 0 || j + 1 >= (int)tab.size()) continue;
                const float fr = (float)(d - j);
                v += c.sym[(size_t)(m - c.base)] * (tab[j] + fr * (tab[j + 1] - tab[j]));
            }
            c.carrier += 2 * M_PI * (c.g.offsetHz + s.doppler(c, t)) / s.fs;
            if (c.carrier > M_PI) c.carrier -= 2 * M_PI; else if (c.carrier < -M_PI) c.carrier += 2 * M_PI;
            acc += v * cf32((float)std::cos(c.carrier), (float)std::sin(c.carrier));
        }
        acc += cf32((float)s.rng.gauss(s.sigma), (float)s.rng.gauss(s.sigma));
        out[i] = acc * (float)s.gain;
        s.n++;
    }
}

uint64_t Signal::packetsSent(int ch) const { return p_->ch[ch & 1].sent; }
uint64_t Signal::packetsBroken(int ch) const { return p_->ch[ch & 1].broken; }
uint64_t Signal::framesSent(int ch) const { return p_->ch[ch & 1].frames; }
double Signal::dopplerAt(int ch, double t) const { return p_->doppler(p_->ch[ch & 1], t); }

} // namespace dect2::orbcomm
