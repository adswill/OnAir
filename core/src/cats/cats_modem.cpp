// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// CATS 2-FSK modem: 9600 bd, +-4.8 kHz (bit 1 = upper tone), frames found by the 32-bit preamble tail + sync word.
#include "dect2/cats/cats.h"
#include <cmath>

namespace dect2 {
namespace cats {

std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& frame, double fs, double offsetHz) {
    const int sps = (int)std::lround(fs / kBaud);
    std::vector<std::complex<float>> iq;
    double ph = 0;
    for (size_t b = 0; b < frame.size() * 8; b++) {
        const bool bit = (frame[b / 8] >> (7 - b % 8)) & 1;
        const double w = 2 * M_PI * (offsetHz + (bit ? kDeviationHz : -kDeviationHz)) / fs;
        for (int s = 0; s < sps; s++) { ph += w; iq.emplace_back((float)std::cos(ph), (float)std::sin(ph)); }
    }
    for (int s = 0; s < 4 * sps; s++) iq.emplace_back(0.f, 0.f);
    return iq;
}

std::vector<Packet> demodulate(const std::vector<std::complex<float>>& iq, double fs) {
    std::vector<Packet> res;
    const int sps = (int)std::lround(fs / kBaud);
    if (sps < 4 || iq.size() < (size_t)sps * 100) return res;
    std::vector<float> disc(iq.size(), 0.f);
    for (size_t i = 1; i < iq.size(); i++) disc[i] = std::arg(iq[i] * std::conj(iq[i - 1]));
    // moving sum over one symbol
    std::vector<float> acc(disc.size() + 1, 0.f);
    for (size_t i = 0; i < disc.size(); i++) acc[i + 1] = acc[i] + disc[i];
    const uint64_t kPat = 0x55ABCDEF12ull & 0xFFFFFFFFFFull;   // last preamble byte + sync word (40 bits)
    size_t resume = 0;
    std::vector<std::pair<size_t, Packet>> found;
    for (int off = 0; off < sps; off++) {
        const size_t nsym = (disc.size() - off) / sps;
        std::vector<float> s(nsym);
        double mean = 0;
        for (size_t k = 0; k < nsym; k++) { s[k] = acc[off + (k + 1) * sps] - acc[off + k * sps]; mean += s[k]; }
        if (nsym == 0) continue;
        mean /= nsym;   // the carrier offset; the alternating preamble makes the middle of a burst unbiased
        uint64_t reg = 0;
        for (size_t k = 0; k < nsym; k++) {
            reg = ((reg << 1) | (s[k] > mean ? 1 : 0)) & 0xFFFFFFFFFFull;
            if (reg != kPat || k + 17 >= nsym) continue;
            size_t p = k + 1;
            auto byteAt = [&](size_t q) { uint8_t b = 0; for (int i = 0; i < 8; i++) b = (uint8_t)((b << 1) | (s[q + i] > mean ? 1 : 0)); return b; };
            const size_t L = byteAt(p) | (byteAt(p + 8) << 8);
            p += 16;
            if (L < 5 || L > (size_t)kMaxLen * 2 + 4 || p + L * 8 > nsym) continue;
            std::vector<uint8_t> body(L);
            for (size_t i = 0; i < L; i++) body[i] = byteAt(p + i * 8);
            Packet pk;
            if (decodeBody(body, pk)) found.push_back({(size_t)off + k * sps, pk});
        }
    }
    (void)resume;
    // the same frame is seen at neighbouring timing phases: keep one per 2 ms of capture
    std::vector<std::pair<size_t, Packet>> uniq;
    for (auto& f : found) {
        bool dup = false;
        for (auto& u : uniq) if ((f.first > u.first ? f.first - u.first : u.first - f.first) < (size_t)(fs * 0.01) && f.second.callsign == u.second.callsign) dup = true;
        if (!dup) uniq.push_back(f);
    }
    for (size_t i = 0; i < uniq.size(); i++) {
        size_t best = i;
        for (size_t j = i + 1; j < uniq.size(); j++) if (uniq[j].first < uniq[best].first) best = j;
        std::swap(uniq[i], uniq[best]);
    }
    for (auto& u : uniq) res.push_back(u.second);
    return res;
}

}  // namespace cats
}  // namespace dect2
