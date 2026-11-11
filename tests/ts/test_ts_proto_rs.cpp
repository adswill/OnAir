// Golden-vector test of the transport-stream parsing layer (dect2::tsp, the Rust crate onair_ts): CRC-32, packet header, PAT packet, and PAT / PMT / SDT / NIT / EIT /
// TVCT / TDT sections built from random descriptors, mutated, truncated, with corrupt length fields, over ONAIR_DIFF_ITERS (default 200000) seeded inputs per
// family. Buffers are exactly as long as the section so a sanitizer build sees any read past the end. Every result goes into a digest (tests/common/golden.h)
// compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/tv/ts_proto.h"
#include "../common/golden.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::tsp;

static golden::Digest dig;
static long okCount[8];

static std::mt19937 g(24681);
static uint32_t r32() { return g(); }
using Buf = std::vector<uint8_t>;

template <class T> static void addSpan(const T& a) { dig.add(a.off, a.len); }

// ---- generators ----------------------------------------------------------------------------------------------------------------------------------------
static void put(Buf& b, std::initializer_list<int> v) { for (int x : v) b.push_back((uint8_t)x); }
static void rnd(Buf& b, size_t n) { for (size_t i = 0; i < n; i++) b.push_back((uint8_t)r32()); }
static void crcPad(Buf& b) { rnd(b, 4); }
static Buf descriptor(int tag, const Buf& body) { Buf d{(uint8_t)tag, (uint8_t)body.size()}; d.insert(d.end(), body.begin(), body.end()); return d; }
static Buf randText(size_t n) { Buf t; for (size_t i = 0; i < n; i++) t.push_back((uint8_t)(32 + r32() % 90)); return t; }
static void cat(Buf& a, const Buf& b) { a.insert(a.end(), b.begin(), b.end()); }

static Buf genPat() {
    Buf s; put(s, {0x00, 0xB0, 0, (int)(r32() & 0xFF), (int)(r32() & 0xFF), 0xC1, 0, 0});
    const int n = (int)(r32() % 12);
    for (int i = 0; i < n; i++) { put(s, {(int)(r32() % 4 ? r32() & 0xFF : 0), (int)(r32() & 0xFF), 0xE0 | (int)(r32() & 0x1F), (int)(r32() & 0xFF)}); }
    crcPad(s); return s;
}
static Buf genSubtitlingDesc() { Buf b; const int n = (int)(r32() % 4); for (int i = 0; i < n; i++) { put(b, {'e', 'n', 'g'}); rnd(b, 5); } if (r32() % 5 == 0) rnd(b, r32() % 9); return descriptor(0x59, b); }
static Buf genTeletextDesc() {
    Buf b; const int n = (int)(r32() % 4);
    for (int i = 0; i < n; i++) { put(b, {'d', 'e', 'u'}); put(b, {(int)(r32() & 0xFF), r32() % 5 ? (int)((r32() % 10) << 4 | (r32() % 10)) : (int)(r32() & 0xFF)}); }
    return descriptor(0x56, b);
}
static Buf genEsDescs() {
    Buf d; const int n = (int)(r32() % 4);
    for (int i = 0; i < n; i++) {
        switch (r32() % 12) {
        case 0: cat(d, descriptor(0x09, [] { Buf b; rnd(b, 4); return b; }())); break;
        case 1: { Buf b = {'D', 'R', 'A', '1'}; if (r32() % 3) b = {'X', 'Y', 'Z', 'W'}; cat(d, descriptor(0x05, b)); break; }
        case 2: { Buf b; rnd(b, r32() % 6); cat(d, descriptor(0x0A, b)); break; }
        case 3: cat(d, descriptor(0x6A, {})); break;
        case 4: cat(d, descriptor(0x7A, {})); break;
        case 5: cat(d, descriptor(0x7B, {})); break;
        case 6: cat(d, descriptor(0x7C, {})); break;
        case 7: cat(d, genSubtitlingDesc()); break;
        case 8: cat(d, genTeletextDesc()); break;
        case 9: cat(d, descriptor(0x46, {})); break;
        default: { Buf b; rnd(b, r32() % 12); cat(d, descriptor((int)(r32() & 0xFF), b)); }
        }
    }
    return d;
}
static Buf genPmt() {
    Buf s; put(s, {0x02, 0xB0, 0, 0x01, 0x01, 0xC1, 0, 0, 0xE1, 0x00});
    Buf pi; if (r32() % 3 == 0) cat(pi, descriptor(0x09, {1, 2, 3, 4})); if (r32() % 5 == 0) { Buf b; rnd(b, r32() % 8); cat(pi, descriptor((int)(r32() & 0xFF), b)); }
    put(s, {0xF0 | (int)(pi.size() >> 8), (int)(pi.size() & 0xFF)}); cat(s, pi);
    static const int types[] = {0x01, 0x02, 0x03, 0x04, 0x06, 0x0F, 0x11, 0x1B, 0x24, 0x42, 0x81, 0x87, 0xD2, 0xD4, 0x05};
    const int n = (int)(r32() % 6);
    for (int i = 0; i < n; i++) {
        Buf es = genEsDescs();
        put(s, {r32() % 8 ? types[r32() % 15] : (int)(r32() & 0xFF), 0xE0 | (int)(r32() & 0x1F), (int)(r32() & 0xFF), 0xF0 | (int)(es.size() >> 8), (int)(es.size() & 0xFF)});
        cat(s, es);
    }
    crcPad(s); return s;
}
static Buf genSdt() {
    Buf s; put(s, {0x42, 0xF0, 0, 0x12, 0x34, 0xC1, 0, 0, 0x20, 0x01, 0xFF});
    const int n = (int)(r32() % 5);
    for (int i = 0; i < n; i++) {
        Buf d; const int k = (int)(r32() % 3);
        for (int j = 0; j < k; j++) {
            if (r32() % 6 == 0) { Buf b; rnd(b, r32() % 6); cat(d, descriptor((int)(r32() & 0xFF), b)); continue; }
            Buf b; b.push_back((uint8_t)(r32() & 0x1F)); Buf p = randText(r32() % 10), nm = randText(r32() % 14);
            b.push_back((uint8_t)p.size()); cat(b, p); b.push_back((uint8_t)nm.size()); cat(b, nm);
            if (r32() % 5 == 0) b.resize(r32() % (b.size() + 1));
            cat(d, descriptor(0x48, b));
        }
        put(s, {(int)(r32() & 0xFF), (int)(r32() & 0xFF), 0xFC, ((int)(r32() & 0xF) << 4) | (int)(d.size() >> 8), (int)(d.size() & 0xFF)});
        cat(s, d);
    }
    crcPad(s); return s;
}
static Buf genNit() {
    Buf s; put(s, {0x40, 0xF0, 0, 0x12, 0x34, 0xC1, 0, 0});
    Buf nd; const int k = (int)(r32() % 3);
    for (int i = 0; i < k; i++) { if (r32() % 4) cat(nd, descriptor(0x40, randText(r32() % 16))); else { Buf b; rnd(b, r32() % 6); cat(nd, descriptor((int)(r32() & 0xFF), b)); } }
    put(s, {0xF0 | (int)(nd.size() >> 8), (int)(nd.size() & 0xFF)}); cat(s, nd);
    Buf tl; const int nt = (int)(r32() % 4);
    for (int i = 0; i < nt; i++) {
        Buf d; if (r32() % 3) { Buf b; const int m = (int)(r32() % 6); for (int j = 0; j < m; j++) rnd(b, 4); if (r32() % 6 == 0) rnd(b, r32() % 4); cat(d, descriptor(0x83, b)); }
        if (r32() % 4 == 0) { Buf b; rnd(b, r32() % 9); cat(d, descriptor((int)(r32() & 0xFF), b)); }
        put(tl, {0x12, 0x34, 0x20, 0x01, 0xF0 | (int)(d.size() >> 8), (int)(d.size() & 0xFF)}); cat(tl, d);
    }
    put(s, {0xF0 | (int)(tl.size() >> 8), (int)(tl.size() & 0xFF)}); cat(s, tl);
    crcPad(s); return s;
}
static Buf genEit() {
    Buf s; put(s, {r32() % 2 ? 0x4E : 0x50 + (int)(r32() % 16), 0xF0, 0, 0x01, (int)(r32() & 0xFF), 0xC1, (int)(r32() % 3), 0, 0x20, 0x01, 0x00, 0x01, 0x4E, 0x4E});
    const int n = (int)(r32() % 4);
    for (int i = 0; i < n; i++) {
        Buf d; const int k = (int)(r32() % 5);
        for (int j = 0; j < k; j++) {
            switch (r32() % 5) {
            case 0: case 1: { Buf b = {'e', 'n', 'g'}; Buf t = randText(r32() % 20), x = randText(r32() % 30); b.push_back((uint8_t)t.size()); cat(b, t); b.push_back((uint8_t)x.size()); cat(b, x);
                if (r32() % 6 == 0) b.resize(r32() % (b.size() + 1)); cat(d, descriptor(0x4D, b)); break; }
            case 2: case 3: { Buf b; Buf items; if (r32() % 4 == 0) rnd(items, r32() % 5); Buf t = randText(r32() % 40);
                b.push_back((uint8_t)(((r32() % 16) << 4) | (r32() % 16))); put(b, {'e', 'n', 'g'}); b.push_back((uint8_t)items.size()); cat(b, items); b.push_back((uint8_t)t.size()); cat(b, t);
                if (r32() % 6 == 0) b.resize(r32() % (b.size() + 1)); cat(d, descriptor(0x4E, b)); break; }
            default: { Buf b; rnd(b, r32() % 5); cat(d, descriptor(0x54, b)); }
            }
        }
        const int mjd = r32() % 20 ? 50000 + (int)(r32() % 20000) : (int)(r32() & 0xFFFF);
        put(s, {(int)(r32() & 0xFF), (int)(r32() & 0xFF), mjd >> 8, mjd & 0xFF});
        if (r32() % 10 == 0) put(s, {0xFF, 0xFF, 0xFF}); else put(s, {(int)((r32() % 3) << 4 | (r32() % 10)), (int)((r32() % 6) << 4 | (r32() % 10)), (int)((r32() % 6) << 4 | (r32() % 10))});
        if (r32() % 8 == 0) rnd(s, 3); else put(s, {0x00, (int)((r32() % 3) << 4 | (r32() % 10)), (int)((r32() % 6) << 4 | (r32() % 10))});
        put(s, {((int)(r32() % 8) << 5) | (int)(d.size() >> 8), (int)(d.size() & 0xFF)}); cat(s, d);
    }
    crcPad(s); return s;
}
static Buf genTvct() {
    Buf s; put(s, {0xC8, 0xF0, 0, 0x12, 0x34, 0xC1, 0, 0, 0, 0});
    const int n = (int)(r32() % 5); s.push_back((uint8_t)(r32() % 8 ? n : r32() & 0xFF));
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 7; k++) { if (r32() % 5 == 0) put(s, {0, 0}); else if (r32() % 8 == 0) put(s, {(int)(r32() & 0xFF), (int)(r32() & 0xFF)}); else put(s, {0, 0x20 + (int)(r32() % 95)}); }
        Buf rest; rnd(rest, 32 - 14);
        s.insert(s.end(), rest.begin(), rest.end());
        s[s.size() - 32 + 24] = (uint8_t)(r32() % 10 ? 0x00 : 0xFF); s[s.size() - 32 + 25] = (uint8_t)(r32() % 6 ? r32() & 0xFF : 0xFF);
        s[s.size() - 32 + 30] &= 0xFC; s[s.size() - 32 + 31] = (uint8_t)(r32() % 4 ? 0 : r32() % 10);
        s.resize(s.size() + (s[s.size() - 1] % 10 == 0 ? 0 : 0));
    }
    crcPad(s); return s;
}
static Buf mutate(Buf s) {
    const int k = (int)(r32() % 4);
    for (int i = 0; i < k; i++) {
        switch (r32() % 6) {
        case 0: if (!s.empty()) s[r32() % s.size()] = (uint8_t)r32(); break;
        case 1: if (!s.empty()) s[r32() % s.size()] ^= (uint8_t)(1u << (r32() % 8)); break;
        case 2: s.resize(r32() % (s.size() + 1)); break;
        case 3: rnd(s, r32() % 12); break;
        case 4: if (s.size() > 4) s.erase(s.begin() + r32() % (s.size() - 1)); break;
        default: if (s.size() > 20) s[10 + r32() % 10] = (uint8_t)(r32() % 3 ? 0xFF : r32()); break;
        }
    }
    return s;
}
static Buf pick(Buf (*gen)()) { Buf s = gen(); const uint32_t r = r32() % 10; if (r < 3) return s; if (r < 9) return mutate(s); Buf t; rnd(t, r32() % 300); return t; }

// ---- comparison ----------------------------------------------------------------------------------------------------------------------------------------
template <class F> static void withExact(const Buf& s, F f) {   // an exactly sized heap copy
    uint8_t* p = s.empty() ? nullptr : (uint8_t*)malloc(s.size());
    if (p) memcpy(p, s.data(), s.size());
    f(p, (int)s.size());
    free(p);
}

static void testPat(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        int ta = -1; std::vector<A::PatEntry> ea;
        const bool a = A::parsePat(p, n, ta, ea);
        dig.add(a);
        if (a) { dig.add(ta, (uint64_t)ea.size()); okCount[0]++; for (const auto& e : ea) dig.add(e.prog, e.pid); }
    });
}
static void testPmt(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        A::Pmt a;
        const bool ba = A::parsePmt(p, n, a);
        dig.add(ba);
        if (!ba) return;
        okCount[1]++;
        dig.add(a.prog, a.pcrPid, a.ca, (uint64_t)a.streams.size());
        for (const auto& x : a.streams) {
            dig.add(x.pid, x.streamType, x.kind, x.codec, x.lang, (uint64_t)x.subs.size());
            for (const auto& u : x.subs) dig.add(u.teletext, u.lang, u.type, u.page, u.ancPage);
        }
    });
}
static void testSdt(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        int ta = -1, oa = -1; std::vector<A::SdtService> ea;
        const bool a = A::parseSdt(p, n, ta, oa, ea);
        dig.add(a);
        if (!a) return;
        okCount[2]++;
        dig.add(ta, oa, (uint64_t)ea.size());
        for (const auto& e : ea) { dig.add(e.sid, e.freeCa, e.hasType, e.hasProvider, e.hasName, e.type); addSpan(e.provider); addSpan(e.name); }
    });
}
static void testNit(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        A::Nit a;
        const bool ba = A::parseNit(p, n, a);
        dig.add(ba);
        if (!ba) return;
        okCount[3]++;
        dig.add(a.hasName); addSpan(a.name); dig.add((uint64_t)a.lcn.size());
        for (const auto& l : a.lcn) dig.add(l.sid, l.lcn);
    });
}
static void testEit(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        A::Eit a;
        const bool ba = A::parseEit(p, n, a);
        dig.add(ba);
        if (!ba) return;
        okCount[4]++;
        dig.add(a.sid, a.secNum, a.legacy, a.hasName); addSpan(a.nowNextName); dig.add((uint64_t)a.events.size());
        for (const auto& x : a.events) {
            dig.add(x.eventId, x.start, x.duration, x.running, x.genre, x.lang, x.hasTitle, x.hasText); addSpan(x.title); addSpan(x.text); dig.add(x.extMask);
            for (int k = 0; k < 16; k++) addSpan(x.ext[k]);
        }
    });
}
static void testTvct(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        int ta = -1; std::vector<A::Tvct> ea;
        const bool a = A::parseTvct(p, n, ta, ea);
        dig.add(a);
        if (!a) return;
        okCount[5]++;
        dig.add(ta, (uint64_t)ea.size());
        for (const auto& e : ea) dig.add(e.name, e.major, e.minor, e.prog, e.stype);
    });
}
static void testTdt(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        A::Tdt a;
        const bool ba = A::parseTdt(p, n, a);
        dig.add(ba);
        if (ba) { dig.add(a.unixSec, a.text); okCount[6]++; }
    });
}
static void testCrcPacket(const Buf& s) {
    withExact(s, [&](const uint8_t* p, int n) {
        dig.add(A::crc32(p, n));
        A::PacketHeader a;
        const bool ba = A::packetHeader(p, n, a);
        dig.add(ba);
        if (ba) { dig.add(a.pid, a.tei, a.pusi, a.scr, a.afc, a.cc, a.payloadOff); okCount[7]++; }
    });
}

int main() {
    const long iters = getenv("ONAIR_DIFF_ITERS") ? atol(getenv("ONAIR_DIFF_ITERS")) : 200000;
    // known vectors first: the CRC check value and a PAT packet
    if (A::crc32((const uint8_t*)"123456789", 9) != 0x0376E6E7u) { printf("crc check value\n"); return 1; }
    { uint8_t a[188]; for (int i = 0; i < 1000; i++) { const int ts = (int)(r32() & 0xFFFF), sid = (int)(r32() & 0xFFFF), pp = (int)(r32() & 0x1FFF), cc = (int)(r32() & 0xFFFF);
        A::patPacket(ts, sid, pp, cc, a); dig.bytes(a, 188); } }
    // negative lengths and null pointers
    { int t; std::vector<A::PatEntry> v; uint8_t b[4] = {0}; A::PacketHeader h;
      if (A::parsePat(nullptr, -5, t, v) || A::parsePat(b, -1, t, v) || A::packetHeader(nullptr, 188, h)) { printf("null / negative lengths must be rejected\n"); return 1; }
      dig.add(A::crc32(nullptr, -3));
      if (A::crc32(nullptr, 0) != 0xFFFFFFFFu) { printf("crc of nothing\n"); return 1; } }
    for (long i = 0; i < iters; i++) {
        testPat(pick(genPat)); testPmt(pick(genPmt)); testSdt(pick(genSdt)); testNit(pick(genNit)); testEit(pick(genEit)); testTvct(pick(genTvct));
        { Buf d; rnd(d, 8 + r32() % 6); d[3] = (uint8_t)(r32() % 3 ? 0x00 + r32() % 0xFF : r32()); testTdt(d); if (r32() % 4 == 0) { d.resize(r32() % 9); testTdt(d); } }
        { Buf d; rnd(d, r32() % 200); if (d.size() > 5 && r32() % 2) d[0] = 0x47; testCrcPacket(d); }
    }
    printf("ts proto: parsed ok: pat %ld pmt %ld sdt %ld nit %ld eit %ld tvct %ld tdt %ld packets %ld\n", okCount[0], okCount[1], okCount[2], okCount[3], okCount[4], okCount[5], okCount[6], okCount[7]);
    // recorded again for 3.0: stream type 0x14 is DRA audio (dtmb-dra-030, rust/onair_ts/src/si.rs)
    return golden::check("ts_proto_rs", dig, 0x40910f7b8b055a5full, !getenv("ONAIR_DIFF_ITERS"));
}
