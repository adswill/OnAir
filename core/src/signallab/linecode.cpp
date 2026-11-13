// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 4: line codes, LFSR descramblers, polarity. Decoders work on the slicer's symbols (for the two-chip codes,
// Manchester / differential Manchester / Miller, one symbol is a half bit). Ranking uses how valid the code is (forbidden chip
// pairs) and how much structure (a repeated sync word, low-entropy windows) the decoded stream shows.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace dect2::signallab {

const char* lineCodeName(LineCode c) {
    switch (c) {
        case LineCode::NRZ: return "NRZ";
        case LineCode::NRZI: return "NRZI";
        case LineCode::Manchester: return "Manchester";
        case LineCode::ManchesterInv: return "Manchester (inverted)";
        case LineCode::DiffManchester: return "differential Manchester";
        case LineCode::Miller: return "Miller";
        case LineCode::Differential: return "differential";
    }
    return "";
}

Bits invert(const Bits& b) { Bits o(b.size()); for (size_t i = 0; i < b.size(); i++) o[i] = (uint8_t)(b[i] ^ 1); return o; }

Bits decodeLine(const Bits& in, LineCode c, int phase, double* vr) {
    Bits o;
    size_t bad = 0, tot = 0;
    switch (c) {
        case LineCode::NRZ: o = in; break;
        case LineCode::NRZI: case LineCode::Differential:   // a transition is a 1
            for (size_t i = 1; i < in.size(); i++) o.push_back((uint8_t)(in[i] != in[i - 1]));
            break;
        case LineCode::Manchester: case LineCode::ManchesterInv:
            for (size_t i = (size_t)phase; i + 1 < in.size(); i += 2) {
                tot++;
                if (in[i] == in[i + 1]) bad++;
                // IEEE 802.3: 0 = high-to-low (10), 1 = low-to-high (01)
                uint8_t b = (uint8_t)(in[i] == 0 && in[i + 1] == 1);
                if (in[i] == in[i + 1]) b = in[i + 1];
                o.push_back(c == LineCode::ManchesterInv ? (uint8_t)(b ^ 1) : b);
            }
            break;
        case LineCode::DiffManchester:
            for (size_t i = (size_t)phase; i + 1 < in.size(); i += 2) {
                tot++;
                if (in[i] == in[i + 1]) bad++;
                // a transition at the start of the bit is a 0
                const uint8_t prev = i > 0 ? in[i - 1] : (uint8_t)(in[i] ^ 1);
                o.push_back((uint8_t)(prev == in[i]));
            }
            break;
        case LineCode::Miller: {
            uint8_t last = 1;
            for (size_t i = (size_t)phase; i + 1 < in.size(); i += 2) {
                const uint8_t b = (uint8_t)(in[i] != in[i + 1]);   // a mid-bit transition is a 1
                if (i > 0) { tot++; if (in[i - 1] != in[i] && !(last == 0 && b == 0)) bad++; }   // boundary transitions only between two 0s
                o.push_back(b);
                last = b;
            }
            break;
        }
    }
    if (vr) *vr = tot ? (double)bad / (double)tot : 0;
    return o;
}

Bits encodeLine(const Bits& in, LineCode c) {
    Bits o;
    uint8_t L = 0;
    switch (c) {
        case LineCode::NRZ: return in;
        case LineCode::NRZI: case LineCode::Differential:
            o.push_back(L);
            for (uint8_t b : in) { if (b) L ^= 1; o.push_back(L); }
            break;
        case LineCode::Manchester: case LineCode::ManchesterInv:
            for (uint8_t b : in) {
                const uint8_t v = c == LineCode::ManchesterInv ? (uint8_t)(b ^ 1) : b;
                o.push_back((uint8_t)(v ? 0 : 1)); o.push_back((uint8_t)(v ? 1 : 0));
            }
            break;
        case LineCode::DiffManchester:
            L = 1;
            for (uint8_t b : in) { if (!b) L ^= 1; o.push_back(L); L ^= 1; o.push_back(L); }
            break;
        case LineCode::Miller: {
            uint8_t last = 1;
            for (uint8_t b : in) {
                if (b) { o.push_back(L); L ^= 1; o.push_back(L); }
                else { if (last == 0) L ^= 1; o.push_back(L); o.push_back(L); }
                last = b;
            }
            break;
        }
    }
    return o;
}

const std::vector<Scrambler>& scramblers() {
    static const std::vector<Scrambler> s = {
        {"G3RUH x^17+x^12+1 (9600 bd packet)", (1u << 16) | (1u << 11), 17, false},
        {"V.22bis x^17+x^14+1", (1u << 16) | (1u << 13), 17, false},
        {"V.29 x^23+x^18+1", (1u << 22) | (1u << 17), 23, false},
        {"802.11b x^7+x^4+1 (self-synchronising)", (1u << 6) | (1u << 3), 7, false},
        {"802.11a x^7+x^4+1 (additive)", (1u << 6) | (1u << 3), 7, true},
        {"PN9 x^9+x^5+1 (additive whitening)", (1u << 8) | (1u << 4), 9, true},
        {"PN15 x^15+x^14+1 (additive)", (1u << 14) | (1u << 13), 15, true},
        {"PN7 x^7+x^6+1 (additive)", (1u << 6) | (1u << 5), 7, true},
        {"CCSDS x^8+x^7+x^5+x^3+1 (additive)", (1u << 7) | (1u << 6) | (1u << 4) | (1u << 2), 8, true},
    };
    return s;
}

namespace {
inline uint8_t parity(uint32_t v) { return (uint8_t)(std::popcount(v) & 1); }
}

Bits descramble(const Bits& in, const Scrambler& s, uint32_t seed) {
    const uint32_t mask = s.degree >= 32 ? 0xFFFFFFFFu : ((1u << s.degree) - 1);
    Bits o(in.size());
    uint32_t r = s.additive ? (seed & mask) : 0;
    if (s.additive && !r) r = 1;
    for (size_t i = 0; i < in.size(); i++) {
        const uint8_t fb = parity(r & s.taps);
        o[i] = (uint8_t)(in[i] ^ fb);
        r = ((r << 1) | (s.additive ? fb : in[i])) & mask;
    }
    return o;
}

Bits scramble(const Bits& in, const Scrambler& s, uint32_t seed) {
    if (s.additive) return descramble(in, s, seed);
    const uint32_t mask = s.degree >= 32 ? 0xFFFFFFFFu : ((1u << s.degree) - 1);
    Bits o(in.size());
    uint32_t r = 0;
    for (size_t i = 0; i < in.size(); i++) {
        o[i] = (uint8_t)(in[i] ^ parity(r & s.taps));
        r = ((r << 1) | o[i]) & mask;
    }
    return o;
}

double structureScore(const Bits& b) {
    if (b.size() < 64) return 0;
    std::vector<uint32_t> h(256, 0);
    uint32_t w = 0;
    for (size_t i = 0; i < b.size(); i++) { w = ((w << 1) | b[i]) & 0xFF; if (i >= 7) h[w]++; }
    const double n = (double)(b.size() - 7);
    double H = 0;
    for (uint32_t c : h) if (c) { const double p = c / n; H -= p * std::log2(p); }
    // the commonest 24-bit window that is not a run or an alternation
    std::unordered_map<uint32_t, uint32_t> cnt;
    uint32_t v = 0, top = 0;
    for (size_t i = 0; i < b.size(); i++) {
        v = ((v << 1) | b[i]) & 0xFFFFFF;
        if (i < 23 || v == 0 || v == 0xFFFFFF || v == 0xAAAAAA || v == 0x555555) continue;
        top = std::max(top, ++cnt[v]);
    }
    const double rep = std::min(1.0, top > 2 ? (double)top * 48.0 / (double)b.size() : 0.0);
    return (1 - H / 8) + 0.5 * rep;
}

namespace {
bool twoChip(LineCode c) { return c == LineCode::Manchester || c == LineCode::ManchesterInv || c == LineCode::DiffManchester || c == LineCode::Miller; }
// two-chip codes: each stream takes the chip phase with fewer violations (a burst can start on either half of a bit)
Bits decodeAuto(const Bits& s, LineCode c, double* v) {
    double v0 = 0, v1 = 0;
    Bits d0 = decodeLine(s, c, 0, &v0);
    if (!twoChip(c)) { if (v) *v = v0; return d0; }
    Bits d1 = decodeLine(s, c, 1, &v1);
    if (v) *v = std::min(v0, v1);
    return v1 < v0 ? d1 : d0;
}
}

Bits applyLineGuess(const Bits& raw, const LineCodeGuess& g) {
    Bits b = decodeAuto(raw, g.code, nullptr);
    if (g.scrambler >= 0 && !scramblers()[(size_t)g.scrambler].additive) b = descramble(b, scramblers()[(size_t)g.scrambler]);
    if (g.inverted) b = invert(b);
    return b;
}

std::vector<LineCodeGuess> rankLineCodes(const std::vector<Bits>& streams) {
    std::vector<LineCodeGuess> out;
    struct Try { LineCode c; int ph; };
    const Try tries[] = {{LineCode::NRZ, 0}, {LineCode::NRZI, 0}, {LineCode::Manchester, 0}, {LineCode::DiffManchester, 0}, {LineCode::Miller, 0}};
    for (const Try& t : tries) {
        // validity over all streams (each burst picks its own chip phase for the two-chip codes)
        double viol = 0, nv = 0;
        std::vector<Bits> dec;
        for (const Bits& s : streams) {
            double v = 0;
            Bits d = decodeAuto(s, t.c, &v);
            viol += v * (double)s.size(); nv += (double)s.size();
            dec.push_back(std::move(d));
        }
        viol = nv > 0 ? viol / nv : 0;
        if (t.c != LineCode::NRZ && t.c != LineCode::NRZI && viol > 0.08) continue;
        for (int sc = -1; sc < (int)scramblers().size(); sc++) {
            if (sc >= 0 && scramblers()[(size_t)sc].additive) continue;   // additive PN needs the frame start: tried at the frame stage
            Bits all;
            for (const Bits& d : dec) {
                const Bits q = sc >= 0 ? descramble(d, scramblers()[(size_t)sc]) : d;
                all.insert(all.end(), q.begin(), q.end());
            }
            LineCodeGuess g;
            g.code = t.c; g.phase = t.ph; g.scrambler = sc; g.violations = viol;
            g.score = structureScore(all) - 4 * viol + (t.c == LineCode::NRZ ? 0.02 : 0) - (sc >= 0 ? 0.03 : 0);
            // the two-chip codes halve the rate: only better when their chip pairs really are valid
            // valid chip pairs are strong evidence: random NRZ bits break them half the time. Any rate-1/2 chip stream also looks
            // structured to the window entropy, so the bonus must outweigh that. Differential Manchester shares Manchester's chips.
            if (t.c != LineCode::NRZ && t.c != LineCode::NRZI) g.score += (t.c == LineCode::DiffManchester ? 0.9 : 1.0) * (1 - viol / 0.08);
            out.push_back(g);
        }
    }
    std::sort(out.begin(), out.end(), [](const LineCodeGuess& a, const LineCodeGuess& b) { return a.score > b.score; });
    return out;
}

} // namespace dect2::signallab
