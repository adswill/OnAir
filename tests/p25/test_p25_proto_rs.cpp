// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust P25 frame layer (rust/onair_p25, dect2::p25): NID over all messages and noisy words, Reed-Solomon of every shape with 0..t+3
// errors, Golay / Hamming over their word space, CRC, trellis with noisy / NaN / random levels, TSBK, link control, encryption sync and header words, the IMBE
// interleave and the names, over ONAIR_DIFF_ITERS (default 200000) seeded inputs per family. Every result goes into a digest (tests/common/golden.h; floats
// widened and rounded to 1e-6) compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/p25/p25_proto.h"
#include "../common/golden.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::p25;

static golden::Digest dig;
static void B(const void* p, size_t n) { dig.bytes(p, n); }

static std::mt19937 g(1357);
static uint32_t r32() { return g(); }
static uint64_t r64() { const uint64_t hi = (uint64_t)g() << 32; return hi | g(); }   // (two draws in one expression would be evaluated in an unspecified order)

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;

    // tables and names
    B(A::kSync, sizeof A::kSync);
    for (int d = -3; d < 20; d++) dig.add(A::duidName(d), A::frameLen(d));
    for (int o = -2; o < 70; o++) dig.add(A::opcodeName(o));
    for (int a = -2; a < 260; a++) dig.add(A::algName(a));
    for (long i = 0; i < iters; i++) { const int ch = (int)r32(); dig.add(A::channelText(ch)); }

    // NID: every message, then noisy and random words
    for (int m = 0; m < 65536; m++) {
        uint8_t a[64];
        A::nidEncode(m >> 4, m & 15, a);
        B(a, 64);
    }
    for (long i = 0; i < iters / 2; i++) {
        uint8_t w[64];
        if (g() % 4 == 0) { for (auto& x : w) x = (uint8_t)(g() & 1); }
        else {
            const int nac = (int)r32(), did = (int)r32();
            A::nidEncode(nac, did, w);
            const int nf = (int)(g() % 16);
            for (int e = 0; e < nf; e++) w[g() % 64] ^= 1;
        }
        int na = 7, da = 8, ea = -1;
        const bool oa = A::nidDecode(w, na, da, &ea);
        dig.add(oa, ea, na, da);
        if (i % 16 == 0) dig.add(A::nidDecode(w, na, da));
    }

    // Reed-Solomon: the three shapes of the standard and random shapes
    static const int shapes[][2] = {{36, 20}, {24, 12}, {24, 16}, {63, 51}, {63, 0}, {10, 10}, {1, 0}, {2, 1}};
    for (long i = 0; i < iters; i++) {
        int n, k;
        if (g() % 3 == 0) { n = 1 + (int)(g() % 63); k = (int)(g() % (unsigned)(n + 1)); }
        else { const int s = (int)(g() % 8); n = shapes[s][0]; k = shapes[s][1]; }
        uint8_t a[64], b[64];
        for (int j = 0; j < 64; j++) a[j] = b[j] = (uint8_t)(g() % 64);
        A::rsEncode(b, n, k);
        B(b, 64);
        const int t = (n - k) / 2;
        const int ne = (int)(g() % (unsigned)(t + 4));
        for (int e = 0; e < ne; e++) b[g() % (unsigned)n] ^= (uint8_t)(1 + g() % 63);
        if (g() % 12 == 0) for (int j = 0; j < n; j++) b[j] = (uint8_t)(g() % 64);
        memcpy(a, b, 64);
        int ea = 5;
        const bool oa = A::rsDecode(a, n, k, &ea);
        dig.add(oa, ea); B(a, 64);
        if (i % 8 == 0) { uint8_t c[64]; memcpy(c, b, 64); dig.add(A::rsDecode(c, n, k)); B(c, 64); }
    }

    // Hamming, Golay, CRC
    for (int h = -2; h < 70; h++) dig.add(A::hamming10Encode(h), A::golay18Encode(h));
    for (int d = -2; d < 4200; d++) dig.add(A::golay24Encode(d));
    for (long i = 0; i < iters; i++) {
        const uint32_t w = r32();
        int ea = -1;
        const int h1 = A::hamming10Decode((uint16_t)w, &ea);
        dig.add(h1, ea);
        const uint32_t g24 = g() % 3 ? (A::golay24Encode((int)(w & 0xFFF)) ^ (r32() & r32() & r32() & 0xFFFFFF)) : w;
        const int d1 = A::golay24Decode(g24, &ea);
        dig.add(d1, ea);
        const uint32_t g18 = g() % 3 ? (A::golay18Encode((int)(w & 63)) ^ (r32() & r32() & r32() & 0x3FFFF)) : w;
        const int e1 = A::golay18Decode(g18, &ea);
        dig.add(e1, ea);
        dig.add(A::golay24Decode(g24), A::hamming10Decode((uint16_t)w), A::golay18Decode(g18));
        uint8_t bits[100]; for (auto& x : bits) x = (uint8_t)(g() % 5 ? (g() & 1) : g());
        const int nFull = (int)(g() % 101); const int n = nFull - (g() % 20 == 0 ? 3 : 0);
        dig.add(A::crcCcitt(bits, n));
    }

    // trellis
    for (long i = 0; i < iters; i++) {
        uint8_t bits[96]; for (auto& x : bits) x = (uint8_t)(g() & 1);
        uint8_t db[98];
        A::trellisEncode(bits, db);
        B(db, 98);
        float lv[98];
        const int mode = (int)(g() % 6);
        const float sigma = (float)(g() % 40) / 10.f;
        for (int k = 0; k < 98; k++) {
            const float clean = (float)A::dibitToSym(db[k]);
            const float noise = ((float)(g() & 0xFFFF) / 65536.f - 0.5f) * 2.f * sigma;
            switch (mode) {
            case 0: lv[k] = clean; break;
            case 1: case 2: lv[k] = clean + noise; break;
            case 3: lv[k] = ((float)r32() / 4294967296.f - 0.5f) * 12.f; break;
            case 4: lv[k] = g() % 40 == 0 ? std::nanf("") : clean + noise; break;
            default: lv[k] = g() % 60 == 0 ? INFINITY : (g() % 30 == 0 ? -1e30f : clean + noise); break;
            }
        }
        uint8_t oa[96];
        memset(oa, 0, 96);
        float ma = -1;
        const bool ra = A::trellisDecode(lv, oa, &ma);
        dig.add(ra, ma); if (ra) B(oa, 96);
        if (i % 8 == 0) dig.add(A::trellisDecode(lv, oa));
    }

    // TSBK, link control, encryption sync, header word, IMBE
    for (long i = 0; i < iters; i++) {
        A::Tsbk ta;
        ta.last = g() & 1; ta.prot = g() & 1; ta.opcode = (int)r32(); ta.mfid = (int)r32(); ta.args = r64();
        uint8_t bb[96];
        A::tsbkBits(ta, bb);
        B(bb, 96);
        if (g() % 2) bb[g() % 96] ^= 1;
        if (g() % 10 == 0) for (auto& x : bb) x = (uint8_t)(g() & 1);
        A::Tsbk pa;
        pa.mfid = 99;
        const bool oa = A::tsbkParse(bb, pa);
        dig.add(oa, pa.last, pa.prot, pa.opcode, pa.mfid, pa.args);

        A::Lc la;
        const int lcos[] = {A::kLcGroup, A::kLcUnit, A::kLcTerm, (int)(g() % 64), (int)r32()};
        la.lco = lcos[g() % 5]; la.mfid = g() % 3 ? (int)(g() % 3) : (int)r32(); la.prot = g() & 1;
        la.svcOpts = (int)r32(); la.tg = g() % 4 ? (int)(g() % 70000) : -1; la.src = (int)r32(); la.dst = g() % 4 ? (int)(g() % 20000000) : -(int)(g() % 5); la.raw = r64();
        uint8_t hb[12];
        A::lcHexbits(la, hb);
        B(hb, 12);
        uint8_t hin[12];
        for (auto& x : hin) x = (uint8_t)(g() % 20 == 0 ? g() : g() % 64);
        if (g() % 2) memcpy(hin, hb, 12);
        A::Lc xa;
        xa.tg = 4242;
        const bool qa = A::lcParse(hin, xa);
        dig.add(qa, xa.lco, xa.mfid, xa.prot, xa.svcOpts, xa.tg, xa.src, xa.dst, xa.raw);

        A::Crypto ca;
        for (int k = 0; k < 9; k++) ca.mi[k] = (uint8_t)g();
        ca.algId = (int)r32(); ca.keyId = (int)r32(); ca.mfid = (int)r32(); ca.tg = g() % 4 ? (int)(g() % 70000) : -1;
        uint8_t e1[16], u1[20];
        A::esHexbits(ca, e1);
        A::hduHexbits(ca, u1);
        B(e1, 16); B(u1, 20);
        uint8_t hx[20]; for (auto& x : hx) x = (uint8_t)(g() % 30 == 0 ? g() : g() % 64);
        A::Crypto sa;
        sa.mfid = 17; sa.tg = 18;
        A::esParse(hx, sa);
        B(sa.mi, 9); dig.add(sa.algId, sa.keyId, sa.mfid, sa.tg);
        A::hduParse(hx, sa);
        B(sa.mi, 9); dig.add(sa.algId, sa.keyId, sa.mfid, sa.tg);

        uint8_t d72[72]; for (auto& x : d72) x = (uint8_t)(g() % 7 == 0 ? g() : g() & 3);
        char fa[8][23];
        A::imbeDeinterleave(d72, fa);
        B(fa, sizeof fa);
        uint8_t i1[72];
        char m[8][23]; for (auto& row : m) for (auto& c : row) c = (char)g();
        A::imbeInterleave(m, i1);
        B(i1, 72);
        uint8_t f1[144];
        A::imbeFlatten(m, f1);
        B(f1, 144);
        char u1c[8][23];
        uint8_t bits144[144]; for (auto& x : bits144) x = (uint8_t)g();
        A::imbeUnflatten(bits144, u1c);
        B(u1c, sizeof u1c);
    }
    return golden::check("p25_proto_rs", dig, 0x089ce94f3fd2ba14ull, !it);
}
