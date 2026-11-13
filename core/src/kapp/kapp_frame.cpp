// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP burst builder (see kapp_frame.h).
#include "dect2/kapp/kapp_frame.h"
#include <algorithm>
#include <cmath>

namespace dect2::kapp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kPow37_7 = 94931877133ull;   // 37^7

int charCode(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    return 36;   // '/' and anything else
}
char codeChar(int v) {
    if (v < 26) return (char)('A' + v);
    if (v < 36) return (char)('0' + (v - 26));
    return '/';
}

// bits of an unsigned value, MSB first
void putBits(std::vector<uint8_t>& b, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back((uint8_t)((v >> i) & 1u));
}
uint64_t getBits(const uint8_t* b, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | (uint64_t)(b[i] & 1u);
    return v;
}
}  // namespace

const Profile* profileById(int id) {
    if (id < 0 || id > kMaxProfileId) return nullptr;
    return &kProfiles[(size_t)id];
}

int symbolsPerBlock(const Profile& p) { return (kBlockN + p.bitsPerSym - 1) / p.bitsPerSym; }
int dataSymbols(const Profile& p, int nBlocks) { return nBlocks * symbolsPerBlock(p); }
int payloadSymbols(const Profile& p, int nBlocks) {
    const int d = dataSymbols(p, nBlocks);
    return d + d / kPilotGroup;
}
int burstSymbols(const Profile& p, int nBlocks) { return kFixedOverheadSyms + payloadSymbols(p, nBlocks); }
int maxBlocksForSymbols(const Profile& p, long burstSyms) {
    if (burstSyms <= kFixedOverheadSyms) return 0;
    const double n = (double)(burstSyms - kFixedOverheadSyms) * kPilotGroup / kPilotPeriod / symbolsPerBlock(p);
    return (int)std::floor(n);
}
int maxBlocksForMs(const Profile& p, double ms) { return maxBlocksForSymbols(p, (long)std::floor(ms * 1e-3 * kSymRate)); }

bool profileTableConsistent() {
    for (const Profile& p : kProfiles) {
        if (kris::ldpcK(kBlockN, p.rate) != infoBitsPerBlock(p)) return false;
    }
    return true;
}

// ---- callsign ----
uint64_t encodeCallsign(const std::string& call) {
    uint64_t v = 0;
    for (size_t i = 0; i < 7; ++i) v = v * 37u + (uint64_t)(i < call.size() ? charCode(call[i]) : 36);
    return v;
}
bool callsignCodeValid(uint64_t v) { return v < kPow37_7; }
std::string decodeCallsign(uint64_t v) {
    if (!callsignCodeValid(v)) return std::string();
    char s[7];
    for (int i = 6; i >= 0; --i) {
        s[i] = codeChar((int)(v % 37u));
        v /= 37u;
    }
    size_t n = 7;
    while (n > 0 && s[n - 1] == '/') --n;
    return std::string(s, n);
}

// ---- header ----
std::vector<uint8_t> packBits(const uint8_t* bits, size_t n) {
    std::vector<uint8_t> out((n + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) {
        if (bits[i] & 1u) out[i / 8] = (uint8_t)(out[i / 8] | (0x80u >> (i % 8)));
    }
    return out;
}

std::vector<uint8_t> headerInfoBits(const BurstHeader& h) {
    std::vector<uint8_t> b;
    b.reserve(kHeaderInfoBits);
    putBits(b, encodeCallsign(h.callsign), 40);
    putBits(b, (uint64_t)(h.profile & 15), 4);
    putBits(b, (uint64_t)(h.blocks & 1023), 10);
    putBits(b, (uint64_t)(h.seq & 63), 6);
    putBits(b, (uint64_t)(h.flags & 15), 4);
    const std::vector<uint8_t> bytes = packBits(b.data(), b.size());
    putBits(b, kris::crc16(bytes.data(), bytes.size()), 16);
    return b;
}

bool parseHeaderBits(const uint8_t* bits, BurstHeader& h) {
    const std::vector<uint8_t> bytes = packBits(bits, 64);
    if ((uint64_t)kris::crc16(bytes.data(), bytes.size()) != getBits(bits + 64, 16)) return false;
    const uint64_t call = getBits(bits, 40);
    if (!callsignCodeValid(call)) return false;
    h.callsign = decodeCallsign(call);
    h.profile = (int)getBits(bits + 40, 4);
    h.blocks = (int)getBits(bits + 44, 10);
    h.seq = (int)getBits(bits + 54, 6);
    h.flags = (int)getBits(bits + 60, 4);
    return true;
}

std::vector<cf> headerSymbols(const BurstHeader& h) {
    const std::vector<uint8_t> info = headerInfoBits(h);
    const std::vector<uint8_t> coded = kris::convEncode(0, info, true);
    std::vector<cf> s;
    s.reserve(coded.size() * kHeaderRepeat);
    for (int r = 0; r < kHeaderRepeat; ++r) {
        for (uint8_t c : coded) s.push_back(cf{c ? -1.f : 1.f, 0.f});
    }
    return s;
}

// ---- payload ----
std::vector<uint8_t> scramblerSequence(int blockIdx, int nBits) {
    kris::XorShift64 g{0x9E3779B97F4A7C15ull * (uint64_t)(blockIdx + 1) ^ 0x4B4150505F534352ull};
    std::vector<uint8_t> out((size_t)nBits);
    uint64_t w = 0;
    for (int i = 0; i < nBits; ++i) {
        if ((i & 63) == 0) w = g.next();
        out[(size_t)i] = (uint8_t)((w >> (i & 63)) & 1u);
    }
    return out;
}

const std::vector<cf>& preambleSymbols() {
    static const std::vector<cf> pre = [] {
        std::vector<cf> p = kris::zadoffChu(kZcRoot1, kZcLen);
        const std::vector<cf> q = kris::zadoffChu(kZcRoot2, kZcLen);
        p.insert(p.end(), q.begin(), q.end());
        return p;
    }();
    return pre;
}

const std::vector<float>& rrcShape() {
    static const std::vector<float> h = [] {
        std::vector<float> t = kris::rrcTaps(kRrcBeta, kSps, kRrcSpan);
        double e = 0;
        for (float v : t) e += (double)v * v;
        const float sc = e > 0 ? (float)std::sqrt((double)kSps / e) : 1.f;
        for (float& v : t) v *= sc;
        return t;
    }();
    return h;
}

namespace {
bool encodeBlock(const Profile& p, int blockIdx, const uint8_t* data, std::vector<cf>& out) {
    const int K = infoBitsPerBlock(p);
    if (kris::ldpcK(kBlockN, p.rate) != K) return false;
    std::vector<uint8_t> info((size_t)K);
    for (int i = 0; i < p.payloadBytes; ++i) {
        for (int b = 0; b < 8; ++b) info[(size_t)(i * 8 + b)] = (uint8_t)((data[i] >> (7 - b)) & 1u);
    }
    const uint16_t crc = kris::crc16(data, (size_t)p.payloadBytes);
    for (int b = 0; b < 16; ++b) info[(size_t)(p.payloadBytes * 8 + b)] = (uint8_t)((crc >> (15 - b)) & 1u);
    std::vector<uint8_t> cw((size_t)kBlockN);
    if (!kris::ldpcEncode(kBlockN, p.rate, info.data(), cw.data())) return false;
    const std::vector<uint8_t> pn = scramblerSequence(blockIdx, kBlockN);
    for (size_t i = 0; i < cw.size(); ++i) cw[i] = (uint8_t)((cw[i] ^ pn[i]) & 1u);
    cw.resize((size_t)symbolsPerBlock(p) * (size_t)p.bitsPerSym, 0);
    const std::vector<cf> sy = kris::qamMap(cw.data(), cw.size(), p.mod);
    out.insert(out.end(), sy.begin(), sy.end());
    return true;
}

// symbol rate -> internal rate: zero stuffing + RRC (polyphase: each symbol adds its scaled pulse)
std::vector<cf> shapeSymbols(const std::vector<cf>& s) {
    const std::vector<float>& h = rrcShape();
    if (s.empty() || h.empty()) return {};
    std::vector<cf> out((s.size() - 1) * (size_t)kSps + h.size(), cf{0.f, 0.f});
    for (size_t k = 0; k < s.size(); ++k) {
        const float re = s[k].real(), im = s[k].imag();
        cf* o = out.data() + k * (size_t)kSps;
        for (size_t j = 0; j < h.size(); ++j) o[j] += cf{re * h[j], im * h[j]};
    }
    return out;
}
}  // namespace

std::vector<cf> buildBurstSymbols(const BurstSpec& spec) {
    const Profile* P = profileById(spec.profileId);
    if (!P || spec.payload.empty()) return {};
    const size_t bp = (size_t)P->payloadBytes;
    const size_t nBlocks = (spec.payload.size() + bp - 1) / bp;
    if (nBlocks < 1 || nBlocks > (size_t)kMaxBlocks) return {};

    std::vector<cf> data;
    data.reserve(nBlocks * (size_t)symbolsPerBlock(*P));
    std::vector<uint8_t> blk(bp);
    for (size_t b = 0; b < nBlocks; ++b) {
        std::fill(blk.begin(), blk.end(), (uint8_t)0);
        const size_t from = b * bp;
        const size_t take = std::min(bp, spec.payload.size() - from);
        std::copy(spec.payload.begin() + (long)from, spec.payload.begin() + (long)(from + take), blk.begin());
        if (!encodeBlock(*P, (int)b, blk.data(), data)) return {};
    }

    BurstHeader hdr;
    hdr.callsign = spec.callsign;
    hdr.profile = spec.profileId;
    hdr.blocks = (int)nBlocks;
    hdr.seq = spec.seq;
    hdr.flags = spec.flags;
    const std::vector<cf> hs = headerSymbols(hdr);
    const std::vector<cf>& pre = preambleSymbols();
    if (hs.size() != (size_t)kHeaderSyms || pre.size() != (size_t)kPreambleSyms) return {};

    std::vector<cf> s;
    s.reserve((size_t)kFixedOverheadSyms + data.size() + data.size() / kPilotGroup + 2);
    for (int k = 0; k < kRampSyms; ++k) {
        const float w = std::sin(0.5f * (float)kPi * ((float)k + 0.5f) / (float)kRampSyms);
        s.push_back(pre[0] * (w * w));
    }
    s.insert(s.end(), pre.begin(), pre.end());
    s.insert(s.end(), hs.begin(), hs.end());
    int cnt = 0;
    for (const cf& d : data) {
        s.push_back(d);
        if (++cnt == kPilotGroup) {
            s.push_back(cf{1.f, 0.f});
            cnt = 0;
        }
    }
    for (int k = 0; k < kRampSyms; ++k) {
        const float w = std::cos(0.5f * (float)kPi * ((float)k + 0.5f) / (float)kRampSyms);
        s.push_back(cf{w * w, 0.f});
    }
    return s;
}

std::vector<cf> buildBurst(const BurstSpec& spec) { return shapeSymbols(buildBurstSymbols(spec)); }

// ---- ACK ----
std::vector<uint8_t> encodeAckPayload(float snrDb, const std::vector<uint8_t>& blockOk) {
    std::vector<uint8_t> bits;
    bits.reserve(560);
    int code = (int)std::lround(snrDb) + 10;
    code = std::max(0, std::min(63, code));
    putBits(bits, (uint64_t)code, 6);
    for (int i = 0; i < kAckMaxBlocks; ++i) bits.push_back((size_t)i < blockOk.size() ? (uint8_t)(blockOk[(size_t)i] ? 1 : 0) : (uint8_t)0);
    bits.resize(560, 0);
    return packBits(bits.data(), bits.size());
}

bool decodeAckPayload(const uint8_t* data, size_t n, float& snrDb, std::vector<uint8_t>& bitmap512) {
    if (n < 70) return false;
    auto bit = [&](size_t i) { return (uint8_t)((data[i / 8] >> (7 - i % 8)) & 1u); };
    int code = 0;
    for (size_t i = 0; i < 6; ++i) code = (code << 1) | bit(i);
    snrDb = (float)(code - 10);
    bitmap512.assign((size_t)kAckMaxBlocks, 0);
    for (size_t i = 0; i < (size_t)kAckMaxBlocks; ++i) bitmap512[i] = bit(6 + i);
    return true;
}

BurstSpec ackSpec(const AckInfo& a) {
    BurstSpec s;
    s.callsign = a.callsign;
    s.profileId = 0;
    s.seq = a.seq;
    s.flags = kFlagAck;
    s.payload = encodeAckPayload(a.snrDb, a.blockOk);
    return s;
}
std::vector<cf> buildAck(const AckInfo& a) { return buildBurst(ackSpec(a)); }

// ---- resampler ----
namespace {
constexpr int kResTable = 256;   // kernel points per input sample
}

Resampler::Resampler(double inRate, double outRate) {
    step_ = inRate / outRate;
    const double cut = std::min(0.95, 0.95 * outRate / inRate);
    half_ = (int)std::ceil(16.0 / cut);
    const size_t n = (size_t)(2 * half_) * (size_t)kResTable + 1;
    table_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const double x = ((double)i / kResTable) - half_;
        const double u = cut * x;
        const double sinc = std::fabs(u) < 1e-12 ? 1.0 : std::sin(kPi * u) / (kPi * u);
        const double w = x / half_;
        const double win = 0.42 + 0.5 * std::cos(kPi * w) + 0.08 * std::cos(2.0 * kPi * w);
        table_[i] = (float)(cut * sinc * win);
    }
}

std::vector<cf> Resampler::process(const cf* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    std::vector<cf> out;
    for (;;) {
        const long long i0 = (long long)std::floor(t_);
        const long long first = i0 - half_ + 1;
        const long long last = i0 + half_;
        if (last >= base_ + (long long)buf_.size()) break;
        float re = 0.f, im = 0.f;
        for (long long i = first; i <= last; ++i) {
            if (i < base_) continue;
            const double pos = ((double)i - t_ + half_) * kResTable;
            const long idx = std::lround(pos);
            if (idx < 0 || idx >= (long)table_.size()) continue;
            const float k = table_[(size_t)idx];
            const cf& v = buf_[(size_t)(i - base_)];
            re += k * v.real();
            im += k * v.imag();
        }
        out.push_back(cf{re, im});
        t_ += step_;
    }
    const long long keepFrom = (long long)std::floor(t_) - half_ + 1;
    if (keepFrom > base_) {
        const size_t drop = (size_t)std::min<long long>(keepFrom - base_, (long long)buf_.size());
        buf_.erase(buf_.begin(), buf_.begin() + (long)drop);
        base_ += (long long)drop;
    }
    return out;
}

std::vector<cf> Resampler::flush() {
    const std::vector<cf> z((size_t)(2 * half_ + 2), cf{0.f, 0.f});
    return process(z.data(), z.size());
}

std::vector<cf> resample(const std::vector<cf>& x, double inRate, double outRate) {
    Resampler r(inRate, outRate);
    std::vector<cf> y = r.process(x.data(), x.size());
    const std::vector<cf> tail = r.flush();
    y.insert(y.end(), tail.begin(), tail.end());
    return y;
}

}  // namespace dect2::kapp
