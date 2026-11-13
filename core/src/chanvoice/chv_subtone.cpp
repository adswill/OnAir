// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/chanvoice/chv_subtone.h"
#include "dect2/tones/tones_proto.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace chv {

namespace {
constexpr int kWin = 1000;            // one second at 1 kHz
constexpr int kHopSamples = 250;      // a reading every 250 ms
constexpr double kPi = 3.14159265358979323846;
constexpr double kDcsSps = 1000.0 / tones::kDcsBaud;   // 7.44 samples per bit
constexpr int kDcsBits = 69;                              // three words
}

SubToneReader::SubToneReader() : buf_(kWin, 0.f) {}
void SubToneReader::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.f);
    pos_ = filled_ = sinceHop_ = 0;
    bq_[0][0] = bq_[0][1] = bq_[1][0] = bq_[1][1] = 0; dec_ = 0;
    t_ = SubTone{};
    ctcssPend_ = -1; ctcssRun_ = ctcssMiss_ = 0;
    dcsPendCode_ = -1; dcsPendInv_ = false; dcsRun_ = dcsMiss_ = 0;
}

void SubToneReader::feed(const float* x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf_[pos_] = x[i];
        pos_ = (pos_ + 1) % kWin;
        if (filled_ < (size_t)kWin) filled_++;
        if (++sinceHop_ >= (size_t)kHopSamples) { sinceHop_ = 0; if (filled_ >= (size_t)kWin) hop(); }
    }
}

void SubToneReader::feed48k(const float* x, size_t n) {
    // RBJ low-pass, fc 250 Hz at 48 kHz, Q 0.7071 twice (computed once)
    static const double w0 = 2 * kPi * 250.0 / 48000.0, al = std::sin(w0) / (2 * 0.7071), a0 = 1 + al;
    static const double b0 = (1 - std::cos(w0)) / 2 / a0, b1 = (1 - std::cos(w0)) / a0, a1 = -2 * std::cos(w0) / a0, a2 = (1 - al) / a0;
    for (size_t i = 0; i < n; i++) {
        double v = x[i];
        for (int s = 0; s < 2; s++) {
            const double y = b0 * v + bq_[s][0];
            bq_[s][0] = b1 * v - a1 * y + bq_[s][1];
            bq_[s][1] = b0 * v - a2 * y;
            v = y;
        }
        if (++dec_ >= 48) { dec_ = 0; const float f = (float)v; feed(&f, 1); }
    }
}

void SubToneReader::hop() {
    std::vector<float> w(kWin);
    for (int i = 0; i < kWin; i++) w[(size_t)i] = buf_[(pos_ + (size_t)i) % kWin];   // oldest first
    dcsHop(w.data(), kWin);
    ctcssHop(w.data(), kWin);
}

void SubToneReader::ctcssHop(const float* w, int n) {
    // Hann-windowed Goertzel at each of the 50 tones
    static const std::vector<float> han = [] {
        std::vector<float> h(kWin);
        for (int i = 0; i < kWin; i++) h[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * kPi * i / (kWin - 1));
        return h;
    }();
    double mean = 0;
    for (int i = 0; i < n; i++) mean += w[i];
    mean /= n;
    double p[tones::kCtcssN];
    double total = 0;
    for (int k = 0; k < tones::kCtcssN; k++) {
        const double f = tones::ctcssHz(k), c = 2 * std::cos(2 * kPi * f / 1000.0);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) { const double s = (w[i] - mean) * han[(size_t)i] + c * s1 - s2; s2 = s1; s1 = s; }
        p[k] = s1 * s1 + s2 * s2 - c * s1 * s2;
        total += p[k];
    }
    int top = 0;
    for (int k = 1; k < tones::kCtcssN; k++) if (p[k] > p[top]) top = k;
    double second = 0;
    for (int k = 0; k < tones::kCtcssN; k++) if (std::fabs(tones::ctcssHz(k) - tones::ctcssHz(top)) > 3.5) second = std::max(second, p[k]);
    const double avg = total / tones::kCtcssN;
    // a Hann window sums to n / 2: a sine of amplitude A gives |X| = A n / 4
    const double amp = std::sqrt(p[top]) * 4.0 / n;
    const bool good = t_.dcsCode < 0 && p[top] > 4.0 * second && p[top] > 8.0 * avg && amp > 0.003;   // 0.003 of 5 kHz = 15 Hz of deviation (a tone is usually 300 - 700 Hz)
    if (good) {
        ctcssMiss_ = 0;
        if (top == ctcssPend_) ctcssRun_++; else { ctcssPend_ = top; ctcssRun_ = 1; }
        if (ctcssRun_ >= 2 && t_.ctcssIdx != top) { t_.ctcssIdx = top; t_.ctcssHz = tones::ctcssHz(top); t_.dcsCode = -1; t_.dcsInverted = false; t_.dcsAlias = -1; dcsRun_ = 0; dcsPendCode_ = -1; }
    } else {
        ctcssRun_ = 0; ctcssPend_ = -1;
        if (t_.ctcssIdx >= 0 && ++ctcssMiss_ >= 6) { t_.ctcssIdx = -1; t_.ctcssHz = 0; ctcssMiss_ = 0; }
    }
}

void SubToneReader::dcsHop(const float* w, int n) {
    // the last three words: slice at the bit centres, for each sampling phase; take the phase whose word repeats best and is a valid Golay word
    const double lastIdx = n - 1;
    int bestMis = 99; tones::DcsHit best; bool have = false;
    for (double ph = 0; ph < kDcsSps; ph += 0.5) {
        double v[kDcsBits];
        double lo = 1e9, hi = -1e9;
        for (int k = 0; k < kDcsBits; k++) {
            const double pos = lastIdx - (kDcsBits - 0.5 - k) * kDcsSps - ph;
            const int i0 = (int)std::floor(pos);
            if (i0 < 0 || i0 + 1 >= n) { v[k] = 0; continue; }
            const double fr = pos - i0;
            v[k] = w[i0] * (1 - fr) + w[i0 + 1] * fr;
            lo = std::min(lo, v[k]); hi = std::max(hi, v[k]);
        }
        if (hi - lo < 0.02) continue;   // no DCS-sized swing (+-0.1 of 5 kHz would be 500 Hz; 0.02 is 100 Hz peak to peak)
        const double thr = (hi + lo) / 2;
        uint8_t b[kDcsBits];
        for (int k = 0; k < kDcsBits; k++) b[k] = v[k] > thr ? 1 : 0;
        int mis = 0;
        for (int k = 0; k + 23 < kDcsBits; k++) if (b[k] != b[k + 23]) mis++;
        if (mis > 4) continue;
        // majority of the three copies for each bit, then read the word
        uint32_t word = 0;
        for (int k = 0; k < 23; k++) {
            int s = b[k] + b[k + 23] + b[k + 46];
            if (s >= 2) word |= 1u << k;
        }
        tones::DcsHit h;
        if (tones::dcsDecode(word, h) && mis < bestMis) { best = h; bestMis = mis; have = true; }
    }
    if (have) {
        dcsMiss_ = 0;
        if (best.code == dcsPendCode_ && best.inverted == dcsPendInv_) dcsRun_++; else { dcsPendCode_ = best.code; dcsPendInv_ = best.inverted; dcsRun_ = 1; }
        if (dcsRun_ >= 2) { t_.dcsCode = best.code; t_.dcsInverted = best.inverted; t_.dcsAlias = best.aliasCode; t_.dcsAliasInverted = best.aliasInverted; t_.ctcssIdx = -1; t_.ctcssHz = 0; ctcssRun_ = 0; ctcssPend_ = -1; }
    } else {
        dcsRun_ = 0; dcsPendCode_ = -1;
        if (t_.dcsCode >= 0 && ++dcsMiss_ >= 6) { t_.dcsCode = -1; t_.dcsInverted = false; t_.dcsAlias = -1; dcsMiss_ = 0; }
    }
}

} // namespace chv
} // namespace dect2
