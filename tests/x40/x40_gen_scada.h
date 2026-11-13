// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals for the SCADA radio telemetry mode (x40 variant Scada, docs/SCADA-4.1.md). The frame encoders and the modulator exist only here,
// for the tests; the application has no transmit path for this mode. The CRCs are written again here (MSB-first form of the reflected
// polynomials, checked against the catalogue check values) so the receiver's own CRC code is not used to build what it is tested on.
#pragma once
#include "dect2/common/ring.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace scadagen {

using dect2::cf32;
using Bytes = std::vector<uint8_t>;
using Bits = std::vector<uint8_t>;
inline constexpr double kPi = 3.14159265358979323846;

inline uint8_t rev8(uint8_t b) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (b & (1 << i)) r |= (uint8_t)(0x80 >> i); return r; }
inline uint16_t rev16(uint16_t v) { uint16_t r = 0; for (int i = 0; i < 16; i++) if (v & (1 << i)) r |= (uint16_t)(0x8000 >> i); return r; }
// a reflected CRC-16 computed the long way: bytes bit-reversed, MSB-first shift register, result reversed
inline uint16_t crcRef(const uint8_t* p, size_t n, uint16_t poly, uint16_t init, uint16_t xorOut) {
    uint16_t c = rev16(init);
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(rev8(p[i]) << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ poly) : (uint16_t)(c << 1);
    }
    return (uint16_t)(rev16(c) ^ xorOut);
}
inline uint16_t crcMb(const Bytes& b) { return crcRef(b.data(), b.size(), 0x8005, 0xFFFF, 0); }
inline uint16_t crcDnp(const uint8_t* p, size_t n) { return crcRef(p, n, 0x3D65, 0, 0xFFFF); }
inline uint16_t crcX25(const Bytes& b) { return crcRef(b.data(), b.size(), 0x1021, 0xFFFF, 0xFFFF); }

// ---- Modbus RTU
inline Bytes mb(int addr, int fc, const Bytes& data, bool breakCrc = false) {
    Bytes f{(uint8_t)addr, (uint8_t)fc};
    f.insert(f.end(), data.begin(), data.end());
    uint16_t c = crcMb(f);
    if (breakCrc) c ^= 0x0100;
    f.push_back((uint8_t)(c & 0xFF)); f.push_back((uint8_t)(c >> 8));
    return f;
}
inline Bytes be16(int v) { return Bytes{(uint8_t)(v >> 8), (uint8_t)v}; }
inline Bytes cat(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
inline Bytes mbReadReq(int addr, int fc, int start, int qty) { return mb(addr, fc, cat(be16(start), be16(qty))); }
inline Bytes mbRegResp(int addr, int fc, const std::vector<int>& regs) {
    Bytes d{(uint8_t)(2 * regs.size())};
    for (int r : regs) d = cat(d, be16(r));
    return mb(addr, fc, d);
}

// ---- DNP3: one link frame with its CRCs (header, then a CRC every 16 octets of user data)
inline Bytes dnpLink(int ctrl, int dst, int src, const Bytes& ud, int breakBlock = -1) {
    Bytes f{0x05, 0x64, (uint8_t)(5 + ud.size()), (uint8_t)ctrl, (uint8_t)dst, (uint8_t)(dst >> 8), (uint8_t)src, (uint8_t)(src >> 8)};
    uint16_t c = crcDnp(f.data(), 8);
    f.push_back((uint8_t)c); f.push_back((uint8_t)(c >> 8));
    for (size_t i = 0, blk = 0; i < ud.size(); i += 16, blk++) {
        const size_t k = std::min<size_t>(16, ud.size() - i);
        f.insert(f.end(), ud.begin() + (ptrdiff_t)i, ud.begin() + (ptrdiff_t)(i + k));
        c = crcDnp(&ud[i], k);
        if ((int)blk == breakBlock) c ^= 0x0001;
        f.push_back((uint8_t)c); f.push_back((uint8_t)(c >> 8));
    }
    return f;
}
// an application fragment through the transport layer: segments of at most 249 octets, unconfirmed user data (function 4)
inline std::vector<Bytes> dnpApp(bool fromMaster, int dst, int src, const Bytes& app, int tseq = 0) {
    std::vector<Bytes> out;
    const int ctrl = (fromMaster ? 0x80 : 0x00) | 0x40 | 4;
    for (size_t i = 0; i < app.size() || i == 0; i += 249) {
        const size_t k = std::min<size_t>(249, app.size() - i);
        const bool fir = i == 0, fin = i + k >= app.size();
        Bytes ud{(uint8_t)((fin ? 0x80 : 0) | (fir ? 0x40 : 0) | (tseq++ & 0x3F))};
        ud.insert(ud.end(), app.begin() + (ptrdiff_t)i, app.begin() + (ptrdiff_t)(i + k));
        out.push_back(dnpLink(ctrl, dst, src, ud));
        if (k == 0) break;
    }
    return out;
}
inline Bytes le16(int v) { return Bytes{(uint8_t)v, (uint8_t)(v >> 8)}; }
inline Bytes le32(uint32_t v) { return Bytes{(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; }

// ---- byte framing to bits
inline Bits uart(const Bytes& b, int parity /*0 none 1 even 2 odd*/, int stopBits = 1, int gapBits = 0) {
    Bits o;
    for (uint8_t v : b) {
        o.push_back(0);
        int ones = 0;
        for (int i = 0; i < 8; i++) { const int x = (v >> i) & 1; o.push_back((uint8_t)x); ones += x; }
        if (parity == 1) o.push_back((uint8_t)(ones & 1));
        if (parity == 2) o.push_back((uint8_t)!(ones & 1));
        for (int i = 0; i < stopBits + gapBits; i++) o.push_back(1);
    }
    return o;
}
// HDLC: flags, the payload and its FCS (low octet first) LSB first with zero insertion, flags; NRZI optional (a 0 is a change)
inline Bits hdlc(const Bytes& payload, bool nrzi, int flags = 8) {
    Bytes f = payload;
    const uint16_t c = crcX25(payload);
    f.push_back((uint8_t)c); f.push_back((uint8_t)(c >> 8));
    Bits d;
    auto flag = [&] { for (int i = 0; i < 8; i++) d.push_back((0x7E >> i) & 1); };
    for (int i = 0; i < flags; i++) flag();
    int ones = 0;
    for (uint8_t v : f) for (int i = 0; i < 8; i++) {
        const int x = (v >> i) & 1;
        d.push_back((uint8_t)x);
        if (x) { if (++ones == 5) { d.push_back(0); ones = 0; } } else ones = 0;
    }
    for (int i = 0; i < 4; i++) flag();
    if (!nrzi) return d;
    Bits o; int lev = 1;
    for (uint8_t x : d) { if (!x) lev ^= 1; o.push_back((uint8_t)lev); }
    return o;
}

// ---- the air: FM with audio FSK, or direct 2-FSK / 4-FSK, in bursts, with noise and impairments
struct Phy {
    int kind = 0;                        // 0 audio FSK (tones on FM), 1 2-FSK, 2 4-FSK
    double markHz = 1200, spaceHz = 2200, fmDevHz = 3000, twistDb = 0;   // audio FSK; twistDb: space tone level against mark (pre- / de-emphasis)
    double bitRate = 1200, devHz = 2400;   // direct FSK: deviation of a 2-FSK symbol, the outer symbol of 4-FSK
    bool invert = false;
    double bt = 0.5;                     // Gaussian shaping of direct FSK
    int preambleBits = 48, tailBits = 16;
    bool dotting = true;                 // direct FSK: the preamble alternates (2-FSK 1010..., 4-FSK +3 -3 ...) as radio modems send it; then 12 idle bits. Audio FSK: idle mark
};
struct Air {
    double fs = 250000, offsetHz = 0, driftHzPerSec = 0, amp = 1;
    double cn0 = 200;                    // carrier to noise density, dB-Hz (200: no noise)
    double clockPpm = 0;                 // the transmitter's bit clock error
    bool continuousCarrier = false;      // the carrier stays on between bursts (idle mark)
    unsigned seed = 1;
};
struct Burst { Bits bits; double gapAfterSec = 0.04; };

// Returns the signal; startSec receives the time each burst's data begins and endSec where it ends
inline std::vector<cf32> render(const Phy& p, const Air& a, const std::vector<Burst>& bursts, double leadSec = 0.25, double tailSec = 0.4,
                                std::vector<double>* startSec = nullptr, std::vector<double>* endSec = nullptr) {
    const double fs = a.fs, rate = p.bitRate * (1 + a.clockPpm * 1e-6);
    const int bps = p.kind == 2 ? 2 : 1;
    const double symRate = rate / bps;
    // frequency per sample (normalised), carrier on/off
    std::vector<float> f;
    std::vector<uint8_t> on;
    auto idle = [&](double sec, bool carrier) { const size_t n = (size_t)(sec * fs); f.insert(f.end(), n, 1e9f); on.insert(on.end(), n, (uint8_t)carrier); };
    idle(leadSec, a.continuousCarrier);
    for (const Burst& b : bursts) {
        Bits all;
        if (p.kind != 0 && p.dotting) {
            for (int i = 0; i < p.preambleBits; i++) all.push_back(p.kind == 1 ? (uint8_t)(i & 1 ? 0 : 1) : (uint8_t)((i & 3) != 0));   // 4-FSK: dibits 01 11 = +3 -3
            all.insert(all.end(), 12, 1);
        } else all.assign((size_t)p.preambleBits, 1);
        const size_t pre = all.size();
        all.insert(all.end(), b.bits.begin(), b.bits.end());
        all.insert(all.end(), (size_t)p.tailBits, 1);
        if (bps == 2 && all.size() % 2) all.push_back(1);
        const size_t nSym = all.size() / bps;
        const size_t n0 = f.size(), n = (size_t)std::ceil(nSym / symRate * fs);
        if (startSec) startSec->push_back((double)n0 / fs + pre / rate);
        if (endSec) endSec->push_back((double)n0 / fs + (pre + b.bits.size()) / rate);
        for (size_t i = 0; i < n; i++) {
            const size_t k = std::min(nSym - 1, (size_t)((double)i / fs * symRate));
            float v;
            if (bps == 1) v = all[k] ? 1.f : -1.f;
            else { const int b0 = all[2 * k], b1 = all[2 * k + 1]; v = b0 == 0 ? (b1 ? 1.f : 1.f / 3) : (b1 ? -1.f : -1.f / 3); }
            if (p.kind != 0 && p.invert) v = -v;
            f.push_back(v); on.push_back(1);
        }
        idle(b.gapAfterSec, a.continuousCarrier);
    }
    idle(tailSec, a.continuousCarrier);
    // idle samples carry the mark level (a continuous carrier between bursts sends idle mark)
    const float idleLev = p.kind == 2 ? -1.f : 1.f;   // async idle is all ones: 2-FSK mark, 4-FSK dibit 11 (-3)
    for (float& v : f) if (v > 1e8f) v = p.kind == 0 ? 1.f : (p.invert ? -idleLev : idleLev);
    std::vector<float> fr(f.size());
    if (p.kind == 0) {
        double ph = 0;
        const double twist = std::pow(10.0, p.twistDb / 20);
        for (size_t i = 0; i < f.size(); i++) {
            const bool mark = f[i] > 0;
            ph += 2 * kPi * (mark ? p.markHz : p.spaceHz) / fs;
            if (ph > kPi) ph -= 2 * kPi;
            fr[i] = (float)(p.fmDevHz * std::sin(ph) * (mark ? 1.0 : twist));
        }
    } else {
        // premodulation shaping: two centred boxcars of about BT = 0.5 (a close stand-in for a Gaussian filter, O(1) per sample)
        const double sps = fs / symRate;
        const int Lb = std::max(1, (int)std::lround(0.5 / p.bt * 0.5 * sps));
        std::vector<float> tmp(f.size());
        auto box = [&](const std::vector<float>& in, std::vector<float>& out) {
            double acc = 0; const long n = (long)in.size(), h = Lb / 2;
            auto at = [&](long j) { return (double)in[(size_t)std::min(n - 1, std::max(0L, j))]; };
            for (long j = -h; j < Lb - h; j++) acc += at(j);
            for (long i = 0; i < n; i++) { out[(size_t)i] = (float)(acc / Lb); acc += at(i + Lb - h) - at(i - h); }
        };
        box(f, tmp); box(tmp, fr);
        for (float& v : fr) v = (float)(v * p.devHz);
    }
    std::mt19937 rng(a.seed);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float sig = a.cn0 >= 199 ? 0.f : (float)(a.amp * std::sqrt(fs * std::pow(10.0, -a.cn0 / 10) / 2));
    std::vector<cf32> x(f.size());
    double ph = 0;
    for (size_t i = 0; i < f.size(); i++) {
        const double t = (double)i / fs;
        ph += 2 * kPi * (a.offsetHz + a.driftHzPerSec * t + fr[i]) / fs;
        if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
        const cf32 c = on[i] ? cf32((float)(a.amp * std::cos(ph)), (float)(a.amp * std::sin(ph))) : cf32(0, 0);
        x[i] = c + (sig > 0 ? cf32(sig * nd(rng), sig * nd(rng)) : cf32(0, 0));
    }
    return x;
}

} // namespace scadagen
