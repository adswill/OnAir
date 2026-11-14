// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the DVB service information layers (the Rust crate onair_dvbsi behind dect2::text, dect2::dvbsi and the teletext / DVB subtitle decoders):
// broadcast character sets and the DVB text field, Hamming codes, G0 subsets, teletext data units, the page renderer and a whole TeletextDecoder fed with PES
// packets, DVB subtitle segment parsers, colours, run-length pixel strings, blending and a whole DvbSubDecoder fed with PES packets, over ONAIR_DIFF_ITERS
// (default 200000) seeded inputs per family. Every result goes into a digest (tests/common/golden.h; doubles rounded to 1e-6) compared with the one recorded from
// the C++ layers the Rust code replaced.
#include "dect2/common/text_charset.h"
#include "dect2/tv/dvb_text.h"
#include "dect2/tv/dvbsub.h"
#include "dect2/tv/teletext.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
namespace A = dect2;
namespace AT = dect2::text;
namespace DV = dect2::dvbsi;

// Two digests of the same run. `all` also takes what dvbText() makes of the tables that need the system's iconv (GB 2312, Big5, ISO 8859-x): glibc,
// libiconv and macOS give different text for invalid input, so that part is compared only on macOS, where it was recorded. `portable` leaves it out
// and is compared everywhere.
struct DualDigest {
    golden::Digest all, portable;
    template <class... T> void add(const T&... v) { all.add(v...); portable.add(v...); }
    void bytes(const void* p, size_t n) { all.bytes(p, n); portable.bytes(p, n); }
    void addIconv(const std::string& s) { all.add(s); }
} dig;
static long rsStat[16];
static std::mt19937 g(777);
static uint32_t r32() { return g(); }
using Buf = std::vector<uint8_t>;
static void rnd(Buf& b, size_t n) { for (size_t i = 0; i < n; i++) b.push_back((uint8_t)r32()); }
static uint32_t orOf(uint32_t a, uint32_t b) { return a | b; }   // with ORD_CALL: both parts draw, and the operands of | are not evaluated in a fixed order
static void put(Buf& b, std::initializer_list<int> v) { for (int x : v) b.push_back((uint8_t)x); }
static void cat(Buf& a, const Buf& b) { a.insert(a.end(), b.begin(), b.end()); }

// An exactly sized heap copy, so a sanitizer build sees reads past the end.
template <class F> static void exact(const Buf& s, F f) {
    uint8_t* p = s.empty() ? nullptr : (uint8_t*)malloc(s.size());
    if (p) memcpy(p, s.data(), s.size());
    f(p, s.size());
    free(p);
}

// ---- character sets --------------------------------------------------------------------------------------------------------------------------------------
static Buf textBytes(bool prefixed) {
    Buf b;
    if (prefixed) {
        static const int pre[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x1F, 0x20, 0x41, 0xA1, 0xC1, 0xFF};
        b.push_back((uint8_t)pre[r32() % 26]);
        if (b[0] == 0x10) put(b, {0x00, (int)(r32() % 16)});
    }
    const size_t n = r32() % 60;
    for (size_t i = 0; i < n; i++) {
        switch (r32() % 8) {
        case 0: b.push_back((uint8_t)(0xC1 + r32() % 15)); b.push_back((uint8_t)(0x20 + r32() % 96)); break;           // accent + letter
        case 1: b.push_back((uint8_t)(0xA1 + r32() % 94)); b.push_back((uint8_t)(0xA1 + r32() % 94)); break;           // GB 2312 pair
        case 2: b.push_back(0x8A); break;
        case 3: put(b, {0xC2, 0x80 + (int)(r32() % 32)}); break;                                                        // C1 control in UTF-8
        case 4: b.push_back((uint8_t)(r32() % 3 ? 0x20 : 0x0A)); break;
        case 5: { const uint32_t c = r32() % 6; static const Buf u[] = {{0xE2, 0x80, 0x8B}, {0xEF, 0xBB, 0xBF}, {0xC2, 0xA0}, {0xF0, 0x9F, 0x98, 0x80}, {0xC0, 0x80}, {0xED, 0xA0, 0x80}}; cat(b, u[c]); break; }
        default: b.push_back((uint8_t)r32());
        }
    }
    return b;
}

static void testText() {
    for (int c = 0; c < 256; c++) {
        const uint8_t one[2] = {(uint8_t)c, (uint8_t)(c ^ 0x55)};
        dig.add(AT::ebuLatin(one, 2), AT::iso6937(one, 2), AT::ucs2be(one, 2));
    }
    dig.add(AT::ebuLatin(nullptr, 0), AT::clean(""));
    long iters = getenv("ONAIR_DIFF_ITERS") ? atol(getenv("ONAIR_DIFF_ITERS")) : 200000;
    for (long i = 0; i < iters; i++) {
        const Buf b = textBytes(false);
        exact(b, [&](const uint8_t* p, size_t n) {
            dig.add(AT::ebuLatin(p, n));
            dig.add(AT::iso6937(p, n));
            dig.add(AT::ucs2be(p, n));
            for (int cs : {0, 4, 6, 15, 3, 99, -1}) dig.add(AT::dabCharset(cs, p, n));
            const std::string s((const char*)p, n);
            const std::string c1 = AT::clean(s);
            dig.add(c1);
            if (!c1.empty()) rsStat[0]++;
        });
        const Buf t = textBytes(true);
        exact(t, [&](const uint8_t* p, size_t n) {
            for (bool gb : {false, true}) {
                // Keep the original C++ golden corpus within its supported domain: its GB default only handled a leading GBK pair.
                // ASCII-leading GBK is covered by the DVB-C regression in test_tv_epg and the Rust charset tests.
                const bool referenceGb = gb && n > 0 && p[0] >= 0x81;
                const std::string a = DV::dvbText(p, (int)n, {referenceGb ? DV::TextEncoding::Gb2312 : DV::TextEncoding::Dvb});
                dig.addIconv(a);
                if (!a.empty()) rsStat[1]++;
            }
        });
    }
    dig.addIconv(DV::dvbText(nullptr, -3));
}

// ---- teletext --------------------------------------------------------------------------------------------------------------------------------------------
static uint8_t hammEnc(int nib) {   // a transmitted Hamming 8/4 byte, found through the decoder
    for (int b = 0; b < 256; b++) { bool ok = A::ttxHamming84((uint8_t)b) == nib; for (int k = 0; ok && k < 8; k++) ok = A::ttxHamming84((uint8_t)(b ^ (1 << k))) == nib; if (ok) return (uint8_t)b; }
    return 0;
}
static uint8_t flip(uint8_t b) { return r32() % 6 == 0 ? (uint8_t)(b ^ (1 << (r32() % 8))) : r32() % 40 == 0 ? (uint8_t)r32() : b; }
// A 44 byte data unit (not yet bit-reversed fields: built in the logical order, then reversed as in the PES).
static Buf teletextUnit(int mag, int row, bool header, int page) {
    uint8_t b[42];
    const int addr = (mag & 7) | row << 3;
    b[0] = flip(hammEnc(addr & 15)); b[1] = flip(hammEnc(addr >> 4));
    if (header) {
        b[2] = flip(hammEnc(page % 10)); b[3] = flip(hammEnc(page / 10 % 10));
        for (int i = 4; i < 10; i++) b[i] = flip(hammEnc((int)(r32() % 16)));
        for (int i = 10; i < 42; i++) b[i] = (uint8_t)r32();
    } else if (row == 27) {
        b[2] = flip(hammEnc(r32() % 6 ? 0 : (int)(r32() % 4)));
        for (int i = 3; i < 42; i++) b[i] = flip(hammEnc((int)(r32() % 16)));
        if (r32() % 4 == 0) { const int l = (int)(r32() % 6); b[3 + l * 6] = hammEnc(15); b[4 + l * 6] = hammEnc(15); }
    } else {
        for (int i = 2; i < 42; i++) b[i] = (uint8_t)(r32() % 3 ? 0x20 + r32() % 96 : r32());
    }
    Buf u{(uint8_t)(r32() % 3 ? 0xC0 | (r32() % 32) : r32()), (uint8_t)(r32() % 20 ? 0xE4 : r32() % 2 ? 0x27 : r32())};
    for (int i = 0; i < 42; i++) u.push_back(A::ttxReverse8(b[i]));
    return u;
}
static void addUnit(const A::TtxUnit& a) {
    dig.add(a.kind, a.serial, a.pageOk, a.erase, a.newsflash, a.subtitle, a.inhibit, a.mag, a.row, a.number, a.subcode, a.national);
    dig.bytes(a.head, 32); dig.bytes(a.rowData, 40);
    for (int i = 0; i < 6; i++) dig.add(a.linkAct[i], a.linkPage[i], a.linkSub[i]);
}
static Buf teletextPes(const std::vector<Buf>& units) {
    Buf p; put(p, {0, 0, 1, 0xBD, 0, 0, 0x80, 0x00, 0}); p[8] = (uint8_t)(r32() % 4 ? 0 : r32() % 8); rnd(p, p[8]);
    p.push_back((uint8_t)(r32() % 20 ? 0x10 + r32() % 16 : r32()));
    for (const Buf& u : units) { p.push_back((uint8_t)(r32() % 10 ? 0x02 + (r32() % 2) : r32())); p.push_back((uint8_t)(r32() % 20 ? 44 : r32() % 60)); cat(p, u); }
    return p;
}

static void testTeletext() {
    for (int b = 0; b < 256; b++) dig.add(A::ttxHamming84((uint8_t)b), A::ttxReverse8((uint8_t)b));
    for (int c = 0; c < 256; c++) for (int n = -2; n < 10; n++) dig.add(A::ttxG0((uint8_t)c, n));
    const long iters = getenv("ONAIR_DIFF_ITERS") ? atol(getenv("ONAIR_DIFF_ITERS")) : 200000;
    for (long i = 0; i < iters; i++) {
        uint8_t w[3] = {(uint8_t)r32(), (uint8_t)r32(), (uint8_t)r32()};
        dig.add(A::ttxHamming2418(w));
        if (A::ttxHamming2418(w) >= 0) rsStat[2]++;
        // data units
        const Buf u = (r32() % 4 == 0) ? [&] { Buf x; rnd(x, 44); return x; }() : ORD_CALL(teletextUnit, (int)(r32() % 9), r32() % 3 == 0 ? 0 : r32() % 4 == 0 ? 27 : (int)(r32() % 32), false, 0);   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
        const Buf hu = ORD_CALL(teletextUnit, (int)(r32() % 9), 0, true, (int)(r32() % 100));
        for (const Buf* x : {&u, &hu}) {
            exact(*x, [&](const uint8_t* p, size_t) {
                const A::TtxUnit a = A::ttxDecodeUnit(p);
                addUnit(a);
                if (a.kind >= 1 && a.kind <= 3) rsStat[3 + a.kind - 1]++;
            });
        }
        // PES walk
        std::vector<Buf> units; const int nu = (int)(r32() % 4);
        for (int k = 0; k < nu; k++) units.push_back(r32() % 3 ? ORD_CALL(teletextUnit, (int)(r32() % 9), (int)(r32() % 30), r32() % 3 == 0, (int)(r32() % 100)) : Buf(44, (uint8_t)r32()));
        Buf pes = teletextPes(units);
        if (r32() % 4 == 0) pes.resize(r32() % (pes.size() + 1));
        exact(pes, [&](const uint8_t* p, size_t n) {
            uint32_t oa[16];
            const size_t ca = A::ttxPesUnits(p, n, oa, 16);
            dig.add((uint64_t)ca); dig.bytes(oa, sizeof(uint32_t) * (ca < 16 ? ca : 16));
            if (ca) rsStat[6]++;
        });
        // renderer
        A::TtxPage pa;
        pa.national = (int)(r32() % 9) - 1;
        pa.rowMask = r32() % 5 ? r32() : 0xFFFFFFFFu;
        for (int r = 0; r < 25; r++) for (int c = 0; c < 40; c++) {
            const uint8_t v = (uint8_t)(r32() % 3 == 0 ? r32() % 32 : r32() % 5 == 0 ? r32() : 0x20 + r32() % 96);
            pa.rows[r][c] = v;
        }
        A::TtxGrid ga;
        A::ttxRender(pa, ga);
        for (int r = 0; r < 25; r++) {
            for (int c = 0; c < 40; c++) { const auto &x = ga[r][c]; dig.add(x.ch, x.mosaic, x.sextants, x.separated, x.fg, x.bg, x.dh, x.flash, x.conceal, x.boxed); }
            dig.add(A::ttxRowText(ga, r));
        }
        dig.add(A::ttxRowText(ga, -1), A::ttxRowText(ga, 25));
    }
    // whole decoders on the same PES stream
    for (long i = 0; i < iters / 40; i++) {
        A::TeletextDecoder da;
        da.setPid(100);
        const int np = (int)(1 + r32() % 12);
        for (int k = 0; k < np; k++) {
            std::vector<Buf> units;
            const int mag = (int)(r32() % 3), pg = (int)(r32() % 10);
            if (r32() % 3) units.push_back(teletextUnit(mag, 0, true, pg));
            const int nr = (int)(r32() % 8);
            for (int q = 0; q < nr; q++) units.push_back(teletextUnit(mag, r32() % 5 == 0 ? 27 : (int)(1 + r32() % 24), false, 0));
            if (r32() % 5 == 0) units.push_back(ORD_CALL(teletextUnit, (int)(r32() % 8), 0, true, (int)(r32() % 100)));
            Buf pes = teletextPes(units);
            if (r32() % 10 == 0) pes.resize(r32() % (pes.size() + 1));
            da.feedPes(pes.data(), pes.size());
        }
        dig.add((uint64_t)da.packets(), da.pages());
        for (int n : da.pages()) {
            dig.add(da.subpages(n));
            for (int sub : da.subpages(n)) {
                A::TtxPage x;
                const bool ba = da.page(n, x, sub);
                dig.add(ba, x.number, x.subcode, x.national, x.subtitle, x.newsflash, x.inhibit, x.rowMask, x.version, x.serial, x.complete);
                dig.bytes(&x.rows, sizeof x.rows); dig.bytes(x.fastext, sizeof x.fastext); dig.bytes(x.fastextSub, sizeof x.fastextSub);
                rsStat[7]++;
            }
        }
    }
}

// ---- DVB subtitles ---------------------------------------------------------------------------------------------------------------------------------------
static Buf pixelBlock() {   // sub-blocks of pixel data: random code strings, map tables, line ends
    Buf f;
    const int n = (int)(r32() % 6);
    for (int k = 0; k < n; k++) {
        switch (r32() % 7) {
        case 0: f.push_back(0xF0); break;
        case 1: put(f, {0x20, (int)(r32() & 0xFF), (int)(r32() & 0xFF)}); break;
        case 2: f.push_back(0x21); rnd(f, 4); break;
        case 3: f.push_back(0x22); rnd(f, 16); break;
        case 4: if (r32() % 15 == 0) f.push_back((uint8_t)r32()); break;
        default: { f.push_back((uint8_t)(0x10 + r32() % 3)); rnd(f, r32() % 30); if (r32() % 2) put(f, {0, 0}); }
        }
    }
    return f;
}
static Buf segment(int type, int page, const Buf& d) { Buf s{0x0F, (uint8_t)type, (uint8_t)(page >> 8), (uint8_t)page, (uint8_t)(d.size() >> 8), (uint8_t)d.size()}; cat(s, d); return s; }
static Buf pageSeg() { Buf d{(uint8_t)(r32() % 40), (uint8_t)ORD_CALL(orOf, (r32() % 16) << 4, (r32() % 4) << 2)}; const int n = (int)(r32() % 4); for (int i = 0; i < n; i++) put(d, {(int)(r32() % 3), 0, (int)(r32() % 2), (int)(r32() & 0xFF), 0, (int)(r32() % 20)}); if (r32() % 8 == 0) rnd(d, r32() % 5); return d; }
static Buf regionSeg() {
    Buf d{(uint8_t)(r32() % 3), (uint8_t)ORD_CALL(orOf, (r32() % 16) << 4, (r32() % 2) << 3), 0, (uint8_t)(1 + r32() % 40), 0, (uint8_t)(1 + r32() % 20), (uint8_t)(((r32() % 4) << 2)), (uint8_t)(r32() % 3), (uint8_t)r32(), (uint8_t)r32()};
    if (r32() % 12 == 0) { d[2] = (uint8_t)r32(); d[4] = (uint8_t)r32(); }
    const int n = (int)(r32() % 4);
    for (int i = 0; i < n; i++) { put(d, {0, (int)(r32() % 3), (int)ORD_CALL(orOf, (r32() % 3) << 6, r32() % 3), (int)(r32() % 30), 0, (int)(r32() % 10)}); if ((d[d.size() - 4] >> 6) != 0 && (d[d.size() - 4] >> 6) != 3) rnd(d, 2); }
    return d;
}
static Buf clutSeg() { Buf d{(uint8_t)(r32() % 3), (uint8_t)(r32() % 16 << 4)}; const int n = (int)(r32() % 8); for (int i = 0; i < n; i++) { const bool full = r32() % 2; put(d, {(int)(r32() % 20), (int)ORD_CALL(orOf, (r32() % 8) << 5, (r32() % 2 ? 0x1 : 0) * full)}); if (full) { d[d.size() - 1] |= 1; rnd(d, 4); } else rnd(d, 2); } return d; }
static Buf objectSeg() { Buf top = pixelBlock(), bot = r32() % 3 ? pixelBlock() : Buf(); Buf d{0, (uint8_t)(r32() % 3), (uint8_t)(r32() % 8 ? 0 : (r32() % 4) << 2), (uint8_t)(top.size() >> 8), (uint8_t)top.size(), (uint8_t)(bot.size() >> 8), (uint8_t)bot.size()}; cat(d, top); cat(d, bot); return d; }
static Buf subtitlePes(int page) {
    Buf p; put(p, {0, 0, 1, 0xBD, 0, 0, 0x80, r32() % 4 ? 0x80 : 0x00, 5}); p.push_back((uint8_t)(0x21 | ((r32() % 8) << 1))); rnd(p, 4); p[p.size() - 4] &= 0xFE; p[p.size() - 2] |= 1;
    if (r32() % 5 == 0) { p[8] = (uint8_t)(r32() % 4); p.resize(9 + p[8]); }
    put(p, {r32() % 30 ? 0x20 : (int)r32(), 0});
    const int n = (int)(1 + r32() % 7);
    for (int i = 0; i < n; i++) {
        const int pg = r32() % 6 ? page : (int)(r32() % 4);
        switch (r32() % 7) {
        case 0: cat(p, segment(0x10, pg, pageSeg())); break;
        case 1: cat(p, segment(0x11, pg, regionSeg())); break;
        case 2: cat(p, segment(0x12, pg, clutSeg())); break;
        case 3: case 4: cat(p, segment(0x13, pg, objectSeg())); break;
        case 5: cat(p, segment(0x14, pg, Buf{0, 0, (uint8_t)(r32() % 3 ? 2 : r32()), 0, (uint8_t)(r32() % 3 ? 0xFF : r32())})); break;
        default: cat(p, segment(0x80, pg, Buf())); break;
        }
    }
    if (r32() % 6 == 0) cat(p, segment(0x80, page, Buf()));
    if (r32() % 8 == 0) p.resize(r32() % (p.size() + 1));
    return p;
}

static void testSubs() {
    const long iters = getenv("ONAIR_DIFF_ITERS") ? atol(getenv("ONAIR_DIFF_ITERS")) : 200000;
    { uint32_t a2[4], a4[16], a8[256]; A::dvbSubDefaultClut(a2, a4, a8); dig.bytes(a2, sizeof a2); dig.bytes(a4, sizeof a4); dig.bytes(a8, sizeof a8); }
    for (int y = 0; y < 256; y += 1) for (int cr = 0; cr < 256; cr += 17) for (int cb = 0; cb < 256; cb += 15) { const int t = (y * 7 + cr) & 0xFF; dig.add(A::dvbSubYcrcb(y, cr, cb, t)); }
    for (long i = 0; i < iters; i++) {
        const int yy = (int)(r32() % 256), cr = (int)(r32() % 256), cb = (int)(r32() % 256), tt = (int)(r32() % 300) - 20;
        dig.add(A::dvbSubYcrcb(yy, cr, cb, tt));
        // PES + segments + parsers
        Buf pes = subtitlePes(1);
        exact(pes, [&](const uint8_t* p, size_t n) {
            A::DvbSubPesInfo a;
            const bool ba = A::dvbSubPesInfo(p, n, a);
            dig.add(ba);
            if (ba) dig.add(a.payloadOff, a.hasPts, a.pts);
            if (!ba) return;
            rsStat[8]++;
            const uint8_t* d = p + a.payloadOff; const size_t dn = n - a.payloadOff;
            const auto sa = A::dvbSubSegments(d, dn);
            dig.add((uint64_t)sa.size());
            for (const auto& sg : sa) dig.add(sg.type, sg.page, sg.off, sg.len);
            for (size_t k = 0; k < sa.size(); k++) {
                const uint8_t* s = d + sa[k].off; const int sl = (int)sa[k].len;
                if (sa[k].type == 0x10) { A::DvbSubPageInfo x; const bool u = A::dvbSubParsePage(s, sl, x);
                    dig.add(u);
                    if (u) { dig.add(x.timeout, x.version, x.state, (uint64_t)x.regions.size()); for (const auto& rg : x.regions) dig.add(rg.id, rg.x, rg.y); }
                    rsStat[9]++; }
                if (sa[k].type == 0x11) { A::DvbSubRegionInfo x; const bool u = A::dvbSubParseRegion(s, sl, x);
                    dig.add(u);
                    if (u) { dig.add(x.id, x.version, x.fill, x.w, x.h, x.depth, x.clut, x.pix8, x.pix4, x.pix2, (uint64_t)x.objs.size()); for (const auto& o : x.objs) dig.add(o.id, o.type, o.x, o.y, o.fg, o.bg); }
                    rsStat[10]++; }
                if (sa[k].type == 0x12) { int ia = -1; std::vector<A::DvbSubClutEntry> x; const bool u = A::dvbSubParseClut(s, sl, ia, x);
                    dig.add(u);
                    if (u) { dig.add(ia, (uint64_t)x.size()); for (const auto& c : x) dig.add(c.id, c.flags, c.rgba); }
                    rsStat[11]++; }
                if (sa[k].type == 0x13) { A::DvbSubObjectInfo x; const bool u = A::dvbSubParseObject(s, sl, x);
                    dig.add(u); if (u) { dig.add(x.id, x.topOff, x.topLen, x.botOff, x.botLen); rsStat[12]++; } }
                if (sa[k].type == 0x14) { int w1 = -1, h1 = -1; const bool u = A::dvbSubParseDisplay(s, sl, w1, h1); dig.add(u); if (u) dig.add(w1, h1); }
            }
        });
        // the pixel decoder, region conversion and blending
        const Buf f = pixelBlock();
        static const int depths[3] = {2, 4, 8};
        const int depth = depths[r32() % 3], w = (int)(1 + r32() % 30), h = (int)(1 + r32() % 12);
        std::vector<uint8_t> pa((size_t)w * h);
        for (auto& v : pa) v = (uint8_t)(r32() % 3 ? 0 : r32());
        const int x0 = (int)(r32() % 40) - 8, y0 = (int)(r32() % 16) - 4;
        const bool dbl = r32() % 2;
        exact(f, [&](const uint8_t* p, size_t n) { A::dvbSubDecodeField(p, n, x0, y0, 2, dbl, w, h, depth, pa.data()); });
        dig.add(pa);
        uint32_t c2[4], c4[16], c8[256];
        for (auto& v : c2) v = r32(); for (auto& v : c4) v = r32(); for (auto& v : c8) v = r32();
        std::vector<uint8_t> ra(pa.size() * 4);
        A::dvbSubRegionRgba(pa.data(), pa.size(), depth, c2, c4, c8, ra.data());
        dig.add(ra);
        A::DvbSubDisplay da;
        da.dispW = (int)(8 + r32() % 40); da.dispH = (int)(4 + r32() % 20);
        const int nreg = (int)(r32() % 4);
        for (int q = 0; q < nreg; q++) {
            A::DvbSubRegionImage ia; ia.x = (int)(r32() % 50) - 10; ia.y = (int)(r32() % 25) - 5; ia.w = w; ia.h = h;
            ia.rgba.resize((size_t)w * h * 4); for (auto& v : ia.rgba) v = (uint8_t)(r32() % 3 ? 255 : r32() % 2 ? 0 : r32());
            for (size_t k = 3; k < ia.rgba.size(); k += 4) if (r32() % 4 == 0) ia.rgba[k] = (uint8_t)r32();
            da.regions.push_back(ia);
        }
        std::vector<uint8_t> oa;
        A::dvbSubComposite(da, oa);
        dig.add(oa);
        const double pa1 = (double)(r32() % 100000000) / 1000.0, pb1 = (double)(r32() % 100000000) / 1000.0;
        dig.add(A::ptsDelta(pa1, pb1));
    }
    // whole decoders on the same PES stream
    for (long i = 0; i < iters / 40; i++) {
        A::DvbSubDecoder da;
        da.setStream(1, 1, 2);
        const int np = (int)(1 + r32() % 8);
        for (int k = 0; k < np; k++) { const Buf pes = subtitlePes(r32() % 4 ? 1 : 2); da.feedPes(pes.data(), pes.size()); }
        dig.add((uint64_t)da.displays());
        for (double clk : {-1.0, 0.0, 1.0, 1000.0, 90000.0, 20000.5}) {
            const auto x = da.displayAt(clk);
            dig.add(x != nullptr);
            if (x) {
                dig.add(x->seq, x->pts, x->timeout, x->dispW, x->dispH, (uint64_t)x->regions.size());
                for (const auto& rg : x->regions) dig.add(rg.x, rg.y, rg.w, rg.h, rg.rgba);
                if (x->regions.size()) rsStat[13]++;
            }
        }
    }
}

int main() {
    testText();
    testTeletext();
    testSubs();
    printf("dvbsi proto: non-empty text %ld %ld, 2418 ok %ld, units %ld %ld %ld, pes %ld, decoder pages %ld, sub pes %ld page %ld region %ld clut %ld object %ld, decoder displays with regions %ld\n",
           rsStat[0], rsStat[1], rsStat[2], rsStat[3], rsStat[4], rsStat[5], rsStat[6], rsStat[7], rsStat[8], rsStat[9], rsStat[10], rsStat[11], rsStat[12], rsStat[13]);
    const bool def = !getenv("ONAIR_DIFF_ITERS");
    int rc = golden::check("dvbsi_proto_rs (without iconv text)", dig.portable, 0xb59f514d9ea18de6ull, def);
#ifdef __APPLE__
    rc |= golden::check("dvbsi_proto_rs", dig.all, 0xc1f39019e0da1992ull, def);
#endif
    return rc;
}
