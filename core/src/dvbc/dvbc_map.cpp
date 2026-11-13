// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-C constellations, byte to symbol mapping with the differential coding of the two MSBs, and the test transmitter (EN 300 429 sections 7 to 9).
#include "dect2/dvbc/dvbc.h"
#include "dect2/tv/demo_ts.h"
#include "dect2/dvbt/dvbt.h"
#include <algorithm>
#include <cstring>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace dvbc {

const char* qamName(int q) {
    static const char* const n[] = {"16-QAM", "32-QAM", "64-QAM", "128-QAM", "256-QAM"};
    return q >= 0 && q < kNumQam ? n[q] : "QAM ?";
}

namespace {

// quadrant 1 of the square constellations: grid index i (x = 2 i + 1) along I and Q, Gray coded, the LSB bits alternating I (even bits) and Q (odd)
int squareLsb(int ix, int iy, int half) {
    const int gi = ix ^ (ix >> 1), gq = iy ^ (iy >> 1);
    int v = 0;
    for (int j = 0; j < half; j++) v |= ((gi >> j) & 1) << (2 * j) | ((gq >> j) & 1) << (2 * j + 1);
    return v;
}

// 32-QAM quadrant 1 (figure 7): odd coordinates 1, 3, 5 without the corner (5, 5); value of the 3 LSBs, [y][x] with x, y = 0..2
const int k32[3][3] = {{0, 1, 3}, {4, 5, 7}, {6, 2, -1}};

Constellation build(int q) {
    Constellation c;
    c.q = q; c.order = qamOrder(q); c.bits = qamBits(q);
    const int lsbBits = c.bits - 2;
    // quadrant 1 points: (grid x, grid y) in odd units -> LSB value
    std::vector<std::pair<std::pair<int, int>, int>> q1;
    if (q == 0 || q == 2 || q == 4) {
        const int n = 1 << (lsbBits / 2);
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) q1.push_back({{2 * x + 1, 2 * y + 1}, squareLsb(x, y, lsbBits / 2)});
        c.cells = 2 * n;
    } else if (q == 1) {
        for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) if (k32[y][x] >= 0) q1.push_back({{2 * x + 1, 2 * y + 1}, k32[y][x]});
        c.cells = 6;
    } else {
        // 128-QAM: 36 positions of 1..11 without the 2 x 2 corner; the inner 4 x 4 square as 64-QAM (LSB 4 = 0), the two strips outside it
        // (x 9..11 with y 1..7, and y 9..11 with x 1..7) carry LSB 4 = 1, the strip on the right mirrored onto the strip on top so that
        // neighbours across the corner differ in few bits.
        for (int y = 0; y < 6; y++) for (int x = 0; x < 6; x++) {
            if (x >= 4 && y >= 4) continue;
            int v;
            if (x < 4 && y < 4) v = squareLsb(x, y, 2);
            else if (x >= 4) v = 16 | squareLsb(x - 4, y, 1) | ((y >> 1) & 1) << 3 | (((y >> 1) ^ y) & 1) << 1 | 0;   // fills bits 0..3
            else v = 16 | 4 | squareLsb(y - 4, x, 1) | ((x >> 1) & 1) << 3 | (((x >> 1) ^ x) & 1) << 1;
            q1.push_back({{2 * x + 1, 2 * y + 1}, v});
        }
        c.cells = 12;
    }
    // the 128-QAM layout above must be a bijection onto 0..31: repair duplicates deterministically
    {
        std::vector<int> used(1u << lsbBits, 0);
        std::vector<size_t> dup;
        for (size_t i = 0; i < q1.size(); i++) { int& v = q1[i].second; v &= (1 << lsbBits) - 1; if (used[(size_t)v]++) dup.push_back(i); }
        int free = 0;
        for (size_t i : dup) { while (used[(size_t)free]) free++; q1[i].second = free; used[(size_t)free] = 1; }
    }
    c.pts.assign((size_t)c.order, cf32(0, 0));
    double e2 = 0, e4 = 0;
    for (int quad = 0; quad < 4; quad++) {
        for (const auto& p : q1) {
            int x = p.first.first, y = p.first.second;
            for (int r = 0; r < quad; r++) { const int t = x; x = -y; y = t; }   // +90 degrees
            const int sym = (iqOf(quad) << lsbBits) | p.second;
            c.pts[(size_t)sym] = cf32((float)x, (float)y);
            const double m = (double)x * x + (double)y * y;
            e2 += m; e4 += m * m;
        }
    }
    e2 /= c.order; e4 /= c.order;
    const float s = (float)(1.0 / std::sqrt(e2));
    for (auto& p : c.pts) p *= s;
    c.unit = s;
    c.r2 = (float)(e4 / e2 * s * s);
    c.dmin = 2 * s;
    c.lut.assign((size_t)c.cells * (size_t)c.cells, 0);
    for (int iy = 0; iy < c.cells; iy++) for (int ix = 0; ix < c.cells; ix++) {
        const cf32 z(((float)(2 * (ix - c.cells / 2)) + 1) * s, ((float)(2 * (iy - c.cells / 2)) + 1) * s);
        int best = 0; float bd = 1e30f;
        for (int k = 0; k < c.order; k++) { const float d = std::norm(z - c.pts[(size_t)k]); if (d < bd) { bd = d; best = k; } }
        c.lut[(size_t)iy * (size_t)c.cells + (size_t)ix] = (uint16_t)best;
    }
    return c;
}

} // namespace

const Constellation& constellation(int q) {
    static const std::vector<Constellation> all = [] { std::vector<Constellation> v; for (int i = 0; i < kNumQam; i++) v.push_back(build(i)); return v; }();
    return all[(size_t)std::clamp(q, 0, kNumQam - 1)];
}

void Mapper::push(const uint8_t* bytes, size_t n, std::vector<cf32>& out) {
    const int m = c_->bits, lsb = m - 2;
    for (size_t i = 0; i < n; i++) {
        acc_ = (acc_ << 8) | bytes[i];
        nacc_ += 8;
        while (nacc_ >= m) {
            const int v = (int)((acc_ >> (nacc_ - m)) & ((1u << m) - 1));
            nacc_ -= m;
            const int ab = v >> lsb;                         // A_k B_k
            prevQuad_ = (prevQuad_ + quadOf(ab)) & 3;       // AB 00: same quadrant, 10: +90, 11: +180, 01: +270 (section 8)
            out.push_back(c_->pts[(size_t)((iqOf(prevQuad_) << lsb) | (v & ((1 << lsb) - 1)))]);
        }
        acc_ &= 0xFFFF;
    }
}

double rrc(double t, double b) {
    const double pi = dect2::math::kPi;
    if (std::fabs(t) < 1e-9) return 1 - b + 4 * b / pi;
    if (std::fabs(std::fabs(t) - 1 / (4 * b)) < 1e-9)
        return b / std::sqrt(2.0) * ((1 + 2 / pi) * std::sin(pi / (4 * b)) + (1 - 2 / pi) * std::cos(pi / (4 * b)));
    return (std::sin(pi * t * (1 - b)) + 4 * b * t * std::cos(pi * t * (1 + b))) / (pi * t * (1 - 16 * b * b * t * t));
}

struct Signal::Il { dvbt::ConvInterleaver il{false}; };

Signal::Signal(const SignalConfig& cfg) : cfg_(cfg), map_(cfg.qam), il_(new Il), rng_(cfg.seed) {
    ts_ = cfg.ts ? cfg.ts : demoTsSource(netBitrate(cfg.qam, cfg.symbolRate));
    step_ = cfg.symbolRate * (1 + cfg.clockPpm * 1e-6) / cfg.sampleRate;
    taps_.resize((size_t)phases_ * 2 * half_);
    for (int p = 0; p < phases_; p++) for (int j = 0; j < 2 * half_; j++) taps_[(size_t)p * 2 * half_ + j] = (float)rrc((double)p / phases_ + half_ - 1 - j, cfg.rollOff);
    t_ = half_;
    const double snr = std::pow(10.0, cfg.snrDb / 10);
    const double var = cfg.snrDb >= 199 ? 0 : cfg.sampleRate / (cfg.symbolRate * snr);
    noise_ = (float)std::sqrt(var / 2);
    scale_ = (float)(cfg.level / std::sqrt(1 + var));
    dph_ = 2 * dect2::math::kPi * cfg.cfoHz / cfg.sampleRate;
    if (cfg.echoDb > 0) {
        echoLen_ = std::max(1, (int)std::lround(cfg.echoUs * 1e-6 * cfg.sampleRate));
        echo_.assign((size_t)echoLen_, cf32(0, 0));
        echoGain_ = std::polar((float)std::pow(10.0, -cfg.echoDb / 20), (float)(cfg.echoDeg * dect2::math::kPi / 180));
    }
}

Signal::~Signal() = default;

void Signal::refill() {
    if (config().symbols) { config().symbols(sym_); packets_ += 8; return; }
    uint8_t ts[8 * 188], sc[8 * 188], rs[8 * 204], il[8 * 204];
    for (int i = 0; i < 8; i++) { ts_(ts + i * 188); ts[i * 188] = 0x47; }
    dvbt::scramble(ts, 8, sc);
    for (int i = 0; i < 8; i++) dvbt::rsEncode(sc + i * 188, rs + i * 204);
    il_->il.process(rs, il, sizeof rs);
    map_.push(il, sizeof il, sym_);
    packets_ += 8;
}

void Signal::generate(cf32* out, size_t n) {
    const int L = 2 * half_;
    for (size_t i = 0; i < n; i++) {
        const double fl = std::floor(t_);
        const size_t k0 = (size_t)fl - (size_t)half_ + 1;
        while (k0 + (size_t)L > symBase_ + sym_.size()) refill();
        const int p = std::min(phases_ - 1, (int)((t_ - fl) * phases_));
        const float* h = &taps_[(size_t)p * (size_t)L];
        const cf32* s = &sym_[k0 - symBase_];
        float re = 0, im = 0;
        for (int j = 0; j < L; j++) { re += h[j] * s[j].real(); im += h[j] * s[j].imag(); }
        cf32 y(re, im);
        if (echoLen_) { const cf32 d = echo_[echoPos_]; echo_[echoPos_] = y; echoPos_ = (echoPos_ + 1) % (size_t)echoLen_; y += echoGain_ * d; }
        if (cfg_.inverted) y = std::conj(y);
        y *= cf32((float)std::cos(ph_), (float)std::sin(ph_));
        ph_ += dph_;
        if (ph_ > dect2::math::kPi) ph_ -= 2 * dect2::math::kPi; else if (ph_ < -dect2::math::kPi) ph_ += 2 * dect2::math::kPi;
        if (noise_ > 0) y += cf32(noise_ * nd_(rng_), noise_ * nd_(rng_));
        out[i] = y * scale_;
        t_ += step_;
        if (k0 - symBase_ > 200000) { sym_.erase(sym_.begin(), sym_.begin() + 100000); symBase_ += 100000; }
    }
}

} // namespace dvbc
} // namespace dect2
