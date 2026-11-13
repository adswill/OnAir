// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust DMR frame layer (rust/onair_dmr, dect2::dmr): the short codes over their whole word space, Reed-Solomon, CRCs, BPTC, embedded LC,
// short LC, CACH, trellis (noisy, NaN and random soft values), rate 1, sync matching, burst assembly and slicing, slot type / EMB, link control, CSBK, data headers
// and blocks, every text format and the talker alias, over ONAIR_DIFF_ITERS (default 200000) seeded random / mutated inputs per family. Every result goes into a
// digest (tests/common/golden.h) compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/dmr/dmr_proto.h"
#include "../common/golden.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::dmr;

static golden::Digest dig;
static void B(const void* p, size_t n) { dig.bytes(p, n); }

static std::mt19937 g(2468);
static uint32_t r32() { return g(); }
static A::Bits rndBits(size_t n) { A::Bits b(n); for (auto& x : b) x = (uint8_t)(g() & 1); return b; }
static void flips(A::Bits& b, int n) { for (int i = 0; i < n && !b.empty(); i++) b[g() % b.size()] ^= 1; }
static void rndBytes(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)g(); }

static void fecTests(long iters) {
    // the short codes over every word
    for (unsigned i = 0; i < 256; i++) dig.add(A::golay2008Parity(i));
    for (unsigned i = 0; i < (1u << 20); i += (i < 4096 ? 1 : 3)) {
        unsigned a = 77;
        const int ea = A::golay2008Decode(i, a);
        dig.add(ea, a);
    }
    for (unsigned i = 0; i < 128; i++) dig.add(A::qr1676Parity(i));
    for (unsigned i = 0; i < (1u << 16); i++) {
        unsigned a = 77;
        dig.add(A::qr1676Decode(i, a), a);
    }
    for (unsigned i = 0; i < 16; i++) dig.add(A::hamming743Parity(i));
    for (unsigned i = 0; i < 256; i++) {
        unsigned a = 77;
        dig.add(A::hamming743Decode(i, a), a);
    }
    for (int k = 0; k < 4; k++) {
        const A::HammingKind ka = (A::HammingKind)k;
        dig.add(A::hammingInfoBits(ka), A::hammingParityBits(ka));
        const int tot = A::hammingInfoBits(ka) + A::hammingParityBits(ka);
        for (unsigned i = 0; i < (1u << (A::hammingInfoBits(ka) + 2)); i++) dig.add(A::hammingParity(ka, i));
        for (unsigned w = 0; w < (1u << (tot + 1)); w++) {   // one bit more than the word: the high bit must not matter the same way
            unsigned a = w;
            dig.add(A::hammingCorrect(ka, a), a);
        }
    }
    for (long i = 0; i < iters; i++) {
        uint8_t m[9], w[12], pa[3];
        rndBytes(m, 9);
        A::rs129Parity(m, pa);
        B(pa, 3);
        memcpy(w, m, 9); memcpy(w + 9, pa, 3);
        const int errs = (int)(g() % 5);
        for (int e = 0; e < errs; e++) w[g() % 12] ^= (uint8_t)(1 + g() % 255);
        if (g() % 16 == 0) rndBytes(w, 12);
        uint8_t wa[12];
        memcpy(wa, w, 12);
        dig.add(A::rs129Correct(wa)); B(wa, 12);
        // checksums
        uint8_t d[40]; const size_t n = g() % 41; rndBytes(d, 40);
        dig.add(A::crcCcitt(d, n), A::crc32Msg(d, n));
        const uint32_t v = r32();
        dig.add(A::crc8Short(v), A::crc8Short(v & 0x0FFFFFFF));
        dig.add(A::crc9(d, n, v));
        dig.add(A::checksum5(d));
    }
    dig.add(A::crcCcitt(nullptr, 0), A::crc32Msg(nullptr, 0));

    for (long i = 0; i < iters; i++) {
        // BPTC (196,96)
        const A::Bits info = rndBits(96);
        A::Bits tr;
        A::bptc196Encode(info, tr);
        dig.add(tr);
        A::Bits tx = tr;
        const int nf = (int)(g() % 9);
        flips(tx, nf);
        if (g() % 8 == 0) tx = rndBits(196);
        A::Bits ia(96, 5);
        const int ra = A::bptc196Decode(tx, ia);
        dig.add(ra, ia);

        // embedded LC
        uint8_t lc[9]; rndBytes(lc, 9);
        A::Bits fr[4];
        A::embLcEncode(lc, fr);
        for (int f = 0; f < 4; f++) dig.add(fr[f]);
        const int nfl = (int)(g() % 6);
        for (int e = 0; e < nfl; e++) fr[g() % 4][g() % 32] ^= 1;
        if (g() % 10 == 0) for (int f = 0; f < 4; f++) fr[f] = rndBits(32);
        uint8_t la[9];
        memset(la, 0xA5, 9);
        const int ea = A::embLcDecode(fr, la);
        dig.add(ea); B(la, 9);

        // short LC
        const uint32_t s28 = r32();
        A::Bits sr[4];
        A::shortLcEncode(s28, sr);
        for (int f = 0; f < 4; f++) dig.add(sr[f]);
        const int ns = (int)(g() % 5);
        for (int e = 0; e < ns; e++) sr[g() % 4][g() % 17] ^= 1;
        if (g() % 10 == 0) for (int f = 0; f < 4; f++) sr[f] = rndBits(17);
        uint32_t va = 0xDEADBEEF;
        const int xa = A::shortLcDecode(sr, va);
        dig.add(xa, va);

        // CACH
        const int at = (int)r32() - (1 << 30), tc = (int)r32(), ls = (int)r32();
        const unsigned pl = r32();
        A::Bits cr;
        A::cachEncode(at, tc, ls, pl, cr);
        dig.add(cr);
        A::Bits in = g() % 3 ? cr : rndBits(24);
        flips(in, (int)(g() % 3));
        int a1 = 11, t1 = 12, l1 = 13;
        unsigned p1 = 99;
        const int c1 = A::cachDecode(in, a1, t1, l1, p1);
        dig.add(c1, a1, t1, l1, p1);

        // rate 1
        uint8_t d24[24]; rndBytes(d24, 24);
        A::Bits r1b;
        A::rate1Encode(d24, r1b);
        dig.add(r1b);
        A::Bits rx = g() % 2 ? r1b : rndBits(196);
        uint8_t o1[24]; bool p1o = false;
        A::rate1Decode(rx, o1, &p1o);
        B(o1, 24); dig.add(p1o);
        A::rate1Decode(rx, o1);
    }

    // rate 3/4 trellis: encode, decode of clean / noisy / damaged / random soft symbols
    for (long i = 0; i < iters; i++) {
        uint8_t d[18]; rndBytes(d, 18);
        A::Bits tr;
        A::trellis34Encode(d, tr);
        dig.add(tr);
        float sym[98];
        const int mode = (int)(g() % 6);
        const float sigma = (float)(g() % 40) / 10.f;
        for (int k = 0; k < 98; k++) {
            const float clean = (float)A::dibitToSymbol((unsigned)((tr[2 * k] << 1) | tr[2 * k + 1]));
            const float noise = ((float)(g() & 0xFFFF) / 65536.f - 0.5f) * 2.f * sigma;
            switch (mode) {
            case 0: sym[k] = clean; break;
            case 1: case 2: sym[k] = clean + noise; break;
            case 3: sym[k] = ((float)r32() / 4294967296.f - 0.5f) * 12.f; break;
            case 4: sym[k] = g() % 40 == 0 ? std::nanf("") : clean + noise; break;
            default: sym[k] = g() % 60 == 0 ? INFINITY : (g() % 30 == 0 ? -1e30f : clean + noise); break;
            }
        }
        uint8_t da[18];
        memset(da, 0x5A, 18);
        const int ra = A::trellis34Decode(sym, da);
        dig.add(ra); B(da, 18);
    }
}

static void addLc(const A::FullLc& a) { dig.add(a.pf, a.flco, a.fid, a.svc, a.dst, a.src); B(a.raw, 9); }
static void addDh(const A::DataHeader& a) {
    dig.add(a.dpf, a.sap, a.group, a.resp, a.dst, a.src, a.blocks, a.pad, a.fmf, a.fsn, a.ns, a.dd, a.sp, a.dp, a.resync, a.sarq, a.status);
}
static void rndDh(A::DataHeader& h) {
    h.dpf = g() % 5 ? (int)(g() % 16) : (int)r32() % 40;
    h.sap = (int)(g() % 20);
    h.group = g() & 1; h.resp = g() & 1;
    h.dst = r32() & 0xFFFFFF; h.src = r32() & 0xFFFFFF;
    h.blocks = (int)(g() % 130); h.pad = (int)(g() % 300) - 20;
    h.fmf = g() & 1; h.fsn = (int)(g() % 20); h.ns = (int)(g() % 10); h.dd = (int)(g() % 70);
    h.sp = (int)(g() % 9); h.dp = (int)(g() % 9); h.resync = g() & 1; h.sarq = g() & 1; h.status = (int)(g() % 1100);
}

static std::string rndText(size_t n) {   // bytes that exercise every text format: random, ASCII, valid UTF-8 / UTF-16 / UTF-32 with BOMs
    std::string s;
    switch (g() % 7) {
    case 0: for (size_t i = 0; i < n; i++) s += (char)g(); break;
    case 1: for (size_t i = 0; i < n; i++) s += (char)(32 + g() % 95); break;
    case 2: for (size_t i = 0; i < n; i++) s += "aZ \xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\r\n\t\x01"[g() % 15]; break;
    case 3: if (n >= 2 && g() % 2) { s += "\xFF\xFE"; } for (size_t i = 0; i < n; i++) s += (char)(g() % 3 ? (g() % 2 ? 0 : 0xD8 + g() % 8) : g()); break;
    case 4: if (n >= 2 && g() % 2) { s += "\xFE\xFF"; } for (size_t i = 0; i < n; i++) s += (char)(g() % 3 ? (g() % 2 ? 0 : 0xDC + g() % 4) : g()); break;
    case 5: for (size_t i = 0; i < n; i++) s += (char)(g() % 4 ? 0 : g() % 3 ? 1 : 0x11); break;
    default: for (size_t i = 0; i < n; i++) s += (char)(g() % 2 ? (g() % 4) : 0x80 + g() % 0x80); break;
    }
    return s;
}

static void protoTests(long iters) {
    // sync patterns and names
    B(A::kSyncWords, sizeof A::kSyncWords);
    for (int s = -3; s < 14; s++) {
        dig.add(A::syncName(s), A::syncDirectSlot(s));
        if (s >= 0 && s < 9) { int8_t a[24]; A::syncSymbols(s, a); B(a, 24); }
    }
    for (int f = -2; f < 6; f++) for (int v = 0; v < 2; v++) dig.add(A::syncOf(f, v));
    for (int dt = -5; dt < 40; dt++) dig.add(A::dataTypeName(dt), A::dataTypeShort(dt));
    for (int f = -2; f < 70; f++) dig.add(A::flcoName(f), A::dpfName(f), A::sapName(f), A::ddName(f));
    for (int op = 0; op < 70; op++) for (int fid = 0; fid < 2; fid++) dig.add(A::csbkName(op, fid));
    for (int s = 0; s < 256; s++) dig.add(A::serviceOptionsText((uint8_t)s));
    for (int dt = -2; dt < 14; dt++) for (int c = 0; c < 2; c++) dig.add(A::dataBlockBytes(dt, c));

    for (long i = 0; i < iters; i++) {
        // match sync on random fields and on mutated patterns
        A::Bits centre = rndBits(48);
        if (g() % 4) { A::Bits w; A::putBits(w, A::kSyncWords[g() % 9], 48); centre = w; flips(centre, (int)(g() % 14)); }
        const int md = (int)(g() % 26);
        int da = 0;
        dig.add(A::matchSync(centre, md, &da), da);
        dig.add(A::matchSync(centre, md));

        // bursts: data and voice, with sync or embedded, then slicing
        const int cc = (int)r32() - (1 << 30), dt = (int)(g() % 20) - 2;
        const A::Bits pay = rndBits(196);
        const int sy = (int)(g() % 11) - 2;
        const int symOk = sy >= 9 ? -1 : sy;   // an index past the table was undefined in the C++ reference
        A::Bits bb = A::makeDataBurst(cc, dt, pay, symOk);
        dig.add(bb);
        const A::Bits vs = rndBits(216), e32 = rndBits(32);
        const int pi = (int)r32(), ls = (int)r32();
        A::Bits vb = A::makeVoiceBurst(vs, symOk, cc, pi, ls, e32);
        dig.add(vb);
        A::Bits burst = (g() % 3) ? bb : (g() % 2 ? vb : rndBits(264));
        flips(burst, (int)(g() % 6));
        if (g() % 16 == 0) burst.resize(g() % 264);
        const A::SlotTypeInfo sa = A::burstSlotType(burst);
        dig.add(sa.ok, sa.cc, sa.dt, sa.errors);
        if (burst.size() >= 148) {   // the C++ reference read the embedded bits unchecked
            const A::EmbInfo xa = A::burstEmb(burst);
            dig.add(xa.ok, xa.cc, xa.pi, xa.lcss, xa.errors, xa.emb32);
        }
        std::vector<int8_t> y1;
        A::bitsToSymbols(burst, y1);
        dig.add(y1);

        // full link control
        uint8_t raw[9]; rndBytes(raw, 9);
        A::FullLc pa;
        pa.src = 5;
        A::parseFullLc(raw, pa);
        addLc(pa);
        A::FullLc qa;
        qa.pf = g() & 1; qa.flco = (int)r32(); qa.fid = (int)r32(); qa.svc = (uint8_t)g(); qa.dst = r32(); qa.src = r32();
        uint8_t o1[9];
        A::packFullLc(qa, o1);
        B(o1, 9);

        // header / terminator bursts, CRC blocks, CSBK
        const bool term = g() & 1;
        A::Bits lb;
        A::lcBurstInfo(raw, term, lb);
        dig.add(lb);
        flips(lb, (int)(g() % 3));
        if (g() % 12 == 0) lb = rndBits(96);
        uint8_t c1[9];
        memset(c1, 0x33, 9);
        dig.add(A::lcBurstDecode(lb, term, c1)); B(c1, 9);
        uint8_t d10[10]; rndBytes(d10, 10);
        const uint16_t mask = (uint16_t)g();
        A::Bits ib;
        A::crcBlockInfo(d10, mask, ib);
        dig.add(ib);
        if (g() % 2) flips(ib, (int)(g() % 3));
        uint8_t k1[10];
        const bool ca = A::crcBlockCheck(ib, mask, k1);
        dig.add(ca); B(k1, 10);
        A::Csbk xa;
        xa.lb = g() & 1; xa.pf = g() & 1; xa.opcode = (int)r32(); xa.fid = (int)r32();
        rndBytes(xa.data, 8);
        A::Bits kb;
        A::csbkInfo(xa, kb);
        dig.add(kb);
        if (g() % 2) flips(kb, (int)(g() % 3));
        A::Csbk ya;
        ya.opcode = 77;
        const bool pA = A::csbkParse(kb, ya);
        dig.add(pA, ya.lb, ya.pf, ya.opcode, ya.fid); B(ya.data, 8);

        // data headers: from fields and from random octets
        A::DataHeader ha;
        rndDh(ha);
        uint8_t h2[10];
        A::packDataHeader(ha, h2);
        B(h2, 10);
        uint8_t hin[10]; rndBytes(hin, 10);
        if (g() % 2) memcpy(hin, h2, 10);
        A::DataHeader pha;
        pha.fsn = 9;
        const bool oa = A::parseDataHeader(hin, pha);
        dig.add(oa); addDh(pha);

        // data blocks
        const int bdt = (int)(g() % 14);
        const bool conf = g() & 1;
        uint8_t user[30]; rndBytes(user, 30);
        const size_t un = g() % 31;
        const unsigned dbsn = r32();
        A::Bits pa1;
        A::dataBlockEncode(bdt, user, un, conf, dbsn, pa1);
        dig.add(pa1);
        uint8_t oct[24]; rndBytes(oct, 24);
        A::DataBlock da1;
        da1.dbsn = 55; da1.crcOk = false;
        const bool x1 = A::dataBlockParse(bdt, oct, conf, da1);
        dig.add(x1, da1.bytes, da1.dbsn, da1.crcOk, da1.confirmed);
        // a well formed confirmed block parses with a good CRC
        if (conf && (bdt == 7 || bdt == 8 || bdt == 10)) {
            A::Bits p;
            A::dataBlockEncode(bdt, user, un, true, dbsn, p);
            uint8_t ob[24]; rndBytes(ob, 24);
            const int nb = A::dataBlockBytes(bdt, true);
            uint8_t full[24] = {};
            const unsigned crc = (A::crc9(user, (size_t)nb, dbsn & 0x7F) ^ (bdt == 8 ? A::kMaskRate34 : bdt == 10 ? A::kMaskRate1 : A::kMaskRate12)) & 0x1FF;
            full[0] = (uint8_t)(((dbsn & 0x7F) << 1) | (crc >> 8)); full[1] = (uint8_t)crc; memcpy(full + 2, user, (size_t)nb);
            A::DataBlock e1;
            const bool good = A::dataBlockParse(bdt, full, true, e1);
            dig.add(good, e1.crcOk, e1.bytes);
            if (!e1.crcOk) { printf("a well formed confirmed block must parse with a good CRC\n"); return; }
        }

        // text of every format
        const std::string t = rndText(g() % 64);
        const int fmt = (int)(g() % 30) - 3;
        const bool ta = g() & 1;
        dig.add(A::textFromBytes((const uint8_t*)t.data(), t.size(), fmt, ta));
    }

    // talker alias: random sequences of header and block LCs (also with a wrong opcode in between)
    for (long i = 0; i < iters; i++) {
        A::TalkerAlias a;
        const int steps = 1 + (int)(g() % 6);
        for (int s = 0; s < steps; s++) {
            uint8_t raw[9]; rndBytes(raw, 9);
            if (g() % 3 == 0) raw[2] = (uint8_t)((g() % 4) << 6 | (g() % 32) << 1 | (g() & 1));
            A::FullLc la;
            la.flco = g() % 6 ? (int)(4 + g() % 4) : (int)(g() % 64);
            memcpy(la.raw, raw, 9);
            if (s == 0 && g() % 3) la.flco = 4;
            a.add(la);
            dig.add(a.format, a.length, a.have); B(a.data, 28);
            dig.add(a.complete(), a.text());
        }
    }
    for (int f = 0; f < 4; f++) for (int len = 0; len < 40; len++) {
        A::FullLc pa[4];
        const std::string al(len, 'x');
        dig.add(A::talkerAliasPdus(al, f, pa)); B(pa[0].raw, 9);
    }
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;
    fecTests(iters);
    protoTests(iters);
    return golden::check("dmr_proto_rs", dig, 0x74ed06bdeefc0fb2ull, !it);
}
