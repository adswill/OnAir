// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FX.25 and IL2P receivers (packet_fec.h).
#include "dect2/packet/packet_fec.h"
#include "dect2/packet/packet_ax25.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

extern "C" {
void* init_rs_char(int symsize, int gfpoly, int fcr, int prim, int nroots, int pad);
int decode_rs_char(void* rs, unsigned char* data, int* eras_pos, int no_eras);
}

namespace dect2 {

namespace packetfec {

namespace {

constexpr int kPoly = 0x11d;

struct GfTabs { uint8_t exp[512], log[256]; };
const GfTabs& gf() {
    static const GfTabs t = [] {
        GfTabs g{};
        int x = 1;
        for (int i = 0; i < 255; i++) { g.exp[i] = (uint8_t)x; g.log[x] = (uint8_t)i; x <<= 1; if (x & 256) x ^= kPoly; }
        for (int i = 255; i < 512; i++) g.exp[i] = g.exp[i - 255];
        return g;
    }();
    return t;
}
uint8_t gmul(uint8_t a, uint8_t b) { return (a && b) ? gf().exp[gf().log[a] + gf().log[b]] : 0; }

void* rsFor(int nroots, int fcr, int pad) {
    static std::mutex mu;
    static std::map<uint64_t, void*> cache;
    const uint64_t k = (uint64_t)nroots << 32 | (uint64_t)(fcr & 255) << 16 | (uint64_t)pad;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    void* r = init_rs_char(8, kPoly, fcr, 1, nroots, pad);
    cache[k] = r;
    return r;
}

} // namespace

int rsEncodeParity(const uint8_t* d, int n, int nroots, int fcr, uint8_t* par) {
    std::vector<uint8_t> g((size_t)nroots + 1, 0);
    g[0] = 1;
    for (int i = 0; i < nroots; i++) {
        const uint8_t a = gf().exp[fcr + i];
        for (int j = i + 1; j >= 1; j--) g[(size_t)j] = (uint8_t)(g[(size_t)j - 1] ^ gmul(g[(size_t)j], a));
        g[0] = gmul(g[0], a);
    }
    // g[nroots] is the x^nroots coefficient (1), g[0] the constant
    std::vector<uint8_t> rem((size_t)nroots, 0);
    for (int i = 0; i < n; i++) {
        const uint8_t fb = (uint8_t)(d[i] ^ rem[0]);
        for (int j = 1; j < nroots; j++) rem[(size_t)j - 1] = (uint8_t)(rem[(size_t)j] ^ gmul(fb, g[(size_t)(nroots - j)]));
        rem[(size_t)nroots - 1] = gmul(fb, g[0]);
    }
    for (int i = 0; i < nroots; i++) par[i] = rem[(size_t)i];
    return nroots;
}

bool fx25Tag(int ctag, uint64_t& value, int& nBytes, int& kBytes) {
    struct T { uint64_t v; int n, k; };
    static const T k[12] = {
        {0x566ED2717946107EULL, 0, 0}, {0xB74DB7DF8A532F3EULL, 255, 239}, {0x26FF60A600CC8FDEULL, 144, 128}, {0xC7DC0508F3D9B09EULL, 80, 64},
        {0x8F056EB4369660EEULL, 48, 32}, {0x6E260B1AC5835FAEULL, 255, 223}, {0xFF94DC634F1CFF4EULL, 160, 128}, {0x1EB7B9CDBC09C00EULL, 96, 64},
        {0xDBF869BD2DBB1776ULL, 64, 32}, {0x3ADB0C13DEAE2836ULL, 255, 191}, {0xAB69DB6A543188D6ULL, 192, 128}, {0x4A4ABEC4A724B796ULL, 128, 64},
    };
    if (ctag < 1 || ctag > 11) return false;
    value = k[ctag].v; nBytes = k[ctag].n; kBytes = k[ctag].k;
    return true;
}

// The multiplicative scrambler x^9 + x^4 + 1 (draft 0.6; the states of Dire Wolf's il2p_scramble.c): TX start 0x00f and 5 bits of delay, RX start 0x1f0.
std::vector<uint8_t> il2pDescramble(const uint8_t* in, size_t n) {
    std::vector<uint8_t> o(n, 0);
    int st = 0x1f0;
    size_t bo = 0;
    for (size_t i = 0; i < n; i++) for (int b = 7; b >= 0; b--) {
        const int rx = (in[i] >> b) & 1;
        const int out = (rx ^ st) & 1;
        st >>= 1;
        if (rx) st ^= 0x108;
        if (out) o[bo / 8] |= (uint8_t)(0x80 >> (bo % 8));
        bo++;
    }
    return o;
}

std::vector<uint8_t> il2pScramble(const uint8_t* in, size_t n) {
    std::vector<uint8_t> o(n, 0);
    int st = 0x00f;
    int skip = 5;
    size_t bo = 0;
    auto step = [&](int bit) {
        const int out = ((st >> 4) ^ st) & 1;
        const int fb = (bit ^ st) & 1;
        if (st & 1) st ^= 0x10;
        st = (st >> 1) | (fb << 8);
        if (skip > 0) { skip--; return; }
        if (bo < n * 8) { if (out) o[bo / 8] |= (uint8_t)(0x80 >> (bo % 8)); bo++; }
    };
    for (size_t i = 0; i < n; i++) for (int b = 7; b >= 0; b--) step((in[i] >> b) & 1);
    for (int i = 0; i < 5; i++) step(0);
    return o;
}

} // namespace packetfec

struct PacketFecRx::Impl {
    Sink sink;
    int prev = 0;
    uint64_t tagReg = 0;
    // FX.25 block being collected
    int fxBytes = 0, fxK = 0, fxN = 0, fxAcc = 0, fxBit = 0, fxTag = 0;
    std::vector<uint8_t> fxBuf;
    // IL2P
    uint32_t ilSync = 0;
    int ilState = 0, ilAcc = 0, ilBit = 0, ilInv = 0;
    std::vector<uint8_t> ilBuf;
    std::vector<uint8_t> ilHdrPlain;
    size_t ilBlock = 0;
    std::vector<int> ilBlocks;
    std::vector<uint8_t> ilOut;

    void reset() { prev = 0; tagReg = 0; fxBytes = 0; fxN = 0; ilSync = 0; ilState = 0; ilOut.clear(); ilBuf.clear(); ilBit = 0; ilAcc = 0; }

    void fxBlockDone() {
        int nv = 0; uint64_t v; int n = 0, k = 0;
        (void)nv; (void)v;
        packetfec::fx25Tag(fxTag, v, n, k);
        const int nroots = n - k;
        std::vector<uint8_t> block(255, 0);
        std::memcpy(block.data(), fxBuf.data(), (size_t)k);
        std::memcpy(block.data() + 255 - nroots, fxBuf.data() + k, (size_t)nroots);
        const int corr = decode_rs_char(packetfec::rsFor(nroots, 1, 0), block.data(), nullptr, 0);
        if (corr < 0) return;
        // the data holds HDLC with bit stuffing: reuse the HDLC receiver (no NRZI at this layer)
        std::vector<uint8_t> frame;
        bool got = false;
        ax25::HdlcRx h([&](const std::vector<uint8_t>& d, bool ok) { if (ok && !got) { frame = d; got = true; } }, false);
        for (int i = 0; i < k && !got; i++) for (int b = 0; b < 8; b++) h.bit((block[(size_t)i] >> b) & 1);
        if (got && sink) sink(frame, corr, false);
    }

    void fxBitIn(int d) {
        if (fxBytes > 0) {
            fxAcc |= d << fxBit;
            if (++fxBit == 8) { fxBuf.push_back((uint8_t)fxAcc); fxBit = 0; fxAcc = 0; if (--fxBytes == 0) fxBlockDone(); }
            return;
        }
        tagReg = (tagReg >> 1) | ((uint64_t)d << 63);
        for (int t = 1; t < 12; t++) {
            uint64_t v; int n, k;
            packetfec::fx25Tag(t, v, n, k);
            if (__builtin_popcountll(tagReg ^ v) <= 8) { fxTag = t; fxN = n; fxBytes = n; fxBuf.clear(); fxBuf.reserve((size_t)n); fxBit = 0; fxAcc = 0; break; }
        }
    }

    void ilBitIn(int lv) {
        lv ^= ilInv;
        if (ilState == 0) {
            ilSync = ((ilSync << 1) | (uint32_t)lv) & 0xFFFFFF;
            const uint32_t s = 0xF15E48, si = 0x0EA1B7;
            if (__builtin_popcount(ilSync ^ s) <= 1) { ilInv ^= 0; ilState = 1; ilBuf.clear(); ilAcc = 0; ilBit = 0; }
            else if (__builtin_popcount(ilSync ^ si) <= 1) { ilInv ^= 1; ilState = 1; ilBuf.clear(); ilAcc = 0; ilBit = 0; ilSync = s; }
            return;
        }
        ilAcc = (ilAcc << 1) | lv;
        if (++ilBit < 8) return;
        ilBuf.push_back((uint8_t)ilAcc); ilAcc = 0; ilBit = 0;
        if (ilState == 1 && ilBuf.size() == 15) {
            const int corr = decode_rs_char(packetfec::rsFor(2, 0, 255 - 15), ilBuf.data(), nullptr, 0);
            if (corr < 0) { ilState = 0; ilInv = 0; return; }
            ilHdrPlain = packetfec::il2pDescramble(ilBuf.data(), 13);
            const uint8_t* h = ilHdrPlain.data();
            const int type = h[1] >> 7;
            int payload = 0;
            for (int i = 2; i <= 11; i++) payload = payload * 2 + (h[i] >> 7);
            ilOut.clear();
            if (type == 1) {
                auto ch = [&](int i) { return (uint8_t)((h[i] & 0x3F) + 0x20); };
                std::string dst, src;
                for (int i = 0; i < 6; i++) dst += (char)ch(i);
                for (int i = 6; i < 12; i++) src += (char)ch(i);
                const int dssid = h[12] >> 4, sssid = h[12] & 15;
                const int ui = (h[0] >> 6) & 1;
                int pid = 0; for (int i = 1; i <= 4; i++) pid = pid * 2 + ((h[i] >> 6) & 1);
                int ctl = 0; for (int i = 5; i <= 11; i++) ctl = ctl * 2 + ((h[i] >> 6) & 1);
                // AX.25 address fields: dest (C bit set), source (C clear, last)
                for (char c : dst) ilOut.push_back((uint8_t)(c << 1));
                const int cmd = pid < 2 ? (ctl >> 2) & 1 : 1;     // the C bit of S and U frames; I frames are commands
                ilOut.push_back((uint8_t)(0x60 | (dssid << 1) | (cmd ? 0x80 : 0)));
                for (char c : src) ilOut.push_back((uint8_t)(c << 1));
                ilOut.push_back((uint8_t)(0x60 | (sssid << 1) | (cmd ? 0 : 0x80) | 1));
                // control byte and PID per the Type 1 tables
                const int pfBit = (ctl >> 6) & 1;
                if (pid == 0) {                // S frame: P/F bit 6, N(R) bits 5..3, C bit 2, type bits 1..0
                    const int nr = (ctl >> 3) & 7, op = ctl & 3;
                    ilOut.push_back((uint8_t)(nr << 5 | pfBit << 4 | op << 2 | 1));
                } else if (pid == 1) {         // U frame: P/F bit 6, opcode bits 5..3, C bit 2
                    static const uint8_t kCtl[8] = {0x2F, 0x43, 0x0F, 0x63, 0x87, 0x03, 0xAF, 0xE3};
                    ilOut.push_back((uint8_t)(kCtl[(ctl >> 3) & 7] | pfBit << 4));
                    if (ui) ilOut.push_back(0xF0);
                } else {                       // I frame: P/F bit 6, N(R) bits 5..3, N(S) bits 2..0
                    ilOut.push_back((uint8_t)(((ctl >> 3) & 7) << 5 | pfBit << 4 | (ctl & 7) << 1));
                    static const uint8_t kPid[16] = {0xF0, 0xF0, 0x20, 0x01, 0x06, 0x07, 0x08, 0xF0, 0xF0, 0xF0, 0xF0, 0xCC, 0xCD, 0xCE, 0xCF, 0xF0};
                    ilOut.push_back(kPid[pid & 15]);
                }
            }
            ilBlocks.clear();
            if (payload > 0) {
                const int count = (payload + 238) / 239;
                const int small = payload / count, big = payload - count * small;
                for (int i = 0; i < big; i++) ilBlocks.push_back(small + 1);
                for (int i = big; i < count; i++) ilBlocks.push_back(small);
            }
            ilBlock = 0;
            if (ilBlocks.empty()) { if (sink) sink(ilOut, corr, true); ilState = 0; ilInv = 0; return; }
            ilState = 2; ilBuf.clear();
            return;
        }
        if (ilState == 2 && ilBuf.size() == (size_t)ilBlocks[ilBlock] + 16) {
            const int n = ilBlocks[ilBlock];
            const int corr = decode_rs_char(packetfec::rsFor(16, 0, 255 - n - 16), ilBuf.data(), nullptr, 0);
            if (corr < 0) { ilState = 0; ilInv = 0; return; }
            const auto plain = packetfec::il2pDescramble(ilBuf.data(), (size_t)n);
            ilOut.insert(ilOut.end(), plain.begin(), plain.end());
            ilBuf.clear();
            if (++ilBlock == ilBlocks.size()) { if (sink) sink(ilOut, 0, true); ilState = 0; ilInv = 0; }
        }
    }
};

PacketFecRx::PacketFecRx() : p_(std::make_unique<Impl>()) {}
PacketFecRx::~PacketFecRx() = default;
void PacketFecRx::setSink(Sink s) { p_->sink = std::move(s); }
void PacketFecRx::reset() { p_->reset(); }

void PacketFecRx::lineBit(int level, bool il2pActive) {
    Impl& m = *p_;
    const int d = level == m.prev ? 1 : 0;
    m.prev = level;
    m.fxBitIn(d);
    if (il2pActive) m.ilBitIn(level);
}

} // namespace dect2
