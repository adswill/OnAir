// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the DRM signalling and data layers (dect2::drmp, the Rust crate onair_drm): the CRCs, FAC blocks (both robustness mode families, good and
// corrupted), SDC data fields (every entity type, parsed twice into the same record so the in-place refinement is covered), SDC block checks, the audio super frame
// split (consistent and corrupt borders), the text message segment check, the tables, MJD conversion and the data service helpers, over ONAIR_DIFF_ITERS (default
// 200000) seeded inputs per family. Buffers are exactly as long as the input (sanitizer builds see overruns). Every result goes into a digest (tests/common/golden.h)
// compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/drm/drm_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::drmp;
namespace D = dect2::drm;

static golden::Digest dig;
static long okc[16];
static std::mt19937 g(31415);
static uint32_t r32() { return g(); }
using Buf = std::vector<uint8_t>;
static void rnd(Buf& b, size_t n) { for (size_t i = 0; i < n; i++) b.push_back((uint8_t)r32()); }
static void putBits(Buf& b, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }
template <class F> static void exact(const Buf& s, F f) {
    uint8_t* p = s.empty() ? nullptr : (uint8_t*)malloc(s.size());
    if (p) memcpy(p, s.data(), s.size());
    f(p, s.size());
    free(p);
}
static Buf flipBits(Buf b, int maxFlips) { const int k = (int)(r32() % (maxFlips + 1)); for (int i = 0; i < k && !b.empty(); i++) b[r32() % b.size()] ^= 1; return b; }

// ---- FAC -------------------------------------------------------------------------------------------------------------------------------------------------
static Buf genFac(bool modeE, bool goodCrc) {
    Buf b;
    putBits(b, r32() % 2, 1); putBits(b, r32() % 4, 2); putBits(b, modeE ? (r32() % 8 ? 1 : 0) : (r32() % 8 ? 0 : 1), 1);
    putBits(b, r32() % 8, 3); putBits(b, r32() % 2, 1); putBits(b, r32() % 4, 2); putBits(b, r32() % 2, 1); putBits(b, r32() % 16, 4); putBits(b, r32() % 8, 3); putBits(b, r32() % 2, 1); putBits(b, 0, 1);
    for (int i = 0; i < (modeE ? 2 : 1); i++) { putBits(b, r32() & 0xFFFFFF, 24); putBits(b, r32() % 4, 2); putBits(b, r32() % 2, 1); putBits(b, r32() % 16, 4); putBits(b, r32() % 2, 1); putBits(b, r32() % 32, 5); putBits(b, r32() % 2, 1); putBits(b, 0, 6); }
    Buf crcIn = b; if (modeE) crcIn.resize(crcIn.size() + 4, 0);
    uint32_t c = A::crc8(crcIn.data(), crcIn.size()); if (!goodCrc) c ^= 1 + r32() % 255;
    putBits(b, c, 8);
    return b;
}
static void testFac() {
    for (int mode = 0; mode < 2; mode++) {
        const bool me = mode == 1;
        for (int rm = 0; rm < 2; rm++) {   // rm: the flag in the data says mode E, the parameter may disagree
            Buf b = genFac(me, r32() % 5 != 0);
            if (r32() % 4 == 0) b = flipBits(b, 3);
            const bool askE = rm ? !me : me;
            const size_t need = askE ? 116 : 72;
            if (b.size() < need) b.resize(need, 0); else if (b.size() > need) b.resize(need);
            exact(b, [&](const uint8_t* p, size_t) {
                D::FacInfo fa; const bool ba = A::facParse(p, askE, fa);
                dig.add(ba);
                if (ba) {
                    dig.add(fa.enhancement, fa.identity, fa.modeE, fa.occupancy, fa.interleaver, fa.mscMode, fa.sdcMode, fa.numServicesCode, fa.reconfig, fa.toggle, fa.nSvc);
                    for (int i = 0; i < fa.nSvc; i++) dig.add(fa.svc[i].id, fa.svc[i].shortId, fa.svc[i].audioCa, fa.svc[i].dataCa, fa.svc[i].language, fa.svc[i].data, fa.svc[i].descriptor);
                }
                okc[0] += ba;
            });
        }
    }
}

// ---- SDC -------------------------------------------------------------------------------------------------------------------------------------------------
static void entity(Buf& f, int len, int ver, int type, int nib, bool randBody = true) { f.push_back((uint8_t)(len << 1 | ver)); f.push_back((uint8_t)(type << 4 | nib)); if (randBody) rnd(f, len); }
static Buf genSdc() {
    Buf f; const int n = (int)(r32() % 7);
    for (int i = 0; i < n; i++) {
        switch (r32() % 10) {
        case 0: ORD_CALL(entity, std::ref(f), 3 * (1 + r32() % 4), r32() % 3 == 0, 0, (int)(r32() % 16)); break;
        case 1: { const int len = (int)(r32() % 30); ORD_CALL(entity, std::ref(f), len, r32() % 5 == 0, 1, (int)(r32() % 16), false); if (len) f.push_back((uint8_t)(r32() % 4 ? r32() % 18 : 0x41 + r32() % 20)); for (int k = 1; k < len; k++) f.push_back((uint8_t)(0x20 + r32() % 90)); break; }
        case 2: { const int len = (int)(1 + r32() % 20); ORD_CALL(entity, std::ref(f), len, r32() % 5 == 0, 5, (int)(r32() % 16), false); f.push_back((uint8_t)(r32() & 0xFF)); for (int k = 1; k < len; k++) f.push_back((uint8_t)r32()); break; }
        case 3: ORD_CALL(entity, std::ref(f), 3 + r32() % 3, 0, 8, (int)(r32() % 16)); break;
        case 4: ORD_CALL(entity, std::ref(f), 2 + r32() % 6, r32() % 5 == 0, 9, (int)(r32() % 16)); break;
        case 5: ORD_CALL(entity, std::ref(f), 5 + (r32() % 3 == 0 ? r32() % 3 : 0), r32() % 5 == 0, 12, (int)(r32() % 16)); break;
        default: ORD_CALL(entity, std::ref(f), (int)(r32() % 10), r32() % 3 == 0, (int)(r32() % 16), (int)(r32() % 16)); break;
        }
    }
    if (r32() % 3 == 0) f.resize(f.size() + r32() % 6, 0);
    return f;
}
static void fillSdc(D::SdcInfo& s) {   // arbitrary previous state, so the in-place refinement matters
    if (r32() % 2) return;
    s.mux.present = true; s.mux.protA = 1; s.mux.nStreams = 1; s.mux.stream[0].lenA = 7;
    for (int i = 0; i < 4; i++) {
        if (r32() % 2) { s.label[i].present = true; s.label[i].shortId = i; s.label[i].text = "old"; }
        if (r32() % 2) { s.app[i].present = true; s.app[i].shortId = i; s.app[i].packet = true; s.app[i].dataUnit = true; s.app[i].packetId = 2; s.app[i].packetLen = 55; s.app[i].appData = {1, 2, 3}; }
        if (r32() % 2) { s.audio[i].present = true; s.audio[i].config = {9, 9}; s.audio[i].coding = 3; }
        if (r32() % 2) { s.lang[i].present = true; s.lang[i].language = "abc"; s.lang[i].country = "de"; }
    }
    if (r32() % 2) { s.time.present = true; s.time.hasOffset = true; s.time.offsetHalfHours = -7; s.time.mjd = 1; }
    s.otherTypes = {3, 4};
}
static void addSdc(const D::SdcInfo& a) {
    auto mux = [](const D::SdcMux& x) { dig.add(x.present, x.protA, x.protB, x.nStreams); for (int i = 0; i < 4; i++) dig.add(x.stream[i].lenA, x.stream[i].lenB); };
    dig.add(a.afsIndex, a.entities, a.otherTypes, a.time.present, a.time.mjd, a.time.hour, a.time.minute, a.time.hasOffset, a.time.offsetHalfHours);
    mux(a.mux); mux(a.muxNext);
    for (int i = 0; i < 4; i++) {
        const auto &x = a.audio[i]; const auto &l = a.label[i]; const auto &p = a.lang[i]; const auto &s = a.app[i];
        dig.add(x.present, x.shortId, x.streamId, x.coding, x.sbr, x.mode, x.rateCode, x.text, x.enhancement, x.mps, x.config);
        dig.add(l.present, l.shortId, l.textControl, l.text);
        dig.add(p.present, p.shortId, p.language, p.country);
        dig.add(s.present, s.shortId, s.streamId, s.packet, s.domain, s.dataUnit, s.packetId, s.packetLen, s.appData);
    }
}
static void testSdc() {
    Buf f = genSdc();
    if (r32() % 3 == 0) { const int k = (int)(r32() % 3); for (int i = 0; i < k && !f.empty(); i++) f[r32() % f.size()] = (uint8_t)r32(); }
    if (r32() % 5 == 0) f.resize(r32() % (f.size() + 1));
    exact(f, [&](const uint8_t* p, size_t n) {
        D::SdcInfo sa; fillSdc(sa);
        const int ca = A::sdcParse(p, (int)n, sa);
        dig.add(ca); addSdc(sa);
        const int c2 = A::sdcParse(p, (int)n, sa);   // a second field refines the first
        dig.add(c2); addSdc(sa);
        okc[1] += ca > 0; okc[2] += !sa.otherTypes.empty();
    });
    // CRC block
    const int fb = (int)(r32() % 20);
    Buf blk; putBits(blk, r32() % 16, 4); for (int i = 0; i < fb; i++) putBits(blk, r32() & 0xFF, 8);
    Buf crcIn(4, 0); crcIn.insert(crcIn.end(), blk.begin(), blk.end());
    uint32_t c = A::crc16(crcIn.data(), crcIn.size()); if (r32() % 4 == 0) c ^= 1 + r32() % 65535;
    putBits(blk, c, 16);
    if (r32() % 8 == 0) blk = flipBits(blk, 2);
    exact(blk, [&](const uint8_t* p, size_t) { int aa = -1; const bool ba = A::sdcCheckBits(p, fb, aa); dig.add(ba, aa); okc[3] += ba; });
}

// ---- audio super frame -----------------------------------------------------------------------------------------------------------------------------------
static void testAac() {
    for (int rate : {0, 12000, 24000, 48000, 16000}) for (int me = 0; me < 2; me++) dig.add(A::aacNumFrames(me, rate));
    for (int n = -2; n < 20; n++) dig.add(A::aacHeaderBytes(n));
    const int nf = (int)(r32() % 14 < 12 ? (r32() % 2 ? 5 : 10) : (int)(r32() % 20) - 2);
    const int hdr = A::aacHeaderBytes(nf), hp = (int)(r32() % 6), len = (int)(r32() % 3 == 0 ? r32() % 300 : 60 + r32() % 200);
    Buf lf; rnd(lf, (size_t)len);
    if (nf >= 2 && nf <= 16 && len > hdr + nf * (hp + 2) && r32() % 3) {   // consistent borders
        const int payload = len - hdr - nf;
        std::fill(lf.begin(), lf.begin() + hdr, 0);
        int b = 0;
        for (int i = 0; i < nf - 1; i++) { b += hp + (int)(r32() % std::max(1, (payload - b - (nf - i) * hp) / (nf - i) + 1)); const int bit = i * 12; for (int k = 0; k < 12; k++) if (((std::min(b, 4095)) >> (11 - k)) & 1) lf[(size_t)((bit + k) / 8)] |= (uint8_t)(0x80 >> ((bit + k) % 8)); }
    }
    const int lenA = r32() % 3 == 0 ? 0 : r32() % 4 == 0 ? (int)(r32() % 400) - 20 : hdr + nf * (hp + 1);
    exact(lf, [&](const uint8_t* p, size_t) {
        D::AacSuperFrame a; const bool ba = A::aacSuperFrameParse(p, len, lenA, nf, a);
        dig.add(ba, a.numFrames, a.headerOk, a.hpBytes);
        if (ba) dig.add(a.frames, a.crc);
        okc[4 + (ba ? 1 : 0)]++;
    });
}

// ---- tables, dates, data service helpers, text message --------------------------------------------------------------------------------------------------
static Buf genGroup() {
    Buf d; const int ext = (int)(r32() % 3 == 0), crc = (int)(r32() % 4 != 0), seg = (int)(r32() % 3 == 0), ua = (int)(r32() % 3 == 0);
    d.push_back((uint8_t)(ext << 7 | crc << 6 | seg << 5 | ua << 4 | (r32() % 16))); d.push_back((uint8_t)r32());
    if (ext) rnd(d, 2); if (seg) rnd(d, 2); if (ua) { d.push_back((uint8_t)(r32() % 16)); rnd(d, d.back() & 0x0F); } rnd(d, r32() % 30);
    if (crc) { uint16_t c = 0xFFFF; for (uint8_t x : d) { c ^= (uint16_t)(x << 8); for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); } c = (uint16_t)~c; if (r32() % 4 == 0) c ^= (uint16_t)(1 + r32() % 65534); d.push_back(c >> 8); d.push_back(c & 0xFF); }
    return d;
}
static Buf genText() {
    Buf t; const size_t n = r32() % 5 == 0 ? 16000 + r32() % 2000 : r32() % 80;
    for (size_t i = 0; i < n; i++) {
        switch (r32() % 12) {
        case 0: t.push_back((uint8_t)(r32() % 32)); break;
        case 1: t.push_back('\r'); break;
        case 2: t.push_back('\n'); break;
        case 3: t.push_back((uint8_t)(0x80 + r32() % 128)); break;
        case 4: { const uint8_t u[2] = {0xC3, (uint8_t)(0x80 + r32() % 64)}; t.insert(t.end(), u, u + 2); break; }
        case 5: if (r32() % 20 == 0) t.push_back(0); break;
        default: t.push_back((uint8_t)(0x20 + r32() % 95));
        }
    }
    return t;
}
static void testTables() {   // the tables are small: all values, once
    for (int c = -3; c < 40; c++) {
        dig.add(A::languageName(c), A::programmeTypeName(c), A::facAudioServices(c), A::facDataServices(c));
        for (int me = 0; me < 2; me++) dig.add(A::mscModeName(me, c), A::occupancyName(me, c));
    }
    for (int dom = -1; dom < 4; dom++) for (int app = -2; app < 0x460; app++) dig.add(A::appName(dom, app));
}

static void testMisc() {
    for (int i = 0; i < 20; i++) {
        const int mjd = (int)(r32() % 200000) - 10000;
        int y1, m1, d1; A::mjdToDate(mjd, y1, m1, d1);
        dig.add(y1, m1, d1, A::dateToMjd(y1, m1, d1));
    }
    if (A::dateToMjd(2026, 10, 17) != 61330) { printf("mjd value\n"); abort(); }
    Buf bits; rnd(bits, r32() % 300); for (auto& x : bits) x &= 1;
    exact(bits, [&](const uint8_t* p, size_t n) { dig.add(A::crc8(p, n), A::crc16(p, n)); });
    const Buf grp = genGroup();
    exact(grp, [&](const uint8_t* p, size_t n) { size_t oa = 9, la = 9; const bool ba = A::groupPayload(p, n, oa, la); dig.add(ba); if (ba) dig.add((uint64_t)oa, (uint64_t)la); okc[6] += ba; });
    const Buf txt = genText();
    exact(txt, [&](const uint8_t* p, size_t n) {
        dig.add(A::displayText(p, n));
        const bool la = A::looksText(p, n); dig.add(la); okc[7] += la;
    });
    // text message segments
    Buf seg; seg.push_back((uint8_t)(r32() & 0xFF)); const int need = A::textSegmentNeed(seg[0]);
    dig.add(need);
    rnd(seg, (size_t)need - 1);
    { uint16_t c = 0xFFFF; for (int i = 0; i < need - 2; i++) { c ^= (uint16_t)(seg[(size_t)i] << 8); for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); } c = (uint16_t)~c; if (r32() % 4 == 0) c ^= (uint16_t)(1 + r32() % 65534); seg[(size_t)need - 2] = (uint8_t)(c >> 8); seg[(size_t)need - 1] = (uint8_t)c; }
    exact(seg, [&](const uint8_t* p, size_t n) { const auto a = A::textSegment(p, n, need);
        dig.add(a.crcOk); if (a.crcOk) dig.add(a.toggle, a.first, a.last, a.command, a.cmd, a.segnum, a.n); okc[8] += a.crcOk; });
}

int main() {
    const long iters = getenv("ONAIR_DIFF_ITERS") ? atol(getenv("ONAIR_DIFF_ITERS")) : 200000;
    testTables();
    for (long i = 0; i < iters; i++) { testFac(); testSdc(); testAac(); testMisc(); }
    printf("drm proto: ok: fac %ld sdc %ld sdc-other %ld sdc-crc %ld aac bad %ld good %ld group %ld text %ld segment %ld\n", okc[0], okc[1], okc[2], okc[3], okc[4], okc[5], okc[6], okc[7], okc[8]);
    return golden::check("drm_proto_rs", dig, 0x9214822683311cdfull, !getenv("ONAIR_DIFF_ITERS"));
}
