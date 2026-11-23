// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVL physical layer, transmit side (layout in kavl/include/kavl/kavl_phy.h).
#include "kavl/kavl_phy.h"
#include "atsc3/atsc3_ldpc.h"
#include "dect2/kris/kris_fec.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace kavl {

using dect2::kris::Mod;

namespace {

Modcod make(int id, const char* name, Mod mod, int rate15) {
    Modcod m{};
    m.id = id;
    m.name = name;
    m.mod = mod;
    m.bitsPerSym = static_cast<int>(mod);
    m.rate15 = rate15;
    m.blocks = m.bitsPerSym / 2;
    m.kBits = kLdpcN * rate15 / 15;
    m.payloadBytes = (m.kBits - 32) / 8;
    return m;
}

const std::array<Modcod, kNumModcods>& table() {
    static const std::array<Modcod, kNumModcods> t = {
        make(0, "QPSK 8/15", Mod::Qpsk, 8),          make(1, "16-QAM 9/15", Mod::Qam16, 9),        make(2, "64-QAM 10/15", Mod::Qam64, 10),
        make(3, "256-QAM 10/15", Mod::Qam256, 10),   make(4, "256-QAM 12/15", Mod::Qam256, 12),    make(5, "1024-QAM 10/15", Mod::Qam1024, 10),
        make(6, "1024-QAM 11/15", Mod::Qam1024, 11), make(7, "1024-QAM 12/15", Mod::Qam1024, 12),   make(8, "256-QAM 11/15", Mod::Qam256, 11),     make(9, "64-QAM 12/15", Mod::Qam64, 12),
    };
    return t;
}

// One permutation and one scrambling sequence per frame size (1 .. 5 code words).
struct BitMaps {
    std::vector<uint32_t> perm;   // transmitted index -> code index
    std::vector<uint8_t> scr;
};
const BitMaps& bitMaps(int nBits) {
    static const std::array<BitMaps, 5> maps = [] {
        std::array<BitMaps, 5> a;
        for (int b = 1; b <= 5; b++) {
            const int n = b * kLdpcN;
            BitMaps& m = a[static_cast<size_t>(b - 1)];
            m.perm.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; i++) m.perm[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
            dect2::kris::XorShift64 r{0x4B41564C00000000ull + static_cast<uint64_t>(b)};   // "KAVL"
            for (int i = n - 1; i > 0; i--) std::swap(m.perm[static_cast<size_t>(i)], m.perm[static_cast<size_t>(r.next() % static_cast<uint64_t>(i + 1))]);
            dect2::kris::XorShift64 s{0x5343524D4B41564Cull + static_cast<uint64_t>(b)};
            m.scr.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; i++) m.scr[static_cast<size_t>(i)] = static_cast<uint8_t>((s.next() >> 40) & 1u);
        }
        return a;
    }();
    const int b = nBits / kLdpcN;
    return maps[static_cast<size_t>(std::clamp(b, 1, 5) - 1)];
}

}  // namespace

const Modcod* modcod(int id) {
    if (id < 0 || id >= kNumModcods) return nullptr;
    return &table()[static_cast<size_t>(id)];
}

std::vector<uint8_t> headerBits(const Header& h) {
    std::vector<uint8_t> b;
    b.reserve(kHeaderInfoBits);
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) b.push_back(static_cast<uint8_t>((v >> i) & 1u));
    };
    put(0, 2);
    put(static_cast<uint32_t>(h.modcod) & 15u, 4);
    put(static_cast<uint32_t>(h.fec) & 7u, 3);
    put(static_cast<uint32_t>(h.layer) & 3u, 2);
    put(static_cast<uint32_t>(h.seq) & 0xFFFFu, 16);
    put(static_cast<uint32_t>(h.linkQ) & 0xFFu, 8);
    put(0, 13);
    uint8_t bytes[6] = {};
    for (int i = 0; i < 48; i++) bytes[i / 8] = static_cast<uint8_t>(bytes[i / 8] | (b[static_cast<size_t>(i)] << (7 - i % 8)));
    put(dect2::kris::crc16(bytes, 6), 16);
    return b;
}

bool parseHeader(const uint8_t* bits, Header& h) {
    uint8_t bytes[6] = {};
    for (int i = 0; i < 48; i++) bytes[i / 8] = static_cast<uint8_t>(bytes[i / 8] | ((bits[i] & 1) << (7 - i % 8)));
    uint32_t crc = 0;
    for (int i = 48; i < 64; i++) crc = (crc << 1) | (bits[i] & 1u);
    if (crc != dect2::kris::crc16(bytes, 6)) return false;
    auto get = [&](int at, int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | (bits[at + i] & 1u);
        return static_cast<int>(v);
    };
    if (get(0, 2) != 0) return false;
    h.modcod = get(2, 4);
    h.fec = get(6, 3);
    h.layer = get(9, 2);
    h.seq = get(11, 16);
    h.linkQ = get(27, 8);
    return true;
}

std::vector<cf> headerSymbols(const Header& h) {
    const auto coded = dect2::kris::convEncode(0, headerBits(h), true);
    std::vector<cf> s(coded.size());
    for (size_t i = 0; i < coded.size(); i++) s[i] = cf(coded[i] ? -1.f : 1.f, 0.f);
    return s;
}

const std::vector<cf>& preambleSymbols() {
    static const std::vector<cf> p = dect2::kris::zadoffChu(kZcRoot, kPreambleSyms);
    return p;
}

float pilotValue(int i) {
    static const std::vector<float> v = [] {
        std::vector<float> t(kPilots);
        dect2::kris::XorShift64 r{0x50494C4F544B4156ull};
        for (auto& x : t) x = ((r.next() >> 33) & 1u) ? -1.f : 1.f;
        return t;
    }();
    return v[static_cast<size_t>(std::clamp(i, 0, kPilots - 1))];
}

const RegionMap& regionMap() {
    static const RegionMap m = [] {
        RegionMap r;
        int idx = 0, left = kDataSyms;
        while (left > 0) {
            r.pilot.push_back(idx++);
            const int g = std::min(kPilotGroup, left);
            for (int i = 0; i < g; i++) r.data.push_back(idx++);
            left -= g;
        }
        r.pilot.push_back(idx++);
        return r;
    }();
    return m;
}

void interleaveScramble(const uint8_t* in, uint8_t* out, int nBits) {
    const BitMaps& m = bitMaps(nBits);
    for (int i = 0; i < nBits; i++) out[i] = static_cast<uint8_t>((in[m.perm[static_cast<size_t>(i)]] ^ m.scr[static_cast<size_t>(i)]) & 1u);
}

void deinterleaveDescrambleLlr(const float* inTx, float* outCode, int nBits) {
    const BitMaps& m = bitMaps(nBits);
    for (int i = 0; i < nBits; i++) outCode[m.perm[static_cast<size_t>(i)]] = m.scr[static_cast<size_t>(i)] ? -inTx[i] : inTx[i];
}

std::vector<uint8_t> frameCodedBits(const Modcod& m, const uint8_t* payload) {
    const auto& code = dect2::atsc3::ldpc16200(m.rate15);
    const int nBits = m.blocks * kLdpcN;
    std::vector<uint8_t> coded(static_cast<size_t>(nBits));
    for (int b = 0; b < m.blocks; b++) {
        const uint8_t* p = payload + static_cast<size_t>(b) * static_cast<size_t>(m.payloadBytes);
        std::vector<uint8_t> bits(static_cast<size_t>(m.kBits), 0);
        for (int i = 0; i < m.payloadBytes * 8; i++) bits[static_cast<size_t>(i)] = static_cast<uint8_t>((p[i / 8] >> (7 - i % 8)) & 1u);
        const uint32_t crc = dect2::kris::crc32(p, static_cast<size_t>(m.payloadBytes));
        for (int i = 0; i < 32; i++) bits[static_cast<size_t>(m.payloadBytes * 8 + i)] = static_cast<uint8_t>((crc >> (31 - i)) & 1u);
        code.encode(bits);   // extended to 16200
        std::copy(bits.begin(), bits.begin() + kLdpcN, coded.begin() + static_cast<long>(b) * kLdpcN);
    }
    std::vector<uint8_t> tx(coded.size());
    interleaveScramble(coded.data(), tx.data(), nBits);
    return tx;
}

std::vector<cf> assembleFrame(const Header& h, const std::vector<cf>& dataSyms) {
    std::vector<cf> f;
    f.reserve(kFrameSyms);
    const auto& pre = preambleSymbols();
    f.insert(f.end(), pre.begin(), pre.end());
    const auto hs = headerSymbols(h);
    f.insert(f.end(), hs.begin(), hs.end());
    std::vector<cf> region(kRegionSyms);
    const RegionMap& rm = regionMap();
    for (int i = 0; i < kPilots; i++) region[static_cast<size_t>(rm.pilot[static_cast<size_t>(i)])] = cf(pilotValue(i), 0.f);
    for (int i = 0; i < kDataSyms; i++) region[static_cast<size_t>(rm.data[static_cast<size_t>(i)])] = i < static_cast<int>(dataSyms.size()) ? dataSyms[static_cast<size_t>(i)] : cf(0, 0);
    f.insert(f.end(), region.begin(), region.end());
    return f;
}

std::vector<cf> frameSymbols(const Modcod& m, const Header& h, const uint8_t* payload) {
    const auto bits = frameCodedBits(m, payload);
    const auto data = dect2::kris::qamMap(bits.data(), bits.size(), m.mod);
    return assembleFrame(h, data);
}

const std::vector<float>& rrc() {
    static const std::vector<float> t = dect2::kris::rrcTaps(kRrcBeta, kSps, kRrcSpan);
    return t;
}

Transmitter::Transmitter(int modcodId) : m_(modcod(modcodId)) {
    if (!m_) m_ = modcod(0);
}

std::vector<cf> Transmitter::shape(const std::vector<cf>& syms) {
    const auto& taps = rrc();
    const int nt = static_cast<int>(taps.size()), half = nt / 2;
    const long nOut = static_cast<long>(syms.size()) * kSps;
    std::vector<cf> out(static_cast<size_t>(nOut), cf(0, 0));
    const float g = std::sqrt(static_cast<float>(kSps));   // unit-energy taps, one symbol per kSps samples -> power 1 per sample
    for (size_t s = 0; s < syms.size(); s++) {
        const cf v = syms[s] * g;
        if (v == cf(0, 0)) continue;
        const long c = static_cast<long>(s) * kSps;
        for (int k = 0; k < nt; k++) {
            const long i = c + k - half;
            if (i >= 0 && i < nOut) out[static_cast<size_t>(i)] += v * taps[static_cast<size_t>(k)];
        }
    }
    return out;
}

std::vector<cf> Transmitter::modulate(const std::vector<std::vector<uint8_t>>& frames, int fec, int linkQ, int startSeq, int leadSyms, int tailSyms) const {
    std::vector<cf> syms(static_cast<size_t>(std::max(0, leadSyms)), cf(0, 0));
    const size_t need = static_cast<size_t>(framePayloadBytes(*m_));
    int seq = startSeq;
    for (const auto& f : frames) {
        std::vector<uint8_t> pay(need, 0);
        std::memcpy(pay.data(), f.data(), std::min(need, f.size()));
        Header h;
        h.modcod = m_->id;
        h.fec = fec;
        h.seq = seq & 0xFFFF;
        h.linkQ = linkQ;
        const auto fs = frameSymbols(*m_, h, pay.data());
        syms.insert(syms.end(), fs.begin(), fs.end());
        seq++;
    }
    syms.insert(syms.end(), static_cast<size_t>(std::max(0, tailSyms)), cf(0, 0));
    return shape(syms);
}

}  // namespace kavl
