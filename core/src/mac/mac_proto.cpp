// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC/packet codes and tables (see mac_proto.h).
#include "dect2/mac/mac_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace mac {

constexpr double kPiP = dect2::math::kPi;
const char* variantName(int v) { return v == kD ? "D-MAC" : "D2-MAC"; }

Iir1 shelf(double fz, double fp, double gain, double fs) {
    // H(s) = gain (1 + s/wz) / (1 + s/wp), s = K (1 - z^-1) / (1 + z^-1), K = 2 fs
    const double K = 2 * fs, wz = 2 * kPiP * fz, wp = 2 * kPiP * fp;
    const double n0 = gain * (1 + K / wz), n1 = gain * (1 - K / wz), d0 = 1 + K / wp, d1 = 1 - K / wp;
    Iir1 f;
    f.b0 = (float)(n0 / d0); f.b1 = (float)(n1 / d0); f.a1 = (float)(d1 / d0);
    return f;
}
Iir1 j17Pre(double fs) { const double w1 = 3000, w2 = 3000 * std::sqrt(75.0); return shelf(w1 / (2 * kPiP), w2 / (2 * kPiP), 1.0 / std::sqrt(75.0), fs); }
Iir1 j17De(double fs) { const double w1 = 3000, w2 = 3000 * std::sqrt(75.0); return shelf(w2 / (2 * kPiP), w1 / (2 * kPiP), std::sqrt(75.0), fs); }

int lswBit(bool w1, int i) { const int b = (kLswW1 >> (5 - i)) & 1; return w1 ? b : 1 - b; }

int fsdBit(int i) {
    static const uint8_t seq[12] = {0x55, 0x55, 0x55, 0x55, 0x65, 0xAE, 0xF3, 0x15, 0x3F, 0x41, 0xC2, 0x46};
    return (seq[i >> 3] >> (7 - (i & 7))) & 1;
}

// ---------------------------------------------------------------- Golay (23,12)
namespace {
constexpr uint32_t kGolayG = 0xC75;   // x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1
uint32_t polyMod(uint32_t v, int deg, uint32_t g, int gdeg) {   // v of degree < deg
    for (int b = deg - 1; b >= gdeg; b--) if (v >> b & 1) v ^= g << (b - gdeg);
    return v;
}
struct GolayTable {
    std::vector<uint32_t> err;   // syndrome -> error pattern (23 bits, bit 22 = first transmitted)
    GolayTable() : err(2048, 0xFFFFFFFFu) {
        err[0] = 0;
        auto put = [&](uint32_t e) { const uint32_t s = polyMod(e, 23, kGolayG, 11); if (err[s] == 0xFFFFFFFFu || __builtin_popcount(err[s]) > __builtin_popcount(e)) err[s] = e; };
        for (int a = 0; a < 23; a++) {
            put(1u << a);
            for (int b = a + 1; b < 23; b++) {
                put((1u << a) | (1u << b));
                for (int c = b + 1; c < 23; c++) put((1u << a) | (1u << b) | (1u << c));
            }
        }
    }
};
const GolayTable& golayTable() { static const GolayTable t; return t; }
} // namespace

void golay23Encode(const uint8_t info[12], uint8_t out[23]) {
    uint32_t m = 0;
    for (int i = 0; i < 12; i++) m = (m << 1) | (info[i] & 1);
    const uint32_t r = polyMod(m << 11, 23, kGolayG, 11);
    const uint32_t c = (m << 11) | r;
    for (int i = 0; i < 23; i++) out[i] = (uint8_t)((c >> (22 - i)) & 1);
}

int golay23Decode(const uint8_t in[23], uint8_t info[12]) {
    uint32_t c = 0;
    for (int i = 0; i < 23; i++) c = (c << 1) | (in[i] & 1);
    const uint32_t s = polyMod(c, 23, kGolayG, 11);
    const uint32_t e = golayTable().err[s];
    c ^= e;
    for (int i = 0; i < 12; i++) info[i] = (uint8_t)((c >> (22 - i)) & 1);
    return __builtin_popcount(e);
}

// ---------------------------------------------------------------- BCH (127,113) shortened
namespace {
constexpr uint32_t kBchG = (1u << 14) | (1u << 9) | (1u << 8) | (1u << 6) | (1u << 5) | (1u << 4) | (1u << 2) | (1u << 1) | 1u;
uint32_t bchRem(const uint8_t* bits, int n) {   // remainder of the n-bit polynomial (first = highest power) by g
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | (bits[i] & 1);
        if (r >> 14 & 1) r ^= kBchG;
    }
    return r & 0x3FFF;
}
} // namespace

void bchEncode(const uint8_t* msg, int k, uint8_t* out) {
    std::vector<uint8_t> t(msg, msg + k);
    t.resize((size_t)k + 14, 0);
    const uint32_t r = bchRem(t.data(), k + 14);
    for (int i = 0; i < k; i++) out[i] = msg[i] & 1;
    for (int i = 0; i < 14; i++) out[k + i] = (uint8_t)((r >> (13 - i)) & 1);
}

int bchDecode(uint8_t* cw, int n) {
    const uint32_t s = bchRem(cw, n);
    if (s == 0) return 0;
    // the syndrome of a single error at position i (from the end: x^(n-1-i)) is x^(n-1-i) mod g
    uint32_t p = 1;   // x^0 mod g
    for (int j = 0; j < n; j++) {   // j = power
        if (p == s) { cw[n - 1 - j] ^= 1; return 1; }
        p <<= 1;
        if (p >> 14 & 1) p ^= kBchG;
    }
    return -1;
}

// ---------------------------------------------------------------- Hamming 8/4, CRC
uint8_t ham84(int d) {
    const int d1 = d & 1, d2 = (d >> 1) & 1, d3 = (d >> 2) & 1, d4 = (d >> 3) & 1;   // b2, b4, b6, b8
    const int b1 = 1 ^ d1 ^ d4 ^ d3, b3 = d2 ^ (1 ^ d1) ^ d4, b5 = d3 ^ d2 ^ (1 ^ d1), b7 = d4 ^ d3 ^ d2;
    return (uint8_t)(b1 | d1 << 1 | b3 << 2 | d2 << 3 | b5 << 4 | d3 << 5 | b7 << 6 | d4 << 7);
}
int unham84(uint8_t byte) {
    static int tab[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) tab[i] = -1;
        for (int d = 0; d < 16; d++) {
            const uint8_t c = ham84(d);
            tab[c] = d;
            for (int b = 0; b < 8; b++) tab[c ^ (1 << b)] = d;
        }
        init = true;
    }
    return tab[byte];
}

uint16_t crc16(const uint8_t* bytes, size_t n) { return (uint16_t)crc::generic(bytes, n, 16, 0x1021, 0, true, false, 0); }   // bytes LSB first into an MSB-first register

const std::vector<int>& interleave() {
    static const std::vector<int> p = [] {
        std::vector<int> v;
        for (int r = 0; r < 94; r++)
            for (int k = 0; k < 8; k++) { const int idx = r + 94 * k; if (idx < kPacketBits) v.push_back(idx); }
        return v;
    }();
    return p;
}

const std::vector<uint8_t>& prbsFrame(int variant) {
    static std::vector<uint8_t> t[2];
    std::vector<uint8_t>& v = t[variant == kD ? 1 : 0];
    if (v.empty()) {
        const int per = line625Bits(variant);
        v.resize((size_t)per * kLines);
        Prbs p; p.reset();
        for (auto& b : v) b = (uint8_t)p.next();
    }
    return v;
}

// ---------------------------------------------------------------- packets
void packetBits(const Packet& p, uint8_t out[kPacketBits]) {
    uint8_t raw[kPacketBits], info[12];
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)((p.address >> i) & 1);
    info[10] = (uint8_t)(p.ci & 1); info[11] = (uint8_t)((p.ci >> 1) & 1);
    golay23Encode(info, raw);
    for (int i = 0; i < kPacketBytes; i++)
        for (int b = 0; b < 8; b++) raw[23 + i * 8 + b] = (uint8_t)((p.data[i] >> b) & 1);
    const auto& perm = interleave();
    for (int t = 0; t < kPacketBits; t++) out[t] = raw[perm[(size_t)t]];
}

int parsePacket(const uint8_t in[kPacketBits], Packet& p) {
    uint8_t raw[kPacketBits], info[12];
    const auto& perm = interleave();
    for (int t = 0; t < kPacketBits; t++) raw[perm[(size_t)t]] = in[t];
    const int e = golay23Decode(raw, info);
    p.address = 0; for (int i = 0; i < 10; i++) p.address |= info[i] << i;
    p.ci = info[10] | info[11] << 1;
    for (int i = 0; i < kPacketBytes; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) v |= (uint8_t)(raw[23 + i * 8 + b] << b);
        p.data[i] = v;
    }
    return e;
}

// ---------------------------------------------------------------- sound
namespace {
// scale factor code (R2 R1 R0) of a coding / protection range (Part 3 Table 2), index = protection range 1..7, 8 = silence
const int kScaleCode[9] = {0, 7, 6, 5, 3, 4, 2, 1, 0};
int codingRangeOf(int code) { switch (code) { case 7: return 1; case 6: return 2; case 5: return 3; case 3: return 4; default: return 5; } }
// the samples (1 based) that carry R2, R1, R0 of FE1 / FE2 and CIB1 / CIB2
void sfSamples(bool stereo, int fe, int bit, int* idx) {   // 9 indices
    for (int k = 0; k < 9; k++) {
        if (stereo) idx[k] = 1 + 6 * k + (fe ? 1 : 0) + 2 * bit;            // R2A 1,7,..; R1A 3,9,..; R0A 5,11,..; R2B 2,8,..
        else idx[k] = (fe ? 28 : 1) + 3 * k + bit;                          // R2n 1,4,..; R1n 2,5,..; R0n 3,6,..; FE2 from 28
    }
}
int parity6(int s10) { return __builtin_popcount((unsigned)(s10 >> 4) & 0x3F) & 1; }
} // namespace

void soundEncode(const int16_t* x, bool stereo, uint8_t block[90]) {
    int code[2];
    for (int fe = 0; fe < 2; fe++) {
        int peak = 0;
        for (int i = 0; i < 64; i++) {
            const bool inGroup = stereo ? ((i & 1) == fe) : (i / 32 == fe);
            if (inGroup) peak = std::max(peak, std::abs((int)x[i]));
        }
        // protection range from the peak relative to the overload 8192 (6 dB steps)
        int pr = 1;
        double lvl = peak > 0 ? 20 * std::log10(peak / 8192.0) : -100;
        if (lvl < -6) pr = 2;
        if (lvl < -12) pr = 3;
        if (lvl < -18) pr = 4;
        if (lvl < -24) pr = 5;
        if (lvl < -30) pr = 6;
        if (lvl < -36) pr = 7;
        code[fe] = kScaleCode[pr];
    }
    uint16_t s11[64];
    for (int i = 0; i < 64; i++) {
        const int fe = stereo ? (i & 1) : i / 32;
        const int shift = 5 - codingRangeOf(code[fe]);   // range 1: drop 4 bits ... range 5: none
        int v = x[i] >> shift;
        v = std::max(-512, std::min(511, v));
        const int s10 = v & 0x3FF;
        s11[i] = (uint16_t)(s10 | parity6(s10) << 10);   // even parity over X4..X9 and P
    }
    // signal the scale factors and the control bits in the parity bits
    for (int fe = 0; fe < 2; fe++)
        for (int bit = 0; bit < 3; bit++) {
            int idx[9]; sfSamples(stereo, fe, bit, idx);
            const int r = (code[fe] >> (2 - bit)) & 1;
            for (int k = 0; k < 9; k++) s11[idx[k] - 1] ^= (uint16_t)(r << 10);
        }
    // CIB1 = CIB2 = 0: parity unchanged on 55..64
    std::memset(block, 0xFF, 2);   // 16 unallocated bits (set to 1)
    std::memset(block + 2, 0, 88);
    int bitPos = 16;
    for (int i = 0; i < 64; i++)
        for (int b = 0; b < 11; b++, bitPos++)
            if (s11[i] >> b & 1) block[bitPos >> 3] |= (uint8_t)(1 << (bitPos & 7));
}

int soundDecode(const uint8_t block[90], bool stereo, int16_t* x, int range[2]) {
    uint16_t s11[64];
    int bitPos = 16;
    for (int i = 0; i < 64; i++) {
        uint16_t v = 0;
        for (int b = 0; b < 11; b++, bitPos++) v |= (uint16_t)(((block[bitPos >> 3] >> (bitPos & 7)) & 1) << b);
        s11[i] = v;
    }
    auto sig = [&](int i) { return ((s11[i] >> 10) & 1) ^ parity6(s11[i] & 0x3FF); };   // the signalled bit (if no error)
    int code[2];
    for (int fe = 0; fe < 2; fe++) {
        code[fe] = 0;
        for (int bit = 0; bit < 3; bit++) {
            int idx[9]; sfSamples(stereo, fe, bit, idx);
            int ones = 0;
            for (int k = 0; k < 9; k++) ones += sig(idx[k] - 1);
            code[fe] |= (ones >= 5 ? 1 : 0) << (2 - bit);
        }
    }
    // the expected signalling bit of each sample, to find parity errors
    int expect[64] = {};
    for (int fe = 0; fe < 2; fe++)
        for (int bit = 0; bit < 3; bit++) {
            int idx[9]; sfSamples(stereo, fe, bit, idx);
            for (int k = 0; k < 9; k++) expect[idx[k] - 1] = (code[fe] >> (2 - bit)) & 1;
        }
    int cib[2] = {0, 0};
    for (int c = 0; c < 2; c++) { int ones = 0; for (int k = 0; k < 5; k++) ones += sig(54 + 5 * c + k); cib[c] = ones >= 3; }
    for (int i = 54; i < 64; i++) expect[i] = cib[(i - 54) / 5];
    int errs = 0;
    int16_t prev[2] = {0, 0};
    for (int i = 0; i < 64; i++) {
        const int fe = stereo ? (i & 1) : i / 32;
        const int ch = stereo ? (i & 1) : 0;
        const int shift = 5 - codingRangeOf(code[fe]);
        int v = s11[i] & 0x3FF;
        if (v & 0x200) v -= 0x400;
        if (sig(i) != expect[i]) { errs++; x[i] = prev[ch]; continue; }
        x[i] = (int16_t)(v * (1 << shift));
        prev[ch] = x[i];
    }
    if (range) { range[0] = codingRangeOf(code[0]); range[1] = codingRangeOf(code[1]); }
    return errs;
}

// ---------------------------------------------------------------- service identification
namespace {
void putParams(const std::vector<SiParam>& ps, std::vector<uint8_t>& out) {
    for (const SiParam& p : ps) {
        std::vector<uint8_t> f = p.field;
        if (p.pi == kPiPg) { f.clear(); putParams(p.group, f); }
        out.push_back(p.pi);
        if (f.size() < 255) out.push_back((uint8_t)f.size());
        else { out.push_back(255); out.push_back((uint8_t)(f.size() >> 8)); out.push_back((uint8_t)f.size()); }
        out.insert(out.end(), f.begin(), f.end());
    }
}
bool readLi(const uint8_t* b, size_t n, size_t& i, size_t& li) {
    if (i >= n) return false;
    li = b[i++];
    if (li == 255) { if (i + 2 > n) return false; li = (size_t)b[i] << 8 | b[i + 1]; i += 2; }
    return i + li <= n;
}
bool getParams(const uint8_t* b, size_t n, std::vector<SiParam>& out, int depth) {
    size_t i = 0;
    while (i < n) {
        SiParam p;
        p.pi = b[i++];
        size_t li;
        if (!readLi(b, n, i, li)) return false;
        if (p.pi == kPiPg && depth == 0) { if (!getParams(b + i, li, p.group, 1)) return false; }
        else p.field.assign(b + i, b + i + li);
        i += li;
        out.push_back(std::move(p));
    }
    return true;
}
} // namespace

std::vector<uint8_t> siEncodeCommands(const std::vector<SiCommand>& cmds) {
    std::vector<uint8_t> out;
    for (const SiCommand& c : cmds) {
        std::vector<uint8_t> body;
        putParams(c.params, body);
        out.push_back(c.ci);
        if (body.size() < 255) out.push_back((uint8_t)body.size());
        else { out.push_back(255); out.push_back((uint8_t)(body.size() >> 8)); out.push_back((uint8_t)body.size()); }
        out.insert(out.end(), body.begin(), body.end());
    }
    return out;
}

bool siParseCommands(const uint8_t* b, size_t n, std::vector<SiCommand>& out) {
    size_t i = 0;
    while (i < n) {
        SiCommand c;
        c.ci = b[i++];
        size_t li;
        if (!readLi(b, n, i, li)) return false;
        if (!getParams(b + i, li, c.params, 0)) return false;
        i += li;
        out.push_back(std::move(c));
    }
    return true;
}

std::vector<Packet> siDataGroup(int tg, int continuity, const std::vector<SiCommand>& cmds) {
    const std::vector<uint8_t> body = siEncodeCommands(cmds);
    const uint16_t dgs = crc16(body.data(), body.size());
    std::vector<uint8_t> g;
    const size_t total = 8 + body.size() + 2;
    const int S = (int)((total + 87) / 88);
    const int F = (int)(total - (size_t)(S - 1) * 88);
    const int hdr[8] = {tg & 15, continuity & 15, 0, (S >> 4) & 15, S & 15, (F >> 4) & 15, F & 15, 1};
    for (int h : hdr) g.push_back(ham84(h));
    g.insert(g.end(), body.begin(), body.end());
    g.push_back((uint8_t)(dgs >> 8)); g.push_back((uint8_t)dgs);
    std::vector<Packet> pk;
    for (int s = 0; s < S; s++) {
        Packet p;
        p.address = 0;
        p.data[0] = s == 0 ? kPtBc2 : kPtBc1;   // F8: first packet of a data group, C7: next
        for (int k = 0; k < 88; k++) { const size_t at = (size_t)s * 88 + k; p.data[1 + k] = at < g.size() ? g[at] : 0; }
        const uint16_t ps = crc16(p.data + 1, 88);
        p.data[89] = (uint8_t)(ps >> 8); p.data[90] = (uint8_t)ps;
        pk.push_back(p);
    }
    return pk;
}

bool SiAssembler::push(const Packet& p, int& tg, std::vector<SiCommand>& cmds) {
    const uint8_t pt = p.data[0];
    if (pt == kPtBi1 || pt == kPtBi2) { golayPackets++; return false; }   // the Golay coded form is not decoded
    if (pt != kPtBc1 && pt != kPtBc2) return false;
    const uint16_t ps = crc16(p.data + 1, 88);
    if (ps != (uint16_t)(p.data[89] << 8 | p.data[90])) { packetsBad++; if (want_) { groupsBad++; want_ = 0; } return false; }
    if (pt == kPtBc2) {   // first packet: the data group header
        int h[8];
        for (int i = 0; i < 8; i++) h[i] = unham84(p.data[1 + i]);
        bool ok = true;
        for (int v : h) ok &= v >= 0;
        if (want_) groupsBad++;
        want_ = 0;
        if (!ok) { groupsBad++; return false; }
        tg_ = h[0];
        want_ = h[3] << 4 | h[4];
        last_ = h[5] << 4 | h[6];
        if (want_ < 1 || last_ < 1 || last_ > 88) { want_ = 0; groupsBad++; return false; }
        buf_.assign(p.data + 1, p.data + 89);
        have_ = 1;
    } else {
        if (!want_) return false;
        if (lastCi_ >= 0 && p.ci != ((lastCi_ + 1) & 3)) { groupsBad++; want_ = 0; lastCi_ = p.ci; return false; }   // a packet was lost
        buf_.insert(buf_.end(), p.data + 1, p.data + 89);
        have_++;
    }
    lastCi_ = p.ci;
    if (have_ < want_) return false;
    const size_t total = (size_t)(want_ - 1) * 88 + (size_t)last_;
    want_ = 0;
    if (total < 10 || total > buf_.size()) { groupsBad++; return false; }
    const uint8_t* body = buf_.data() + 8;
    const size_t n = total - 10;
    const uint16_t dgs = (uint16_t)(buf_[total - 2] << 8 | buf_[total - 1]);
    if (crc16(body, n) != dgs) { groupsBad++; return false; }
    cmds.clear();
    if (!siParseCommands(body, n, cmds)) { groupsBad++; return false; }
    tg = tg_;
    groupsOk++;
    return true;
}

std::string siText(const std::vector<uint8_t>& f, size_t from) {
    std::string s;
    for (size_t i = from; i < f.size(); i++) {
        const uint8_t c = f[i];
        if (c >= 0x20 && c < 0x7F) s += (char)c;
        else if (c != 0) s += '?';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

const char* caSystemName(int ca) {
    if (ca == kCaEuroCrypt) return "EuroCrypt";
    return "conditional access";
}

std::string vsamText(const Sdf& s) {
    if (!s.scrambled()) return "free access, unscrambled";
    std::string t = s.controlled() ? "controlled access, " : "free access, ";
    t += s.singleCut() ? "single-cut line rotation" : "double-cut component rotation";
    return t;
}

// ---------------------------------------------------------------- line 625
namespace {
void putBits(uint8_t*& p, uint32_t v, int n) { for (int i = 0; i < n; i++) *p++ = (uint8_t)((v >> i) & 1); }   // LSB first
uint32_t getBits(const uint8_t*& p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= (uint32_t)(*p++ & 1) << i; return v; }
} // namespace

void line625Encode(const Sdf& s, const Rdf rdf[5], uint8_t udt5, uint8_t out[546]) {
    uint8_t* p = out;
    putBits(p, udt5, 5);
    uint8_t m[94], *q = m;
    putBits(q, s.chid, 16); putBits(q, s.sdfscr, 8); putBits(q, s.mvscg, 8); putBits(q, s.cafcnt, 20);
    putBits(q, 1, 1); putBits(q, 1, 1); putBits(q, 3, 2); putBits(q, s.sift ? 1 : 0, 1);   // Rp, Fp, unallocated, SIFT
    bchEncode(m, 57, p); p += 71;
    for (int k = 0; k < 5; k++) {
        const Rdf& r = rdf[k];
        q = m;
        putBits(q, (uint32_t)r.tdmcid, 8); putBits(q, (uint32_t)r.fcnt, 8); putBits(q, r.udf, 1);
        putBits(q, (uint32_t)r.fln1, 10); putBits(q, (uint32_t)r.lln1, 10); putBits(q, (uint32_t)r.fln2, 10); putBits(q, (uint32_t)r.lln2, 10);
        putBits(q, (uint32_t)r.fcp, 11); putBits(q, (uint32_t)r.lcp, 11); putBits(q, r.links, 1);
        bchEncode(m, 80, p); p += 94;
    }
}

int line625Decode(const uint8_t in[546], Sdf& s, Rdf rdf[5], bool& sdfOk) {
    int good = 0;
    uint8_t cw[94];
    std::memcpy(cw, in + 5, 71);
    sdfOk = bchDecode(cw, 71) >= 0;
    if (sdfOk) {
        const uint8_t* p = cw;
        s.chid = (uint16_t)getBits(p, 16); s.sdfscr = (uint8_t)getBits(p, 8); s.mvscg = (uint8_t)getBits(p, 8); s.cafcnt = getBits(p, 20);
        getBits(p, 4); s.sift = getBits(p, 1) != 0;
        good++;
    }
    for (int k = 0; k < 5; k++) {
        std::memcpy(cw, in + 76 + 94 * k, 94);
        if (bchDecode(cw, 94) < 0) { rdf[k].tdmcid = -1; continue; }
        const uint8_t* p = cw;
        Rdf& r = rdf[k];
        r.tdmcid = (int)getBits(p, 8); r.fcnt = (int)getBits(p, 8); r.udf = getBits(p, 1) != 0;
        r.fln1 = (int)getBits(p, 10); r.lln1 = (int)getBits(p, 10); r.fln2 = (int)getBits(p, 10); r.lln2 = (int)getBits(p, 10);
        r.fcp = (int)getBits(p, 11); r.lcp = (int)getBits(p, 11); r.links = getBits(p, 1) != 0;
        good++;
    }
    return good;
}

} // namespace mac
} // namespace dect2
