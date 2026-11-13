// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT physical layer: block packing, header, preamble, frame / burst builder, RRC shaping, resampler. See kaft_frame.h.
#include "dect2/kaft/kaft_frame.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace dect2::kaft {

// ---------------------------------------------------------------- small helpers

std::vector<uint8_t> packBits(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++)
        if (bits[i] & 1) out[i >> 3] = (uint8_t)(out[i >> 3] | (0x80u >> (i & 7)));
    return out;
}

std::vector<uint8_t> unpackBits(const uint8_t* bytes, size_t nBytes) {
    std::vector<uint8_t> out(nBytes * 8);
    for (size_t i = 0; i < nBytes * 8; i++) out[i] = (uint8_t)((bytes[i >> 3] >> (7 - (i & 7))) & 1u);
    return out;
}

static void putBits(std::vector<uint8_t>& v, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)((val >> i) & 1u));
}

static uint32_t getBits(const std::vector<uint8_t>& v, size_t& pos, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r = (r << 1) | (uint32_t)(pos + (size_t)i < v.size() ? (v[pos + (size_t)i] & 1u) : 0u);
    pos += (size_t)n;
    return r;
}

static const char kCallChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/-.";   // index + 1 is the 6-bit code, 0 = end of callsign

std::vector<uint8_t> callsignBits(const std::string& cs) {
    std::vector<uint8_t> bits;
    for (int i = 0; i < 6; i++) {
        uint32_t code = 0;
        if ((size_t)i < cs.size()) {
            char c = cs[(size_t)i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            const char* p = std::strchr(kCallChars, c);
            code = (p && c != 0) ? (uint32_t)(p - kCallChars) + 1u : (uint32_t)(std::strchr(kCallChars, '-') - kCallChars) + 1u;
        }
        putBits(bits, code, 6);
    }
    return bits;
}

std::string callsignFromBits(const uint8_t* bits) {
    std::string s;
    for (int i = 0; i < 6; i++) {
        uint32_t code = 0;
        for (int b = 0; b < 6; b++) code = (code << 1) | (uint32_t)(bits[i * 6 + b] & 1u);
        if (code == 0) break;
        if (code - 1 < sizeof(kCallChars) - 1) s.push_back(kCallChars[code - 1]);
    }
    return s;
}

// ---------------------------------------------------------------- block accounting

int bitsPerSymbol(int profile) { return (int)profileInfo(profile).mod; }

int blockInfoBits(int profile) { return kris::ldpcK(kBlockBits, profileInfo(profile).rate); }

int payloadBytes(int profile) { return (blockInfoBits(profile) - kCrcBits) / 8; }

int dataBytesPerBlock(int profile) { return payloadBytes(profile) - kBlockHdrBytes; }

int symbolsPerBlock(int profile) { return kBlockBits / bitsPerSymbol(profile); }

int payloadSymbols(int profile, int nBlocks) {
    const int d = nBlocks * symbolsPerBlock(profile);
    return d + (d + (kPilotPeriod - 2)) / (kPilotPeriod - 1);   // one pilot in front of every group of 31 data symbols
}

int frameSymbolCount(int profile, int nBlocks) { return kPreambleSyms + kHeaderSyms + payloadSymbols(profile, nBlocks); }

int maxBlocksPerFrame(int profile, Width w) {
    const int budget = widthInfo(w).frameSymbols;
    int nb = 1;
    while (frameSymbolCount(profile, nb + 1) <= budget) nb++;
    return nb;
}

double frameSeconds(Width w, int profile, int nBlocks) {
    return (double)frameSymbolCount(profile, nBlocks) / (double)widthInfo(w).symRate;
}

// ---------------------------------------------------------------- block coding

uint64_t blockScramblerSeed(uint16_t frameNo, int blockIdx) {
    uint64_t s = 0x4B41465400000000ULL ^ ((uint64_t)frameNo << 24) ^ ((uint64_t)(uint32_t)blockIdx << 8) ^ 0x9E3779B97F4A7C15ULL;
    s *= 0xD6E8FEB86659FD93ULL;
    if (s == 0) s = 1;
    return s;
}

static void scramble(std::vector<uint8_t>& bits, uint16_t frameNo, int blockIdx) {
    kris::XorShift64 rng{blockScramblerSeed(frameNo, blockIdx)};
    for (int i = 0; i < 4; i++) rng.next();
    uint64_t word = 0;
    for (size_t i = 0; i < bits.size(); i++) {
        if ((i & 63) == 0) word = rng.next();
        bits[i] = (uint8_t)(bits[i] ^ (uint8_t)((word >> (i & 63)) & 1u));
    }
}

std::vector<uint8_t> encodeBlock(int profile, uint16_t frameNo, int blockIdx, const uint8_t* payload, size_t len) {
    const int K = blockInfoBits(profile);
    const size_t P = (size_t)payloadBytes(profile);
    std::vector<uint8_t> bytes(P + 4, 0);
    if (payload && len) std::memcpy(bytes.data(), payload, std::min(len, P));
    const uint32_t crc = kris::crc32(bytes.data(), P);
    for (int i = 0; i < 4; i++) bytes[P + (size_t)i] = (uint8_t)(crc >> (8 * i));
    std::vector<uint8_t> info = unpackBits(bytes.data(), bytes.size());
    info.resize((size_t)K, 0);
    scramble(info, frameNo, blockIdx);
    std::vector<uint8_t> cw((size_t)kBlockBits, 0);
    kris::ldpcEncode(kBlockBits, profileInfo(profile).rate, info.data(), cw.data());
    return cw;
}

bool decodeBlock(int profile, uint16_t frameNo, int blockIdx, const float* llr, std::vector<uint8_t>& payload) {
    const int K = blockInfoBits(profile);
    const size_t P = (size_t)payloadBytes(profile);
    std::vector<uint8_t> info((size_t)K, 0);
    kris::ldpcDecode(kBlockBits, profileInfo(profile).rate, llr, info.data(), 50);
    scramble(info, frameNo, blockIdx);
    const std::vector<uint8_t> bytes = packBits(info);
    if (bytes.size() < P + 4) return false;
    const uint32_t want = kris::crc32(bytes.data(), P);
    uint32_t got = 0;
    for (int i = 0; i < 4; i++) got |= (uint32_t)bytes[P + (size_t)i] << (8 * i);
    if (want != got) return false;
    payload.assign(bytes.begin(), bytes.begin() + (long)P);
    return true;
}

std::vector<uint8_t> packDataBlock(uint32_t offset, const uint8_t* data, size_t len) {
    std::vector<uint8_t> b((size_t)kBlockHdrBytes + len, 0);
    for (int i = 0; i < 4; i++) b[(size_t)i] = (uint8_t)(offset >> (8 * i));
    b[4] = (uint8_t)(len & 0xFF);
    b[5] = (uint8_t)((len >> 8) & 0xFF);
    if (len) std::memcpy(b.data() + kBlockHdrBytes, data, len);
    return b;
}

bool unpackDataBlock(const std::vector<uint8_t>& payload, uint32_t& offset, std::vector<uint8_t>& data) {
    if (payload.size() < (size_t)kBlockHdrBytes) return false;
    offset = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    const size_t len = (size_t)payload[4] | ((size_t)payload[5] << 8);
    if (len > payload.size() - (size_t)kBlockHdrBytes) return false;
    data.assign(payload.begin() + kBlockHdrBytes, payload.begin() + (long)kBlockHdrBytes + (long)len);
    return true;
}

// ---------------------------------------------------------------- header

std::vector<uint8_t> encodeHeaderBits(const FrameHeader& h) {
    std::vector<uint8_t> b;
    putBits(b, (uint32_t)h.width, 2);
    putBits(b, (uint32_t)h.profile, 4);
    putBits(b, (uint32_t)h.type, 2);
    putBits(b, h.session, 16);
    putBits(b, h.frameNo, 16);
    putBits(b, h.nBlocks, 8);
    putBits(b, h.frameInBurst & 15u, 4);
    putBits(b, h.lastInBurst ? 1u : 0u, 1);
    const std::vector<uint8_t> cs = callsignBits(h.callsign);
    b.insert(b.end(), cs.begin(), cs.end());
    const std::vector<uint8_t> bytes = packBits(b);
    putBits(b, kris::crc16(bytes.data(), bytes.size()), 16);
    b.resize((size_t)kHeaderInfoBits, 0);
    return b;
}

bool decodeHeaderBits(const std::vector<uint8_t>& bits, FrameHeader& h) {
    constexpr size_t kBody = 89;
    if (bits.size() < kBody + 16) return false;
    const std::vector<uint8_t> body(bits.begin(), bits.begin() + (long)kBody);
    const std::vector<uint8_t> bytes = packBits(body);
    size_t pos = kBody;
    const uint32_t crc = getBits(bits, pos, 16);
    if (crc != (uint32_t)kris::crc16(bytes.data(), bytes.size())) return false;
    pos = 0;
    const uint32_t w = getBits(bits, pos, 2);
    if (w > 2) return false;
    h.width = (Width)w;
    h.profile = (int)getBits(bits, pos, 4);
    if (h.profile >= kNumProfiles) return false;
    const uint32_t t = getBits(bits, pos, 2);
    if (t > 2) return false;
    h.type = (FrameType)t;
    h.session = (uint16_t)getBits(bits, pos, 16);
    h.frameNo = (uint16_t)getBits(bits, pos, 16);
    h.nBlocks = (uint8_t)getBits(bits, pos, 8);
    h.frameInBurst = (uint8_t)getBits(bits, pos, 4);
    h.lastInBurst = getBits(bits, pos, 1) != 0;
    h.callsign = callsignFromBits(bits.data() + pos);
    return true;
}

std::vector<cf> headerSymbols(const FrameHeader& h) {
    std::vector<uint8_t> coded = kris::convEncode(0, encodeHeaderBits(h), true);
    coded.resize((size_t)kHeaderSyms, 0);
    return kris::qamMap(coded.data(), coded.size(), kris::Mod::Bpsk);
}

// ---------------------------------------------------------------- preamble

const std::vector<cf>& preambleSymbols(Width w) {
    static std::vector<cf> tab[kNumWidths];
    static bool init = false;
    if (!init) {   // built on first use; the table is tiny, a benign race would only compute the same values
        for (int i = 0; i < kNumWidths; i++) {
            const std::vector<cf> zc = kris::zadoffChu(kWidths[i].zcRoot, kZcLen);
            std::vector<cf> p;
            p.reserve((size_t)kPreambleSyms);
            for (int k = kZcLen - kZcGuard; k < kZcLen; k++) p.push_back(zc[(size_t)k]);
            p.insert(p.end(), zc.begin(), zc.end());
            p.insert(p.end(), zc.begin(), zc.end());
            tab[i] = std::move(p);
        }
        init = true;
    }
    return tab[(int)w];
}

// ---------------------------------------------------------------- pilots, shaping

std::vector<cf> pilotSymbols(size_t n) {
    kris::XorShift64 rng{0xA5C3961F2B4D7E81ULL};
    std::vector<cf> p(n);
    uint64_t word = 0;
    for (size_t i = 0; i < n; i++) {
        if ((i & 63) == 0) word = rng.next();
        p[i] = cf(((word >> (i & 63)) & 1u) ? -1.0f : 1.0f, 0.0f);
    }
    return p;
}

const std::vector<float>& rrcFilter() {
    static const std::vector<float> taps = [] {
        std::vector<float> t = kris::rrcTaps(kRrcBeta, kSps, kRrcSpan);
        double e = 0;
        for (float v : t) e += (double)v * v;
        const float s = e > 0 ? (float)std::sqrt((double)kSps / e) : 1.0f;
        for (float& v : t) v *= s;
        return t;
    }();
    return taps;
}

int rrcDelaySamples() { return (int)(rrcFilter().size() - 1) / 2; }

std::vector<cf> rrcShape(const std::vector<cf>& symbols) {
    const std::vector<float>& h = rrcFilter();
    const size_t T = h.size();
    std::vector<cf> out(symbols.size() * (size_t)kSps + T - 1, cf(0, 0));
    for (size_t i = 0; i < symbols.size(); i++) {
        const float re = symbols[i].real(), im = symbols[i].imag();
        cf* o = out.data() + i * (size_t)kSps;
        for (size_t k = 0; k < T; k++) o[k] += cf(re * h[k], im * h[k]);
    }
    return out;
}

// ---------------------------------------------------------------- frames

std::vector<cf> frameSymbols(const FrameSpec& f) {
    const int nb = (int)f.blocks.size();
    const int profile = f.header.profile;
    if (profile < 0 || profile >= kNumProfiles || nb < 1 || nb > 255) return {};
    const int limit = f.header.type == FrameType::Data ? maxBlocksPerFrame(profile, f.header.width) : kCtrlMaxBlocks;
    if (nb > limit) return {};
    FrameHeader h = f.header;
    h.nBlocks = (uint8_t)nb;
    const kris::Mod mod = profileInfo(profile).mod;
    std::vector<cf> data;
    data.reserve((size_t)nb * (size_t)symbolsPerBlock(profile));
    for (int b = 0; b < nb; b++) {
        const std::vector<uint8_t>& pl = f.blocks[(size_t)b];
        const std::vector<uint8_t> bits = encodeBlock(profile, h.frameNo, b, pl.data(), pl.size());
        const std::vector<cf> s = kris::qamMap(bits.data(), bits.size(), mod);
        data.insert(data.end(), s.begin(), s.end());
    }
    std::vector<cf> out;
    out.reserve((size_t)frameSymbolCount(profile, nb));
    const std::vector<cf>& pre = preambleSymbols(h.width);
    out.insert(out.end(), pre.begin(), pre.end());
    const std::vector<cf> hs = headerSymbols(h);
    out.insert(out.end(), hs.begin(), hs.end());
    const size_t groups = (data.size() + (size_t)kPilotPeriod - 2) / (size_t)(kPilotPeriod - 1);
    const std::vector<cf> pil = pilotSymbols(groups);
    size_t pos = 0;
    for (size_t g = 0; g < groups; g++) {
        out.push_back(pil[g]);
        const size_t take = std::min((size_t)kPilotPeriod - 1, data.size() - pos);
        out.insert(out.end(), data.begin() + (long)pos, data.begin() + (long)(pos + take));
        pos += take;
    }
    return out;
}

std::vector<cf> buildFrame(const FrameSpec& f) {
    const std::vector<cf> s = frameSymbols(f);
    if (s.empty()) return {};
    return rrcShape(s);
}

std::vector<cf> buildBurst(const std::vector<FrameSpec>& frames) {
    std::vector<cf> all;
    for (const FrameSpec& f : frames) {
        const std::vector<cf> s = frameSymbols(f);
        all.insert(all.end(), s.begin(), s.end());
    }
    if (all.empty()) return {};
    return rrcShape(all);
}

// ---------------------------------------------------------------- resampler

static int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

std::vector<cf> resample(const std::vector<cf>& x, int up, int down) {
    if (up < 1 || down < 1) return {};
    const int g = std::gcd(up, down);
    up /= g;
    down /= g;
    if (up == 1 && down == 1) return x;
    const int mx = std::max(up, down);
    const int H = 6 * mx;   // half length of the low-pass on the up-rate grid
    std::vector<float> tap((size_t)(2 * H + 1));
    for (int d = -H; d <= H; d++) {
        const double xx = (double)d / mx;
        const double sinc = d == 0 ? 1.0 : std::sin(M_PI * xx) / (M_PI * xx);
        const double win = 0.42 + 0.5 * std::cos(M_PI * (double)d / H) + 0.08 * std::cos(2.0 * M_PI * (double)d / H);
        tap[(size_t)(d + H)] = (float)((double)up / mx * sinc * win);
    }
    const int64_t N = (int64_t)x.size();
    const size_t nOut = (size_t)((uint64_t)x.size() * (uint64_t)up / (uint64_t)down);
    std::vector<cf> out(nOut);
    for (size_t m = 0; m < nOut; m++) {
        const int64_t pos = (int64_t)m * down;
        int64_t k0 = floorDiv(pos - H + up - 1, up);
        int64_t k1 = floorDiv(pos + H, up);
        k0 = std::max<int64_t>(k0, 0);
        k1 = std::min<int64_t>(k1, N - 1);
        float re = 0, im = 0;
        for (int64_t k = k0; k <= k1; k++) {
            const float t = tap[(size_t)(pos - k * up + H)];
            re += t * x[(size_t)k].real();
            im += t * x[(size_t)k].imag();
        }
        out[m] = cf(re, im);
    }
    return out;
}

std::vector<cf> toReferenceRate(const std::vector<cf>& x4sps, Width w, const ResampleFn& fn) {
    const int f = widthInfo(w).refFactor;
    return fn ? fn(x4sps, f, 1) : resample(x4sps, f, 1);
}

}  // namespace dect2::kaft
