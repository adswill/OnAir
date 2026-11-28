// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder generator: a test signal made from a definition (the reverse of the receiver): frame bytes -> bits -> FEC -> interleaver
// -> scrambler -> preamble and sync -> line code -> symbols -> OOK / ASK / FSK / GFSK / GMSK / PSK at the sample rate, with noise,
// a frequency error and a clock error. Used by the round trip tests and the "Generate test signal" button.
#include "dect2/modebuilder/modebuilder.h"
#include <algorithm>
#include <cmath>

namespace dect2::modebuilder {

namespace {
constexpr double kPi = 3.14159265358979323846;

uint32_t next(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
double uni(uint32_t& s) { return (next(s) >> 8) * (1.0 / 16777216.0); }
double gauss(uint32_t& s) {
    const double u1 = std::max(1e-12, uni(s)), u2 = uni(s);
    return std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2);
}

void setBits(std::vector<uint8_t>& b, int off, int w, uint64_t v) {
    for (int i = 0; i < w; i++) {
        const int p = off + i;
        if ((size_t)(p >> 3) >= b.size()) return;
        const uint8_t m = (uint8_t)(0x80 >> (p & 7));
        if ((v >> (w - 1 - i)) & 1) b[(size_t)(p >> 3)] |= m;
        else b[(size_t)(p >> 3)] &= (uint8_t)~m;
    }
}

// raised cosine / root raised cosine impulse response, t in symbols
double rcPulse(double t, double beta, bool root) {
    if (!root) {
        const double s = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        const double den = 1 - 4 * beta * beta * t * t;
        if (std::fabs(den) < 1e-6) return kPi / 4 * s;
        return s * std::cos(kPi * beta * t) / den;
    }
    if (std::fabs(t) < 1e-9) return 1 - beta + 4 * beta / kPi;
    if (std::fabs(std::fabs(t) - 1 / (4 * beta)) < 1e-6)
        return beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
    const double num = std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta));
    return num / (kPi * t * (1 - 16 * beta * beta * t * t));
}
// the GFSK frequency pulse (a rectangle through a Gaussian filter), t in symbols, area 1
double gaussPulse(double t, double bt) {
    const double k = kPi * bt * std::sqrt(2 / std::log(2.0));
    return 0.5 * (std::erf(k * (t + 0.5)) - std::erf(k * (t - 0.5)));
}
}   // namespace

// the frame of a definition with a typed payload: the payload at payloadOffset, zeros around it, length field and check value filled in
static std::vector<uint8_t> payloadFrame(const Definition& d) {
    const std::vector<uint8_t> pl = d.payloadBytes();
    const int cb = d.crcBytes();
    const int need = d.payloadOffset + (int)pl.size() + (d.crcPosition < 0 ? cb : 0);
    int L = d.fixedBytes;
    uint64_t lenValue = 0;
    if (d.lenMode == LenMode::LengthField) {
        const double m = d.lenMultiplier;
        long long v = (long long)std::ceil((std::max(need, (d.lenOffsetBits + d.lenWidthBits + 7) / 8) - d.lenAdjust) / m);
        v = std::clamp(v, 0LL, (long long)((1ull << d.lenWidthBits) - 1));
        lenValue = (uint64_t)v;
        L = std::clamp((int)std::lround((double)v * m) + d.lenAdjust, d.minBytes, d.maxBytes);
    } else if (d.lenMode == LenMode::EndFlag) L = std::clamp(need, d.minBytes, d.maxBytes);
    std::vector<uint8_t> b((size_t)std::max(1, L), 0);
    const size_t limit = b.size() > (size_t)(d.crcPosition < 0 ? cb : 0) ? b.size() - (size_t)(d.crcPosition < 0 ? cb : 0) : 0;
    for (size_t i = 0; i < pl.size() && (size_t)d.payloadOffset + i < limit; i++) b[(size_t)d.payloadOffset + i] = pl[i];
    if (d.lenMode == LenMode::LengthField) setBits(b, d.lenOffsetBits, d.lenWidthBits, lenValue);
    fillCrc(d, b);
    return b;
}

std::vector<uint8_t> randomFrame(const Definition& d, uint32_t& rng) {
    if (!rng) rng = 1;
    if (d.payloadKind != PayloadKind::None && !d.payloadBytes().empty()) return payloadFrame(d);
    for (int attempt = 0; attempt < 64; attempt++) {
        int L = d.fixedBytes;
        uint64_t lenValue = 0;
        if (d.lenMode == LenMode::LengthField) {
            const double m = d.lenMultiplier;
            const long long vmin = (long long)std::ceil((d.minBytes - d.lenAdjust) / m), vmax0 = (long long)std::floor((d.maxBytes - d.lenAdjust) / m);
            const long long vmax = std::min(vmax0, (long long)((1ull << d.lenWidthBits) - 1));
            long long v = vmin > vmax ? std::max(0LL, vmin) : vmin + (long long)(next(rng) % (uint32_t)(vmax - vmin + 1));
            if (v < 0) v = 0;
            lenValue = (uint64_t)v;
            L = std::clamp((int)std::lround((double)v * m) + d.lenAdjust, d.minBytes, d.maxBytes);
            // the frame must hold the length field itself
            if (L * 8 < d.lenOffsetBits + d.lenWidthBits) continue;
        } else if (d.lenMode == LenMode::EndFlag) {
            L = d.minBytes + (int)(next(rng) % (uint32_t)(std::max(1, std::min(d.maxBytes, d.minBytes + 32) - d.minBytes + 1)));
        }
        std::vector<uint8_t> b((size_t)std::max(1, L));
        for (uint8_t& x : b) x = (uint8_t)next(rng);
        if (d.lenMode == LenMode::LengthField) setBits(b, d.lenOffsetBits, d.lenWidthBits, lenValue);
        fillCrc(d, b);
        if (d.lenMode == LenMode::EndFlag) {
            // the flag must not appear in the frame (after stuffing)
            std::vector<uint8_t> bits = stage::bytesToBits(b, d.lsbFirst);
            if (d.bitStuffing) bits = stage::hdlcStuff(bits);
            const std::vector<uint8_t> fl = d.flag.toBits();
            bool clash = false;
            for (size_t i = 0; i + fl.size() <= bits.size() && !clash; i++) clash = std::equal(fl.begin(), fl.end(), bits.begin() + (std::ptrdiff_t)i);
            // nor straddle the end of the frame and the flag
            std::vector<uint8_t> tail(bits.end() - (std::ptrdiff_t)std::min(bits.size(), fl.size()), bits.end());
            tail.insert(tail.end(), fl.begin(), fl.end());
            for (size_t i = 0; i + fl.size() < tail.size() && !clash; i++) clash = std::equal(fl.begin(), fl.end(), tail.begin() + (std::ptrdiff_t)i);
            if (clash) continue;
        }
        return b;
    }
    std::vector<uint8_t> b((size_t)std::max(1, d.longestFrameBytes()), 0);
    fillCrc(d, b);
    return b;
}

std::vector<uint8_t> frameChips(const Definition& d, const std::vector<uint8_t>& frame) {
    const std::vector<uint8_t> body = stage::frameBody(d, frame);
    std::vector<uint8_t> all = d.preamble.toBits();
    const std::vector<uint8_t> sw = d.sync.toBits();
    all.insert(all.end(), sw.begin(), sw.end());
    all.insert(all.end(), body.begin(), body.end());
    // a short tail so the last symbols are not at the edge of the burst
    for (int i = 0; i < 8; i++) all.push_back((uint8_t)(i & 1));
    if (d.line == Line::B8B10) while (all.size() % 8) all.push_back(0);
    if (d.line == Line::B4B5) while (all.size() % 4) all.push_back(0);
    return stage::lineEncode(all, d.line, d.nrziToggleOn);
}

// cyclic framing: one preamble, then a cycle of cycleSymbols symbols per frame (sync word, coded body, alternating padding), line coded as one stream
static std::vector<uint8_t> cycleChips(const Definition& d, const std::vector<std::vector<uint8_t>>& frames) {
    const size_t cycBits = (size_t)std::lround((double)d.cycleSymbols * d.bitsPerSymbol() / stage::lineExpansion(d.line));
    std::vector<uint8_t> all = d.preamble.toBits();
    const std::vector<uint8_t> sw = d.sync.toBits();
    for (const auto& f : frames) {
        std::vector<uint8_t> seg = sw;
        const std::vector<uint8_t> body = stage::frameBody(d, f);
        seg.insert(seg.end(), body.begin(), body.end());
        for (size_t i = 0; seg.size() < cycBits; i++) seg.push_back((uint8_t)(i & 1));
        all.insert(all.end(), seg.begin(), seg.end());
    }
    for (int i = 0; i < 8; i++) all.push_back((uint8_t)(i & 1));
    return stage::lineEncode(all, d.line, d.nrziToggleOn);
}

// carrier offset, frequency error and phase, then noise (the SNR is in the channel bandwidth)
static void carrierAndNoise(std::vector<cf>& out, const Definition& d, const GenOptions& o, double fs) {
    const size_t N = out.size();
    const double f0 = d.offsetHz + o.cfoHz;
    double ph = o.phase;
    const double dph = 2 * kPi * f0 / fs;
    for (size_t n = 0; n < N; n++) {
        out[n] *= cf((float)std::cos(ph), (float)std::sin(ph));
        ph += dph;
        if (ph > kPi) ph -= 2 * kPi;
        if (ph < -kPi) ph += 2 * kPi;
    }
    if (o.snrDb < 200) {
        uint32_t rng = o.seed ? o.seed : 1;
        const double ps = 1.0;   // the carrier is at amplitude 1 (OOK / ASK: the on level)
        const double var = ps / std::pow(10.0, o.snrDb / 10) * fs / std::max(1.0, d.bandwidth());
        const double sd = std::sqrt(var / 2);
        for (cf& x : out) x += cf((float)(sd * gauss(rng)), (float)(sd * gauss(rng)));
    }
}

std::vector<cf> generate(const Definition& d, const std::vector<std::vector<uint8_t>>& frames, const GenOptions& o) {
    const double fs = o.fs > 0 ? o.fs : d.sampleRate;
    if (d.mod == Mod::OFDM) {
        // the block at its own rate (silence between frames), then to fs by linear interpolation
        const double fsO = d.ofdmSampleRate();
        const size_t gap = (size_t)std::max(1.0, std::round(o.gapSymbols)) * (size_t)(d.ofdmFft + d.ofdmCp);
        std::vector<cf> blk(gap, cf(0, 0));
        for (const auto& f : frames) {
            const std::vector<cf> fr = ofdmFrame(d, f);
            blk.insert(blk.end(), fr.begin(), fr.end());
            blk.insert(blk.end(), gap, cf(0, 0));
        }
        std::vector<cf> out;
        if (std::fabs(fs - fsO) < 1e-6) out = std::move(blk);
        else {
            const size_t n = (size_t)std::floor((double)(blk.size() - 1) * fs / fsO);
            out.resize(n);
            for (size_t i = 0; i < n; i++) {
                const double t = (double)i * fsO / fs;
                const size_t k = (size_t)t;
                const float fr = (float)(t - (double)k);
                out[i] = blk[k] + (blk[std::min(k + 1, blk.size() - 1)] - blk[k]) * fr;
            }
        }
        carrierAndNoise(out, d, o, fs);
        return out;
    }
    const int M = d.levels(), bps = d.bitsPerSymbol();
    // value -> level
    std::vector<int> levelOf((size_t)M);
    for (int L = 0; L < M; L++) {
        const int v = !d.symbolMap.empty() ? d.symbolMap[(size_t)L] : (d.gray && M > 2) ? (L ^ (L >> 1)) : L;
        levelOf[(size_t)(v & (M - 1))] = L;
    }
    // symbols with -1 for silence
    std::vector<int> sym;
    const int gap = std::max(1, (int)std::lround(o.gapSymbols));
    auto emit = [&](std::vector<uint8_t> ch) {
        for (int i = 0; i < gap; i++) sym.push_back(-1);
        while (ch.size() % (size_t)bps) ch.push_back(0);
        int acc = 0;   // DPSK phase
        for (size_t i = 0; i < ch.size(); i += (size_t)bps) {
            int v = 0;
            for (int k = 0; k < bps; k++) v = (v << 1) | (ch[i + (size_t)k] & 1);
            int L = levelOf[(size_t)v];
            if (d.mod == Mod::PSK && d.differential) { acc = (acc + L) % M; L = acc; }
            if (d.polarity == Polarity::Inverted) L = d.mod == Mod::PSK ? (M - L) % M : M - 1 - L;
            sym.push_back(L);
        }
    };
    if (d.cycleSymbols > 0) { if (!frames.empty()) emit(cycleChips(d, frames)); }   // one stream: the cycles follow each other without a gap
    else for (const auto& f : frames) emit(frameChips(d, f));
    for (int i = 0; i < gap; i++) sym.push_back(-1);

    const double Rs = d.symbolRate * (1 + o.clockPpm * 1e-6);
    const double sps = fs / Rs;
    const size_t N = (size_t)std::ceil(sym.size() * sps);
    std::vector<cf> out(N);
    const Pulse pulse = d.pulseShape();
    const bool fsk = d.mod == Mod::FSK || d.mod == Mod::GFSK || d.mod == Mod::GMSK;
    const double dev = d.deviation();
    const int span = pulse == Pulse::Gaussian ? 3 : 8;
    auto symAt = [&](long long k) -> int { return k < 0 || k >= (long long)sym.size() ? -1 : sym[(size_t)k]; };
    double phase = 0;
    for (size_t n = 0; n < N; n++) {
        const double t = n / sps;   // in symbols
        const long long k0 = (long long)std::floor(t);
        const bool on = symAt(k0) >= 0;
        cf s(0, 0);
        if (fsk) {
            double fn = 0;   // normalised frequency, -1..1
            if (pulse == Pulse::Gaussian) {
                for (long long k = k0 - span; k <= k0 + span; k++) {
                    const int L = symAt(k);
                    if (L < 0) continue;
                    fn += (2.0 * L / (M - 1) - 1) * gaussPulse(t - (double)k - 0.5, d.bt);
                }
            } else if (on) fn = 2.0 * symAt(k0) / (M - 1) - 1;
            phase += 2 * kPi * fn * dev / fs;
            if (on) s = cf((float)std::cos(phase), (float)std::sin(phase));
        } else {
            auto value = [&](int L) -> cf {
                if (d.mod == Mod::OOK) return cf(L ? 1.f : 0.f, 0);
                if (d.mod == Mod::ASK) return cf((float)(0.25 + 0.75 * L / (M - 1)), 0);
                const double a = 2 * kPi * L / M;
                return cf((float)std::cos(a), (float)std::sin(a));
            };
            if (pulse == Pulse::Rrc || pulse == Pulse::Rc) {
                for (long long k = k0 - span; k <= k0 + span; k++) {
                    const int L = symAt(k);
                    if (L < 0) continue;
                    s += value(L) * (float)rcPulse(t - (double)k - 0.5, d.rolloff, pulse == Pulse::Rrc);
                }
            } else if (on) s = value(symAt(k0));
        }
        out[n] = s;
    }
    carrierAndNoise(out, d, o, fs);
    return out;
}

}   // namespace dect2::modebuilder
