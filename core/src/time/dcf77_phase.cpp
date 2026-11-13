// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/time/dcf77_phase.h"
#include <cmath>

namespace dect2 {

namespace {
constexpr double kChip = 120.0 / 77500.0 * 8000.0;   // samples per chip at 8 kHz
constexpr int kStart = 1600;                         // 200 ms
std::vector<int8_t> makePn() {
    static const char prefix[] = "00000100011000010011100101010110000";
    std::vector<int> s;
    for (const char* p = prefix; *p; p++) s.push_back(*p == '1');
    while (s.size() < 512) { const size_t n = s.size(); s.push_back(s[n - 5] ^ s[n - 9]); }
    std::vector<int8_t> o(512);
    for (size_t i = 0; i < 512; i++) o[i] = s[i] ? -1 : 1;
    return o;
}
}

Dcf77Phase::Dcf77Phase() : pn_(makePn()) { reset(); }
void Dcf77Phase::reset() { n_ = 0; buf_.assign(8000, std::complex<float>(0, 0)); st_ = Dcf77PhaseStatus(); bits_.fill(-1); lockOff_ = -1; seen_ = 0; }

void Dcf77Phase::feed(std::complex<float> z) {
    buf_[n_ % 8000] = z;
    n_++;
    if (n_ % 8000 == 0) second();
}

void Dcf77Phase::second() {
    // phase of each sample against the mean carrier of the last second
    std::complex<float> m(0, 0);
    for (auto& v : buf_) m += v;
    if (std::abs(m) < 1e-9f) return;
    std::vector<float> th(16000);
    for (int i = 0; i < 16000; i++) th[(size_t)i] = std::arg(buf_[(size_t)(i % 8000)] * std::conj(m));
    // integrate over the chips of every start position (a step of a sample around the lock, a coarse search before)
    double best = 0; int bestO = -1; double second = 0;
    const int span = (int)std::ceil(512 * kChip);
    std::vector<float> cum(16001, 0.f);
    for (int i = 0; i < 16000; i++) cum[(size_t)i + 1] = cum[(size_t)i] + th[(size_t)i];
    const int lo = lockOff_ >= 0 ? lockOff_ - 16 : 0, hi = lockOff_ >= 0 ? lockOff_ + 16 : 7999;
    for (int o = lo; o <= hi; o++) {
        int oo = ((o % 8000) + 8000) % 8000;
        if (oo + span >= 16000) continue;
        double acc = 0, en = 0;
        for (int c = 0; c < 512; c++) {
            const int a = oo + (int)std::lround(c * kChip), b = oo + (int)std::lround((c + 1) * kChip);
            const double v = cum[(size_t)b] - cum[(size_t)a];
            acc += pn_[(size_t)c] * v; en += v * v;
        }
        const double r = std::fabs(acc) / std::max(1e-9, std::sqrt(en * 512.0));
        if (r > best) { second = best; best = r; bestO = oo; } else if (r > second) second = r;
    }
    if (bestO < 0) return;
    st_.seconds++;
    st_.corr = (float)best;
    if (best < 0.3) { st_.lastBit = -1; }
    else {
        // the sign at the best offset
        double acc = 0;
        for (int c = 0; c < 512; c++) {
            const int a = bestO + (int)std::lround(c * kChip), b = bestO + (int)std::lround((c + 1) * kChip);
            acc += pn_[(size_t)c] * (cum[(size_t)b] - cum[(size_t)a]);
        }
        st_.lastBit = acc > 0 ? 0 : 1;
        lockOff_ = bestO;
        st_.secondOffset = (bestO + 8000 - kStart) % 8000;
        if (++seen_ > 3) st_.locked = true;
    }
    for (int i = 0; i < 59; i++) bits_[(size_t)i] = bits_[(size_t)i + 1];
    bits_[59] = st_.lastBit;
}

}  // namespace dect2
