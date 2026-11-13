// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/kavb/kavb_phy.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace dect2::kavb {

using kris::LdpcRate;
using kris::Mod;

static const double kSymRates[kNumProfiles] = {700000.0, 1050000.0, 1400000.0, 1400000.0};

Profile profileOf(int id, bool robust) {
    if (id < 0) id = 0;
    if (id >= kNumProfiles) id = kNumProfiles - 1;
    Profile p{};
    p.id = id;
    p.name = static_cast<char>('A' + id);
    p.symRate = kSymRates[id];
    p.robust = robust;
    if (robust) {
        p.mod = Mod::Qpsk;
        p.rate = LdpcRate::R1_2;
    } else {
        p.mod = id == 3 ? Mod::Qam64 : Mod::Qam16;
        p.rate = LdpcRate::R3_4;
    }
    p.bitsPerSym = static_cast<int>(p.mod);
    p.blockPayloadBytes = (kris::ldpcK(kBlockN, p.rate) - 16) / 8;
    p.dataSyms = kFrameCodedBits / p.bitsPerSym;
    p.regionSyms = regionMap(p.dataSyms).pilot.size() + p.dataSyms;
    p.frameSyms = kPreambleSyms + kHeaderCodedBits + p.regionSyms;
    p.sweepFrames = std::max(1, static_cast<int>(std::lround(kChirpPeriodSec * p.symRate / p.frameSyms)));
    return p;
}

// ---- header ---------------------------------------------------------------------------------------------------------------------------------------------------

static void putBits(std::vector<uint8_t>& b, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1);
}
static uint32_t getBits(const uint8_t* b, int& pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (b[pos++] & 1);
    return v;
}
static std::vector<uint8_t> packBytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) out[i / 8] |= static_cast<uint8_t>((bits[i] & 1) << (7 - i % 8));
    return out;
}

std::vector<uint8_t> headerBits(const FrameHeader& h) {
    std::vector<uint8_t> b;
    putBits(b, 0, 2);
    putBits(b, static_cast<uint32_t>(h.profile) & 3, 2);
    putBits(b, h.robust ? 1 : 0, 1);
    putBits(b, static_cast<uint32_t>(h.fec) & 7, 3);
    putBits(b, static_cast<uint32_t>(h.sweepIdx) & 1023, 10);
    putBits(b, static_cast<uint32_t>(h.seq) & 0xFFFF, 16);
    putBits(b, static_cast<uint32_t>(h.linkQ) & 255, 8);
    putBits(b, 0, 6);
    const auto bytes = packBytes(b);
    putBits(b, kris::crc16(bytes.data(), bytes.size()), 16);
    return b;
}

bool parseHeader(const uint8_t* bits, FrameHeader& h) {
    std::vector<uint8_t> first(bits, bits + 48);
    const auto bytes = packBytes(first);
    int pos = 48;
    if (getBits(bits, pos, 16) != kris::crc16(bytes.data(), bytes.size())) return false;
    pos = 0;
    if (getBits(bits, pos, 2) != 0) return false;
    h.profile = static_cast<int>(getBits(bits, pos, 2));
    h.robust = getBits(bits, pos, 1) != 0;
    h.fec = static_cast<int>(getBits(bits, pos, 3));
    h.sweepIdx = static_cast<int>(getBits(bits, pos, 10));
    h.seq = static_cast<int>(getBits(bits, pos, 16));
    h.linkQ = static_cast<int>(getBits(bits, pos, 8));
    return true;
}

std::vector<cf> headerSymbols(const FrameHeader& h) {
    const auto coded = kris::convEncode(0, headerBits(h), true);
    return kris::qamMap(coded.data(), coded.size(), Mod::Bpsk);
}

const std::vector<cf>& preambleSymbols() {
    static const std::vector<cf> zc = kris::zadoffChu(kZcRoot, kPreambleSyms);
    return zc;
}

// ---- region layout ----------------------------------------------------------------------------------------------------------------------------------------------

const RegionMap& regionMap(int dataSyms) {
    static std::mutex mu;
    static std::vector<std::pair<int, RegionMap>> cache;
    std::lock_guard<std::mutex> lk(mu);
    for (auto& e : cache)
        if (e.first == dataSyms) return e.second;
    RegionMap m;
    int placed = 0, j = 0;
    while (placed < dataSyms) {
        m.pilot.push_back(j++);
        for (int i = 0; i < kPilotGroup && placed < dataSyms; i++, placed++) m.data.push_back(j++);
    }
    m.pilot.push_back(j);
    cache.emplace_back(dataSyms, std::move(m));
    return cache.back().second;
}

float pilotValue(int i) {
    static const std::vector<float> v = [] {
        std::vector<float> t(2048);
        kris::XorShift64 g{0xA11CE0FFEE5ull};
        for (auto& x : t) x = (g.next() >> 40) & 1 ? -1.0f : 1.0f;
        return t;
    }();
    return v[static_cast<size_t>(i) % v.size()];
}

// ---- interleaver / scrambler -------------------------------------------------------------------------------------------------------------------------------

static const std::vector<int>& perm() {
    static const std::vector<int> p = [] {
        std::vector<int> t(kFrameCodedBits);
        for (int i = 0; i < kFrameCodedBits; i++) t[i] = i;
        kris::XorShift64 g{0x1920C0DEull};
        for (int i = kFrameCodedBits - 1; i > 0; i--) std::swap(t[i], t[g.next() % static_cast<uint64_t>(i + 1)]);
        return t;
    }();
    return p;
}
static const std::vector<uint8_t>& scramble() {
    static const std::vector<uint8_t> s = [] {
        std::vector<uint8_t> t(kFrameCodedBits);
        kris::XorShift64 g{0x5C4A3B1E2D0Full};
        for (auto& x : t) x = (g.next() >> 41) & 1;
        return t;
    }();
    return s;
}

void interleaveScramble(const uint8_t* in, uint8_t* out) {
    const auto& p = perm();
    const auto& s = scramble();
    for (int i = 0; i < kFrameCodedBits; i++) out[p[i]] = in[i];
    for (int i = 0; i < kFrameCodedBits; i++) out[i] ^= s[i];
}

void deinterleaveDescrambleLlr(const float* inTx, float* outCode) {
    const auto& p = perm();
    const auto& s = scramble();
    for (int i = 0; i < kFrameCodedBits; i++) {
        const float v = inTx[p[i]];
        outCode[i] = s[p[i]] ? -v : v;
    }
}

// ---- frame -------------------------------------------------------------------------------------------------------------------------------------------------------------

std::vector<uint8_t> frameCodedBits(const Profile& p, const uint8_t* payload) {
    const int k = kris::ldpcK(kBlockN, p.rate);
    std::vector<uint8_t> code(kFrameCodedBits), info(k);
    for (int b = 0; b < kBlocksPerFrame; b++) {
        const uint8_t* src = payload + b * p.blockPayloadBytes;
        const uint16_t crc = kris::crc16(src, p.blockPayloadBytes);
        for (int i = 0; i < p.blockPayloadBytes * 8; i++) info[i] = (src[i / 8] >> (7 - i % 8)) & 1;
        for (int i = 0; i < 16; i++) info[p.blockPayloadBytes * 8 + i] = (crc >> (15 - i)) & 1;
        kris::ldpcEncode(kBlockN, p.rate, info.data(), code.data() + b * kBlockN);
    }
    std::vector<uint8_t> tx(kFrameCodedBits);
    interleaveScramble(code.data(), tx.data());
    return tx;
}

std::vector<cf> assembleFrame(const Profile& p, const FrameHeader& h, const std::vector<cf>& dataSyms) {
    std::vector<cf> f;
    f.reserve(p.frameSyms);
    const auto& pre = preambleSymbols();
    f.insert(f.end(), pre.begin(), pre.end());
    const auto hs = headerSymbols(h);
    f.insert(f.end(), hs.begin(), hs.end());
    const auto& rm = regionMap(p.dataSyms);
    f.resize(f.size() + rm.pilot.size() + rm.data.size());
    const size_t base = kPreambleSyms + kHeaderCodedBits;
    for (size_t i = 0; i < rm.pilot.size(); i++) f[base + rm.pilot[i]] = cf(pilotValue(static_cast<int>(i)), 0.0f);
    for (size_t i = 0; i < rm.data.size(); i++) f[base + rm.data[i]] = dataSyms[i];
    return f;
}

std::vector<cf> frameSymbols(const Profile& p, const FrameHeader& h, const uint8_t* payload) {
    const auto bits = frameCodedBits(p, payload);
    return assembleFrame(p, h, kris::qamMap(bits.data(), bits.size(), p.mod));
}

// ---- chirp -------------------------------------------------------------------------------------------------------------------------------------------------------------

cf chirpUnit(double symPos, double periodSyms) {
    double u = std::fmod(symPos, periodSyms);
    if (u < 0) u += periodSyms;
    const double half = periodSyms / 2.0;
    double cyc;
    if (u < half) cyc = kChirpSpan * (u - 2.0 * u * u / periodSyms);
    else {
        const double v = u - half;
        cyc = kChirpSpan * (-v + 2.0 * v * v / periodSyms);
    }
    const double a = 6.283185307179586 * cyc;
#if defined(__GLIBC__)
    double sn, cs;
    sincos(a, &sn, &cs);   // glibc: the same values as sin and cos separately, one range reduction
    return cf(static_cast<float>(cs), static_cast<float>(sn));
#else
    return cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
#endif
}

// ---- transmitter -------------------------------------------------------------------------------------------------------------------------------------------------

Transmitter::Transmitter(int profileId, bool robust, bool chirp) : p_(profileOf(profileId, robust)), chirp_(chirp) {}

std::vector<cf> Transmitter::modulate(const std::vector<std::vector<uint8_t>>& frames, int fec, int linkQ, int startSeq, int startSweep, int leadSyms,
                                      int tailSyms) const {
    std::vector<cf> syms(static_cast<size_t>(leadSyms), cf(0, 0));
    for (size_t f = 0; f < frames.size(); f++) {
        FrameHeader h;
        h.profile = p_.id;
        h.robust = p_.robust;
        h.fec = fec;
        h.linkQ = linkQ;
        h.seq = (startSeq + static_cast<int>(f)) & 0xFFFF;
        h.sweepIdx = (startSweep + static_cast<int>(f)) % p_.sweepFrames;
        std::vector<uint8_t> pay = frames[f];
        pay.resize(static_cast<size_t>(framePayloadBytes(p_)), 0);
        const auto fs = frameSymbols(p_, h, pay.data());
        syms.insert(syms.end(), fs.begin(), fs.end());
    }
    syms.resize(syms.size() + static_cast<size_t>(tailSyms), cf(0, 0));
    auto h = kris::rrcTaps(kRrcBeta, kSps, kRrcSpan);
    const int c = static_cast<int>(h.size() / 2);
    std::vector<cf> out(syms.size() * kSps, cf(0, 0));
    const float g = std::sqrt(static_cast<float>(kSps));
    const long n = static_cast<long>(out.size());
    for (size_t k = 0; k < syms.size(); k++) {
        if (syms[k] == cf(0, 0)) continue;
        for (size_t i = 0; i < h.size(); i++) {
            const long idx = static_cast<long>(k) * kSps + static_cast<long>(i) - c;
            if (idx >= 0 && idx < n) out[idx] += syms[k] * (h[i] * g);
        }
    }
    if (chirp_) {
        const double period = static_cast<double>(p_.sweepFrames) * p_.frameSyms;
        const float amp = static_cast<float>(std::sqrt(kChirpPowerRel));
        for (size_t f = 0; f < frames.size(); f++) {
            const long s0 = static_cast<long>(leadSyms) + static_cast<long>(f) * p_.frameSyms;
            const int idx = (startSweep + static_cast<int>(f)) % p_.sweepFrames;
            for (long i = 0; i < static_cast<long>(p_.frameSyms) * kSps; i++) {
                const double pos = static_cast<double>(idx) * p_.frameSyms + static_cast<double>(i) / kSps;
                out[s0 * kSps + i] += amp * chirpUnit(pos, period);
            }
        }
    }
    return out;
}

}  // namespace dect2::kavb
