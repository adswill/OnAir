// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the VDL Mode 2 layers (rust/onair_vdl2 behind the D::vdl2* functions): the scrambler, every one of the 2^25 header words, every burst
// length, Reed-Solomon with random errors and erasures, burst bodies (built by the generator, with bit errors), the FCS, the HDLC deframer on built / damaged /
// random bit streams, addresses, and AVLC frame decoding of built frames (ACARS, X.25 with CLNP / COTP, call set-up, XID GSIF / link establishment), random
// X.25 packets and random frames. ONAIR_DIFF_ITERS (default 200000) inputs per family. Every result goes into a digest (tests/common/golden.h; doubles rounded
// to 1e-6) compared with the one recorded from the C++ layers the Rust code replaced.
#include "dect2/vdl2/vdl2_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
namespace D = dect2;
static golden::Digest dig;
static std::mt19937 g(1029);
static uint32_t r32() { return g(); }
static uint8_t rb() { return (uint8_t)g(); }

static void addMsg(const dect2::Vdl2Message& a) {
    dig.add(a.src, a.dst, a.srcType, a.dstType, a.fromAir, a.fromGround, a.onGround, a.response, a.frameKind, a.frameName, a.ns, a.nr, a.pf);
    dig.add(a.proto, a.summary, a.detail, a.acars, a.reg, a.flight, a.label, a.labelText, a.msgNum, a.acarsText, a.blockId, a.acarsCrcOk, a.hasPos);
    dig.add(a.lat, a.lon, a.hasAlt, a.altFt, a.posKind, a.hasKin, a.kinTrackDeg, a.groundKt, a.vsFpm);
}

static std::vector<uint8_t> mutate(std::vector<uint8_t> v) {
    for (int k = (int)(g() % 4); k > 0; k--) {
        if (v.empty()) { v.push_back(rb()); continue; }
        switch (g() % 5) {
        case 0: v[g() % v.size()] = rb(); break;
        case 1: v[g() % v.size()] ^= (uint8_t)(1u << (g() % 8)); break;
        case 2: v.erase(v.begin() + (long)(g() % v.size())); break;
        case 3: ORD_CALL(v.insert, v.begin() + (long)(g() % (v.size() + 1)), rb()); break;   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
        default: v.resize(g() % (v.size() + 1)); break;
        }
    }
    return v;
}
static std::vector<uint8_t> rndBytes(size_t n) { std::vector<uint8_t> v(n); for (auto& b : v) b = rb(); return v; }
static std::string rndStr(size_t n, const char* a, size_t al) { std::string s; for (size_t i = 0; i < n; i++) s += a[g() % al]; return s; }
#define RS(n, lit) rndStr((n), lit, sizeof(lit) - 1)

static void testBitLayer(long iters) {
    // header words: every 25 bit word, with preset outputs to catch what is (not) written
    for (uint32_t w = 0; w < (1u << 25); w++) {
        uint32_t la = 0xDEADBEEF;
        bool ca = false;
        const bool oa = D::vdl2HeaderDecode(w, la, ca);
        dig.add(oa, la, ca);
    }
    for (uint32_t len = 0; len < 140000; len++) {
        dig.add(D::vdl2HeaderWord(len));
        D::Vdl2Layout a;
        const bool oa = D::vdl2Layout(len, a);
        dig.add(oa, a.dataOct, a.fecOct, a.blocks, a.lastLen, a.lastFec);
    }
    for (long i = 0; i < iters; i++) {
        const uint32_t len = r32();
        D::Vdl2Layout a;
        const bool oa = D::vdl2Layout(len, a);
        dig.add(oa, a.dataOct, a.fecOct, a.blocks, a.lastLen, a.lastFec);
        dig.add(D::vdl2HeaderWord(len));
    }
    for (int n = -5; n < 400; n++) dig.add(D::vdl2LastFec(n));
    // scrambler
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> a(g() % 3000);
        for (auto& b : a) b = g() % 2;
        const size_t from = g() % (a.size() + 3);
        D::vdl2Scramble(a, from);
        dig.add(a);
    }
}

static void testRs(long iters) {
    long corrected = 0, refused = 0;
    for (long i = 0; i < iters; i++) {
        uint8_t data[249];
        for (auto& b : data) b = rb();
        uint8_t pa[6];
        D::vdl2RsEncode(data, pa);
        dig.bytes(pa, 6);
        uint8_t cw[255];
        memcpy(cw, data, 249); memcpy(cw + 249, pa, 6);
        const int np = (int)(g() % 7);
        if (np < 6) memset(cw + 249 + np, 0, (size_t)(6 - np));      // erased parity octets are zero (as the burst gatherer leaves them)
        const int errs = g() % 3 == 0 ? (int)(g() % 9) : (int)(g() % 4);
        for (int e = 0; e < errs; e++) cw[g() % 255] ^= (uint8_t)(1 + g() % 255);
        uint8_t a[255];
        memcpy(a, cw, 255);
        const int ra = D::vdl2RsDecode(a, np);
        dig.add(ra); dig.bytes(a, 255);
        (ra < 0 ? refused : corrected)++;
    }
    printf("  reed-solomon: %ld decoded, %ld refused\n", corrected, refused);
}

static void testBurst(long iters) {
    long ok = 0;
    for (long i = 0; i < iters; i++) {
        const size_t nbase = 8 + g() % 4000;
        const size_t nbits = nbase + (g() % 40 == 0 ? g() % 20000 : 0);
        std::vector<uint8_t> data(nbits);
        for (auto& b : data) b = g() % 2;
        std::vector<uint8_t> burst = D::vdl2BurstBits(data);       // header, data, FEC, scrambled
        if (burst.size() < 25) continue;
        D::vdl2Scramble(burst, 0);                                 // descramble the lot
        const size_t body = burst.size() - 25;
        const int flips = g() % 4 == 0 ? (int)(g() % 40) : (int)(g() % 5);
        for (int k = 0; k < flips && body; k++) burst[25 + g() % body] ^= 1;
        if (g() % 12 == 0) for (size_t k = 25; k < burst.size(); k++) burst[k] = g() % 2;   // noise
        D::Vdl2Layout la;
        const uint32_t len = (uint32_t)data.size();
        D::vdl2Layout(len, la);
        std::vector<uint8_t> da{1, 2, 3};
        int fa = -7;
        const bool oa = D::vdl2BurstData(burst.data() + 25, la, len, da, fa);
        dig.add(oa, da, fa);
        ok += oa;
    }
    printf("  burst bodies: %ld corrected\n", ok);
}

static void testHdlc(long iters) {
    long framesSeen = 0;
    for (long i = 0; i < iters; i++) {
        const size_t nf = 1 + g() % 3;
        std::vector<std::vector<uint8_t>> frames;
        for (size_t k = 0; k < nf; k++) {
            std::vector<uint8_t> f = rndBytes(g() % 12 == 0 ? g() % 3 : 2 + g() % 40);
            if (g() % 3) for (auto& b : f) if (g() % 4 == 0) b = g() % 2 ? 0x7E : 0xFF;
            if (g() % 2) { const uint16_t c = D::vdl2Fcs(f.data(), f.size()); f.push_back((uint8_t)c); f.push_back((uint8_t)(c >> 8)); }
            frames.push_back(f);
        }
        std::vector<uint8_t> bits = D::vdl2HdlcBits(frames);
        if (g() % 3 == 0) for (int k = (int)(g() % 5); k > 0 && !bits.empty(); k--) bits[g() % bits.size()] ^= 1;
        if (g() % 8 == 0) bits.resize(g() % (bits.size() + 1));
        if (g() % 10 == 0) { bits.clear(); const size_t n = g() % 300; for (size_t k = 0; k < n; k++) bits.push_back(g() % 5 ? 1 : 0); }
        const auto a = D::vdl2HdlcFrames(bits);
        dig.add((uint64_t)a.size());
        for (const auto& fr : a) dig.add(fr);
        framesSeen += (long)a.size();
        for (const auto& f : frames) dig.add(D::vdl2FcsOk(f.data(), f.size()), D::vdl2Fcs(f.data(), f.size()));
    }
    printf("  hdlc: %ld frames extracted\n", framesSeen);
}

static void testAddr(long iters) {
    for (int t = -3; t < 20; t++) dig.add(D::vdl2AddrTypeName(t));
    for (long i = 0; i < iters; i++) {
        D::Vdl2Addr a;
        a.addr = r32(); a.type = (int)(g() % 16); a.status = g() % 2;
        const bool last = g() % 2;
        uint8_t oa[4];
        D::vdl2PutAddr(oa, a, last);
        dig.bytes(oa, 4);
        uint8_t raw[4] = {rb(), rb(), rb(), rb()};
        const D::Vdl2Addr x = D::vdl2GetAddr(raw);
        dig.add(x.addr, x.type, x.status);
    }
}

static std::vector<uint8_t> info(int kind) {
    switch (kind) {
    case 0: return ORD_CALL(D::vdl2AcarsInfo, RS(7, "ABCDEFGHIJ-.0123"), RS(2, "ABCH1_2Q0"), (char)('0' + g() % 10), RS(g() % 80, "ABC xyz 0123 N25E055 /.-,\n"));
    case 1: {
        std::vector<uint8_t> a = rndBytes(g() % 20), b = rndBytes(g() % 20), u = rndBytes(g() % 50);
        return ORD_CALL(D::vdl2X25ClnpData, (int)(g() % 4096), (int)(g() % 8), (int)(g() % 8), a, b, u);
    }
    case 2: return ORD_CALL(D::vdl2X25CallRequest, (int)(g() % 4096), RS(g() % 12, "0123456789"), RS(g() % 12, "0123456789"));
    case 3: return ORD_CALL(D::vdl2XidGsif, (double)((int)(g() % 1800) - 900) / 10.0, (double)((int)(g() % 3600) - 1800) / 10.0, RS(g() % 30, "OMDBLTBYEGLL "));
    case 4: return ORD_CALL(D::vdl2XidAircraftLe, (double)((int)(g() % 1800) - 900) / 10.0, (double)((int)(g() % 3600) - 1800) / 10.0, (int)(g() % 45000), RS(g() % 6, "OMDB"));
    default: return rndBytes(g() % 60);
    }
}

// a random X.25 packet: the packet types of the decoder and random bytes around them
static std::vector<uint8_t> x25Packet() {
    static const uint8_t types[] = {0x0B, 0x0F, 0x13, 0x17, 0x1B, 0x1F, 0xFB, 0xFF, 0x23, 0x27, 0xF1, 0x01, 0x21, 0x05, 0x25, 0x09, 0x29, 0x00, 0x02, 0x10, 0x32};
    std::vector<uint8_t> p = {(uint8_t)(0x10 | g() % 16), rb(), g() % 3 ? types[g() % sizeof types] : rb()};
    const size_t n = g() % 30;
    for (size_t i = 0; i < n; i++) p.push_back(rb());
    if (!(p[2] & 1) && g() % 2) { p.push_back(g() % 2 ? 0x81 : (g() % 2 ? 0x82 : 0x85)); for (size_t i = g() % 25; i > 0; i--) p.push_back(rb()); if (p[3] == 0x81 && p.size() > 8) { p[4] = (uint8_t)(g() % 40); p[7] = 0x1C; } }
    return p;
}

static void testFrames(long iters) {
    long parsed = 0, withPos = 0, acars = 0, x25 = 0, xidn = 0;
    for (long i = 0; i < iters; i++) {
        D::Vdl2Addr d, s;
        d.addr = s.addr = r32(); s.addr = r32(); d.type = (int)(g() % 8); s.type = g() % 2 ? 1 : (int)(g() % 8); d.status = g() % 2; s.status = g() % 2;
        std::vector<uint8_t> f;
        const int mode = (int)(g() % 12);
        if (mode < 7) {
            const int kind = mode;
            std::vector<uint8_t> inf = kind == 6 ? x25Packet() : info(kind);
            uint8_t ctrl;
            switch (g() % 5) {
            case 0: ctrl = ORD_CALL(D::vdl2ControlI, (int)(g() % 8), (int)(g() % 8), g() % 2); break;
            case 1: { const uint32_t c2 = (g() % 4) << 2, c4 = (g() % 2) << 4, c5 = (g() % 8) << 5; ctrl = (uint8_t)(0x01 | c2 | c4 | c5); } break;
            case 2: ctrl = (uint8_t)(D::kVdl2CtrlXID | (g() % 2 ? 0x10 : 0)); break;
            case 3: ctrl = (uint8_t)(D::kVdl2CtrlUI | (g() % 2 ? 0x10 : 0)); break;
            default: ctrl = rb(); break;
            }
            if (kind == 3 || kind == 4) ctrl = (uint8_t)(D::kVdl2CtrlXID | (g() % 2 ? 0x10 : 0));
            f = D::vdl2Frame(d, s, ctrl, inf);
            f.resize(f.size() - 2);                                  // the FCS is checked before parsing
            if (mode == 5 || g() % 9 == 0) f = mutate(f);
        } else {
            f = rndBytes(g() % 40);
            if (f.size() >= 9 && g() % 2) { const D::Vdl2Addr x{r32() & 0xFFFFFF, (int)(g() % 8), false}; D::vdl2PutAddr(&f[0], x, false); D::vdl2PutAddr(&f[4], d, true); }
        }
        dect2::Vdl2Message a;
        const bool oa = D::vdl2ParseFrame(f.data(), f.size(), a);
        dig.add(oa); if (oa) addMsg(a);
        parsed += oa; withPos += oa && a.hasPos; acars += oa && a.acars; x25 += oa && a.proto == "X.25"; xidn += oa && a.proto == "XID";
    }
    printf("  frames: %ld parsed, %ld with a position, %ld ACARS, %ld X.25, %ld XID\n", parsed, withPos, acars, x25, xidn);
}

int main() {
    const char* itc = getenv("ONAIR_DIFF_ITERS");
    const long iters = itc ? atol(itc) : 200000;
    testBitLayer(iters);
    testRs(iters);
    testBurst(iters / 4);
    testHdlc(iters);
    testAddr(iters);
    testFrames(iters);
    return golden::check("vdl2_proto_rs", dig, 0x7434af28f3a034f9ull, !itc);
}
