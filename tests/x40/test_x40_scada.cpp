// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SCADA radio telemetry (x40 variant Scada, docs/SCADA-4.1.md). Simulated signals only; the frame encoders and the modulator are in
// x40_gen_scada.h and exist only for these tests.
//  1. the CRCs against the catalogue check values; the frame finder; the tracker (Modbus and DNP3 meaning, request / response pairing,
//     devices and register map, DNP3 transport, unsolicited, confirm, time sync, Secure Authentication shown raw, CRC failures, export);
//  2. the whole chain for every physical layer (Bell 202 and V.23 audio FSK at 1200 baud, 2-FSK and 4-FSK at 4800, 9600, 19200 bit/s) and
//     framing (8N1, 8E1, 8O1, HDLC, HDLC NRZI, inverted polarity), with the link type, baud rate and deviation it reports;
//  3. impairments: frequency offset and drift, deviation errors, level, bit clock error, other sample rates, continuous carrier, tone twist;
//  4. AWGN down to the threshold (C/N0 sweep, printed), noise alone, unknown framings, CRC failures and pairing through the air.
// ONAIR_TEST_LONG=1 runs the sweep in 1 dB steps.
#include "x40_gen.h"
#include "x40_gen_scada.h"
#include "dect2/x40/scada_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace scadagen;
namespace sc = dect2::scada;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool hasText(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// ------------------------------------------------------------------ 1. protocol

static sc::Frame fr(double t, const Bytes& b, int proto, bool ok = true) { sc::Frame f; f.timeSec = t; f.bytes = b; f.proto = proto; f.crcOk = ok; f.link = "test"; return f; }

static void crcChecks() {
    const Bytes chk{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(sc::crcModbus(chk.data(), 9) == 0x4B37, "CRC-16/MODBUS check value %04X", sc::crcModbus(chk.data(), 9));
    CHECK(sc::crcDnp(chk.data(), 9) == 0xEA82, "CRC-16/DNP check value %04X", sc::crcDnp(chk.data(), 9));
    CHECK(sc::crcHdlc(chk.data(), 9) == 0x906E, "CRC-16/X.25 check value %04X", sc::crcHdlc(chk.data(), 9));
    CHECK(crcMb(chk) == 0x4B37 && crcDnp(chk.data(), 9) == 0xEA82 && crcX25(chk) == 0x906E, "the test encoders' own CRCs");
    const Bytes rq = mbReadReq(1, 3, 0, 10);   // the classic example: 01 03 00 00 00 0A C5 CD
    CHECK(rq.size() == 8 && rq[6] == 0xC5 && rq[7] == 0xCD, "Modbus example CRC %02X %02X", rq[6], rq[7]);
}

static void finderChecks() {
    const Bytes a = mbReadReq(17, 3, 107, 3), b = mbRegResp(17, 3, {0x022B, 0, 0x64});
    const Bytes d = dnpLink(0xC9, 10, 1, {});
    Bytes burst{0x13, 0x77};
    burst = cat(burst, a); burst = cat(burst, d); burst.push_back(0x55); burst = cat(burst, b); burst.push_back(0xFF);
    auto fs = sc::findFrames(burst.data(), burst.size());
    CHECK(fs.size() == 3, "finder: %zu frames in a burst with garbage around", fs.size());
    if (fs.size() == 3) {
        CHECK(fs[0].proto == sc::Modbus && fs[0].start == 2 && fs[0].len == 8, "finder: Modbus request");
        CHECK(fs[1].proto == sc::Dnp3 && fs[1].len == 10, "finder: DNP3 link frame");
        CHECK(fs[2].proto == sc::Modbus && fs[2].len == b.size() && fs[2].crcOk, "finder: Modbus response");
    }
    Bytes bad = mbReadReq(5, 4, 0, 2); bad[7] ^= 0x40;
    fs = sc::findFrames(bad.data(), bad.size());
    CHECK(fs.size() == 1 && !fs[0].crcOk && fs[0].proto == sc::Modbus, "finder: a damaged Modbus frame of the exact length is reported with a failed CRC");
    Bytes dnpBad = dnpLink(0x44, 10, 1, Bytes(20, 0x11), 1);
    fs = sc::findFrames(dnpBad.data(), dnpBad.size());
    CHECK(fs.size() == 1 && fs[0].proto == sc::Dnp3 && !fs[0].crcOk && fs[0].badBlock == 1, "finder: DNP3 data block 1 CRC failure");
    // a function this reader does not know counts only as a whole burst
    const Bytes odd = mb(9, 0x41, {1, 2, 3});
    fs = sc::findFrames(odd.data(), odd.size());
    CHECK(fs.size() == 1 && fs[0].crcOk && fs[0].len == odd.size(), "finder: unknown function as a whole burst");
    Bytes oddIn{0x00, 0x00}; oddIn = cat(oddIn, odd);
    fs = sc::findFrames(oddIn.data(), oddIn.size());
    CHECK(fs.empty(), "finder: an unknown function inside a longer burst is not taken");
    // random bytes: no frame
    std::mt19937 r(5); Bytes rnd(3000); for (auto& v : rnd) v = (uint8_t)r();
    fs = sc::findFrames(rnd.data(), rnd.size());
    CHECK(fs.size() <= 1, "finder: %zu frames in 3000 random bytes", fs.size());
}

static const sc::Message* lastOf(const sc::Tracker& t) { return t.messages().empty() ? nullptr : &t.messages().back(); }
static const sc::Device* dev(const std::vector<sc::Device>& v, int proto, int addr) { for (auto& d : v) if (d.proto == proto && d.addr == addr) return &d; return nullptr; }
static std::string point(const sc::Device* d, int table, int idx) { if (!d) return "?"; auto it = d->points.find({table, idx}); return it == d->points.end() ? "-" : it->second.value; }

static void modbusTracker() {
    sc::Tracker t;
    t.add(fr(1.00, mbReadReq(17, 3, 107, 3), sc::Modbus));
    t.add(fr(1.08, mbRegResp(17, 3, {555, 0, 100}), sc::Modbus));
    const sc::Message* q = &t.messages()[0];
    const sc::Message* r = lastOf(t);
    CHECK(q->dir == 1 && q->from == "master" && q->to == "slave 17" && q->func == "read holding registers", "modbus request: %s %s %s", q->from.c_str(), q->to.c_str(), q->func.c_str());
    CHECK(hasText(q->summary, "40108 (holding register 107)"), "modbus request summary: %s", q->summary.c_str());
    CHECK(r->dir == 2 && r->pair == q->id && q->pair == r->id && std::fabs(r->rttMs - 80) < 1, "modbus pairing: pair %llu rtt %.1f", (unsigned long long)r->pair, r->rttMs);
    CHECK(hasText(r->summary, "555, 0, 100"), "modbus response summary: %s", r->summary.c_str());
    auto ds = t.devices();
    CHECK(point(dev(ds, sc::Modbus, 17), 4, 108) == "0 (0x0000)" && point(dev(ds, sc::Modbus, 17), 4, 109) == "100 (0x0064)", "register map: %s", point(dev(ds, sc::Modbus, 17), 4, 109).c_str());
    // exception response
    t.add(fr(2.0, mbReadReq(5, 4, 9000, 2), sc::Modbus));
    t.add(fr(2.05, mb(5, 0x84, {0x02}), sc::Modbus));
    CHECK(lastOf(t)->func == "exception" && hasText(lastOf(t)->summary, "illegal data address") && lastOf(t)->pair, "exception: %s", lastOf(t)->summary.c_str());
    // write single coil, its echo
    const Bytes wc = mb(2, 5, {0x00, 0xAC, 0xFF, 0x00});
    t.add(fr(3.0, wc, sc::Modbus)); t.add(fr(3.03, wc, sc::Modbus));
    CHECK(t.messages()[t.messages().size() - 2].dir == 1 && lastOf(t)->dir == 2 && lastOf(t)->pair, "coil write and its echo pair");
    CHECK(hasText(lastOf(t)->summary, "00173 (coil 172) = ON"), "coil summary: %s", lastOf(t)->summary.c_str());
    // write multiple registers
    t.add(fr(4.0, mb(2, 16, {0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x0A, 0x01, 0x02}), sc::Modbus));
    t.add(fr(4.04, mb(2, 16, {0x00, 0x01, 0x00, 0x02}), sc::Modbus));
    ds = t.devices();
    CHECK(lastOf(t)->dir == 2 && lastOf(t)->pair && point(dev(ds, sc::Modbus, 2), 4, 2) == "258 (0x0102, written)", "FC16: %s", point(dev(ds, sc::Modbus, 2), 4, 2).c_str());
    // diagnostics echo
    const Bytes dg = mb(3, 8, {0x00, 0x00, 0xA5, 0x37});
    t.add(fr(5.0, dg, sc::Modbus)); t.add(fr(5.02, dg, sc::Modbus));
    CHECK(lastOf(t)->func == "diagnostics" && lastOf(t)->pair, "diagnostics echo pair");
    // read coils, a response of 3 octets (8 bytes, the same length as a request)
    t.add(fr(6.0, mbReadReq(4, 1, 20, 19), sc::Modbus));
    t.add(fr(6.05, mb(4, 1, {0x03, 0xCD, 0x6B, 0x05}), sc::Modbus));
    ds = t.devices();
    CHECK(lastOf(t)->dir == 2 && point(dev(ds, sc::Modbus, 4), 0, 20) == "ON" && point(dev(ds, sc::Modbus, 4), 0, 21) == "OFF", "FC1 response of 8 bytes is a response");
    // an unanswered poll
    t.add(fr(7.0, mbReadReq(7, 3, 0, 1), sc::Modbus));
    t.add(fr(8.0, mbReadReq(7, 3, 0, 1), sc::Modbus));
    ds = t.devices();
    CHECK(dev(ds, sc::Modbus, 7) && dev(ds, sc::Modbus, 7)->unanswered == 1 && dev(ds, sc::Modbus, 7)->polls == 2, "unanswered poll counted");
    // a damaged frame
    Bytes bad = mbReadReq(9, 3, 0, 1); bad[3] ^= 1;
    t.add(fr(9.0, bad, sc::Modbus, false));
    CHECK(lastOf(t)->crc == 0 && hasText(lastOf(t)->summary, "CRC failed"), "CRC failure shown");
    CHECK(t.counts.crcBad == 1 && t.counts.modbus >= 14, "counts: crcBad %llu modbus %llu", (unsigned long long)t.counts.crcBad, (unsigned long long)t.counts.modbus);
    CHECK(dev(t.devices(), sc::Modbus, -1) && dev(t.devices(), sc::Modbus, -1)->role == "master", "the master is listed");
}

// DNP3 traffic between master 1 and outstation 10 (link frames, in time order)
static std::vector<Bytes> dnpTraffic() {
    std::vector<Bytes> v;
    auto add = [&](const std::vector<Bytes>& fs) { v.insert(v.end(), fs.begin(), fs.end()); };
    v.push_back(dnpLink(0xC9, 10, 1, {}));                     // request link status
    v.push_back(dnpLink(0x0B, 1, 10, {}));                     // link status
    add(dnpApp(true, 10, 1, {0xC0, 0x01, 60, 2, 0x06, 60, 3, 0x06, 60, 4, 0x06, 60, 1, 0x06}, 0));   // integrity poll
    Bytes rsp{0xC0, 0x81, 0x90, 0x00, 30, 1, 0x00, 0, 3};
    for (int i = 0; i < 4; i++) { rsp.push_back(0x01); rsp = cat(rsp, le32((uint32_t)(1000 * i - 1500))); }
    rsp = cat(rsp, Bytes{1, 2, 0x00, 0, 7});
    for (int i = 0; i < 8; i++) rsp.push_back((uint8_t)(i % 3 == 0 ? 0x81 : 0x01));
    add(dnpApp(false, 1, 10, rsp, 0));
    Bytes ts{0xC1, 0x02, 50, 1, 0x07, 1};
    const uint64_t ms = 1794571200123ull;                      // 2026-11-13 12:00:00.123 UTC
    for (int i = 0; i < 6; i++) ts.push_back((uint8_t)(ms >> (8 * i)));
    add(dnpApp(true, 10, 1, ts, 1));                           // time sync
    add(dnpApp(false, 1, 10, {0xC1, 0x81, 0x00, 0x00}, 1));
    add(dnpApp(true, 10, 1, {0xC2, 0x01, 60, 2, 0x06}, 2));    // class 1 poll
    Bytes ev{0xE2, 0x81, 0x02, 0x00, 32, 1, 0x28, 1, 0, 2, 0, 0x01};
    ev = cat(ev, le32(4242));
    add(dnpApp(false, 1, 10, ev, 2));                          // an event, confirm requested
    add(dnpApp(true, 10, 1, {0xC2, 0x00}, 3));                 // application confirm
    add(dnpApp(false, 1, 10, {0xF3, 0x82, 0x00, 0x00, 2, 1, 0x28, 1, 0, 5, 0, 0x81}, 3));   // unsolicited
    add(dnpApp(true, 10, 1, {0xD3, 0x00}, 4));
    Bytes big{0xC4, 0x01, 30, 5, 0x06};                        // a large response over two transport segments
    add(dnpApp(true, 10, 1, big, 5));
    Bytes br{0xC4, 0x81, 0x00, 0x00, 30, 5, 0x01, 100, 0, 160, 0};   // AI 100 - 160 (2-octet start and stop)
    for (int i = 0; i <= 60; i++) { br.push_back(0x01); float f = 0.5f * i; uint32_t u; std::memcpy(&u, &f, 4); br = cat(br, le32(u)); }
    add(dnpApp(false, 1, 10, br, 4));
    add(dnpApp(true, 10, 1, {0xC5, 0x20, 120, 1, 0x5B, 1, 8, 0, 0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4}, 6));   // Secure Authentication
    return v;
}

static void dnpTracker() {
    sc::Tracker t;
    const auto v = dnpTraffic();
    double tm = 1;
    for (const Bytes& b : v) { t.add(fr(tm, b, sc::Dnp3)); tm += 0.1; }
    const auto& ms = t.messages();
    CHECK(ms.size() == v.size(), "dnp: %zu messages for %zu link frames", ms.size(), v.size());
    if (ms.size() < 15) return;
    CHECK(ms[0].func == "request link status" && ms[1].func == "link status" && ms[1].pair == ms[0].id, "link status pairing");
    CHECK(ms[0].from == "master 1" && ms[0].to == "outstation 10", "dnp addresses: %s > %s", ms[0].from.c_str(), ms[0].to.c_str());
    CHECK(ms[2].func == "read" && hasText(ms[2].summary, "integrity poll"), "integrity poll: %s", ms[2].summary.c_str());
    CHECK(ms[3].func == "response" && ms[3].pair == ms[2].id && hasText(ms[3].summary, "device restart") && hasText(ms[3].summary, "need time"), "response: %s", ms[3].summary.c_str());
    auto ds = t.devices();
    const sc::Device* o = dev(ds, sc::Dnp3, 10);
    CHECK(o && o->role == "outstation" && point(o, 14, 3) == "1500" && point(o, 14, 0) == "-1500" && point(o, 10, 3) == "1" && point(o, 10, 4) == "0",
          "dnp points: AI3 %s AI0 %s BI3 %s", point(o, 14, 3).c_str(), point(o, 14, 0).c_str(), point(o, 10, 3).c_str());
    CHECK(dev(ds, sc::Dnp3, 1) && dev(ds, sc::Dnp3, 1)->role == "master", "dnp master");
    CHECK(ms[4].func == "write" && hasText(ms[4].summary, "time sync") && hasText(ms[4].summary, "2026-11-13 12:00:00.123"), "time sync: %s", ms[4].summary.c_str());
    CHECK(ms[5].pair == ms[4].id, "time sync answered");
    CHECK(hasText(ms[6].summary, "class poll (class 1)"), "class 1 poll: %s", ms[6].summary.c_str());
    CHECK(ms[7].pair == ms[6].id && hasText(ms[7].summary, "confirm requested") && point(o, 14, 2) == "4242", "event response: %s (pair %llu, request %llu, AI2 %s)",
          ms[7].summary.c_str(), (unsigned long long)ms[7].pair, (unsigned long long)ms[6].id, point(o, 14, 2).c_str());
    CHECK(ms[8].func == "confirm" && ms[8].pair == ms[7].id, "application confirm paired");
    CHECK(ms[9].func == "unsolicited response" && ms[9].dir == 3 && ms[10].pair == ms[9].id, "unsolicited and its confirm");
    CHECK(ms[12].func == "transport segment" && ms[13].func == "response" && ms[13].pair == ms[11].id && point(o, 14, 160) == "30", "two segments: %s / %s, AI160 %s",
          ms[12].summary.c_str(), ms[13].summary.c_str(), point(o, 14, 160).c_str());
    CHECK(ms[14].encrypted && hasText(ms[14].summary, "not decoded"), "Secure Authentication shown, not decoded: %s", ms[14].summary.c_str());
    CHECK(t.counts.encrypted == 1, "encrypted count");
    // a damaged data block
    t.add(fr(20, dnpLink(0x44, 10, 1, Bytes(20, 0x11), 1), sc::Dnp3, false));
    CHECK(lastOf(t)->crc == 0 && hasText(lastOf(t)->summary, "CRC failed"), "dnp block CRC failure");
    // a segment without its first
    Bytes ud{0x83, 1, 2, 3};
    t.add(fr(21, dnpLink(0xC4, 10, 1, ud), sc::Dnp3));
    CHECK(hasText(lastOf(t)->summary, "without its first segment"), "segment without its first: %s", lastOf(t)->summary.c_str());
    // export
    const std::string csv = t.csv(), js = t.json();
    CHECK(hasText(csv, "id,time_s,wall_clock_utc,link,protocol,from,to,function,summary,crc,direction,pair_id,response_ms,encrypted,raw_hex"), "csv header");
    CHECK((size_t)std::count(csv.begin(), csv.end(), '\n') == t.messages().size() + 1, "csv rows");
    int depth = 0, minDepth = 0; bool inStr = false, ctrl = false;
    for (size_t i = 0; i < js.size(); i++) {
        const char c = js[i];
        if (inStr) { if (c == '\\') i++; else if (c == '"') inStr = false; else if ((unsigned char)c < 0x20) ctrl = true; continue; }
        if (c == '"') inStr = true; else if (c == '{' || c == '[') depth++; else if (c == '}' || c == ']') { depth--; minDepth = std::min(minDepth, depth); }
    }
    CHECK(depth == 0 && minDepth == 0 && !inStr && !ctrl, "json is balanced");
    CHECK(hasText(js, "\"receive_only\":true") && hasText(js, "\"point\":\"AI 160\""), "json content");
}

// ------------------------------------------------------------------ 2.- 4. through the air

struct Run {
    std::vector<sc::Message> msgs;
    std::vector<sc::Device> devs;
    std::string phy;
    dect2::X40Telemetry t;
};
static Run runAir(const std::vector<cf32>& x, double fs) {
    { sc::Store& st = sc::store(); std::lock_guard<std::mutex> lk(st.mu); st.tr.clear(); st.phy.clear(); }
    Run r;
    r.t = x40gen::run(dect2::x40::Scada, x, fs);
    sc::Store& st = sc::store();
    std::lock_guard<std::mutex> lk(st.mu);
    r.msgs.assign(st.tr.messages().begin(), st.tr.messages().end());
    r.devs = st.tr.devices();
    r.phy = st.phy;
    return r;
}

struct Score { int sent = 0, got = 0, wrong = 0, unknown = 0, crcBad = 0; std::string link; };
static void dumpMsgs(const Run& r) { for (const sc::Message& m : r.msgs) printf("    [%s] %s > %s %s: %s (%zu bytes)\n", m.link.c_str(), m.from.c_str(), m.to.c_str(), m.func.c_str(), m.summary.c_str(), m.raw.size()); }
static Score score(const Run& r, const std::vector<Bytes>& sent) {
    Score s; s.sent = (int)sent.size();
    std::vector<bool> used(sent.size(), false);
    for (const sc::Message& m : r.msgs) {
        if (m.proto == sc::Unknown) { s.unknown++; continue; }
        if (m.crc == 0) { s.crcBad++; continue; }
        bool hit = false;
        for (size_t i = 0; i < sent.size(); i++) if (!used[i] && sent[i] == m.raw) { used[i] = true; hit = true; break; }
        if (hit) { s.got++; s.link = m.link; } else s.wrong++;
    }
    return s;
}

static std::vector<Bytes> modbusTraffic(int n) {
    std::vector<Bytes> v;
    for (int k = 0; k < n / 2; k++) {
        const int a = 1 + k % 3;
        v.push_back(mbReadReq(a, 3, 100 + k, 4));
        v.push_back(mbRegResp(a, 3, {k * 10, 1000 + k, 0x7ABC, k & 1 ? 0xFFFF : 7}));
    }
    return v;
}

enum Framing { F8N1, F8E1, F8O1, FHdlc, FHdlcNrzi };
static const char* framingName(Framing f) { return f == F8N1 ? "8N1" : f == F8E1 ? "8E1" : f == F8O1 ? "8O1" : f == FHdlc ? "HDLC" : "HDLC NRZI"; }
static std::vector<Burst> bursts(const std::vector<Bytes>& frames, Framing fm, double gap = 0.04) {
    std::vector<Burst> b;
    for (const Bytes& f : frames) {
        Burst x;
        x.bits = fm == FHdlc || fm == FHdlcNrzi ? hdlc(f, fm == FHdlcNrzi) : uart(f, fm == F8E1 ? 1 : fm == F8O1 ? 2 : 0);
        x.gapAfterSec = gap;
        b.push_back(x);
    }
    return b;
}

struct PhyCase { const char* name; Phy p; const char* expect; };
static Phy afsk(bool bell) { Phy p; p.kind = 0; p.markHz = bell ? 1200 : 1300; p.spaceHz = bell ? 2200 : 2100; p.bitRate = 1200; p.fmDevHz = 3000; return p; }
static Phy fsk(int kind, double rate, double dev) { Phy p; p.kind = kind; p.bitRate = rate; p.devHz = dev; p.preambleBits = kind == 2 ? 96 : 64; return p; }

static double numberAfter(const std::string& s, const char* key) {
    const size_t k = s.find(key);
    return k == std::string::npos ? -1 : std::atof(s.c_str() + k + std::strlen(key));
}

static Score airCase(const char* label, const Phy& p, Air a, Framing fm, const std::vector<Bytes>& frames, bool print = true, Run* keep = nullptr) {
    const std::vector<cf32> x = render(p, a, bursts(frames, fm), 0.25, 0.9);
    Run r = runAir(x, a.fs);
    Score s = score(r, frames);
    if (print) printf("  %-44s %2d/%2d frames, %d wrong, %d unknown, %d CRC failed | %s\n", label, s.got, s.sent, s.wrong, s.unknown, s.crcBad, r.phy.c_str());
    if (print && s.got < s.sent) {   // which frames were lost, and what came instead
        for (size_t i = 0; i < frames.size(); i++) {
            bool hit = false;
            for (auto& m : r.msgs) if (m.crc == 1 && m.raw == frames[i]) hit = true;
            if (!hit) printf("    lost frame %zu (%zu bytes)\n", i, frames[i].size());
        }
        for (auto& m : r.msgs) if (m.crc != 1) printf("    [%s] %.3f s %s: %s %s\n", m.link.c_str(), m.timeSec, m.func.c_str(), m.summary.c_str(), sc::hex(m.raw.data(), m.raw.size(), 40).c_str());
    }
    if (keep) *keep = std::move(r);
    return s;
}

static void cleanMatrix() {
    printf("clean signals (C/N0 75 dB-Hz):\n");
    Air a; a.cn0 = 75;
    const auto mbt = modbusTraffic(8);
    const auto dnp = dnpTraffic();
    struct C { const char* label; Phy p; Framing fm; bool useDnp; const char* expect; };
    Phy inv = fsk(1, 9600, 3000); inv.invert = true;
    const C cs[] = {
        {"Bell 202 AFSK 8N1, Modbus", afsk(true), F8N1, false, "Bell 202 AFSK 1200 Bd, 8N1"},
        {"Bell 202 AFSK 8E1, Modbus", afsk(true), F8E1, false, "Bell 202 AFSK 1200 Bd, 8E1"},
        {"Bell 202 AFSK 8O1, DNP3", afsk(true), F8O1, true, "Bell 202 AFSK 1200 Bd, 8O1"},
        {"V.23 AFSK 8E1, Modbus", afsk(false), F8E1, false, "V.23 AFSK 1200 Bd, 8E1"},
        {"2-FSK 4800 8O1, Modbus", fsk(1, 4800, 2000), F8O1, false, "2-FSK 4800 bit/s, 8O1"},
        {"2-FSK 9600 8N1, DNP3", fsk(1, 9600, 3000), F8N1, true, "2-FSK 9600 bit/s, 8N1"},
        {"2-FSK 19200 8E1, Modbus", fsk(1, 19200, 4800), F8E1, false, "2-FSK 19200 bit/s, 8E1"},
        {"4-FSK 4800 8N1, Modbus", fsk(2, 4800, 1800), F8N1, false, "4-FSK 4800 bit/s (2400 symbols/s), 8N1"},
        {"4-FSK 9600 8E1, DNP3", fsk(2, 9600, 2400), F8E1, true, "4-FSK 9600 bit/s (4800 symbols/s), 8E1"},
        {"4-FSK 19200 8N1, Modbus", fsk(2, 19200, 4000), F8N1, false, "4-FSK 19200 bit/s (9600 symbols/s), 8N1"},
        {"2-FSK 9600 HDLC NRZI, DNP3", fsk(1, 9600, 3000), FHdlcNrzi, true, "2-FSK 9600 bit/s, HDLC NRZI"},
        {"4-FSK 9600 HDLC, Modbus", fsk(2, 9600, 2400), FHdlc, false, "4-FSK 9600 bit/s (4800 symbols/s), HDLC"},
        {"2-FSK 9600 8N1 inverted, Modbus", inv, F8N1, false, "2-FSK 9600 bit/s, 8N1, inverted"},
    };
    for (const C& c : cs) {
        const auto& frames = c.useDnp ? dnp : mbt;
        Run r;
        const Score s = airCase(c.label, c.p, a, c.fm, frames, true, &r);
        CHECK(s.got == s.sent && s.wrong == 0 && s.crcBad == 0, "%s: %d of %d frames, %d wrong, %d CRC failed", c.label, s.got, s.sent, s.wrong, s.crcBad);
        CHECK(s.link == c.expect, "%s: link named \"%s\", expected \"%s\"", c.label, s.link.c_str(), c.expect);
        CHECK(s.unknown == 0, "%s: %d bursts not recognised on a clean known signal", c.label, s.unknown);
        if (c.p.kind == 0) {
            const double mk = numberAfter(r.phy, "tones "), sp = numberAfter(r.phy, " / ");
            CHECK(std::fabs(mk - c.p.markHz) < 60 && std::fabs(sp - c.p.spaceHz) < 60, "%s: tones %.0f / %.0f", c.label, mk, sp);
            const double dv = numberAfter(r.phy, "deviation about ");
            CHECK(std::fabs(dv - c.p.fmDevHz / 1000) < 0.6, "%s: FM deviation %.1f kHz", c.label, dv);
        } else {
            const double dv = numberAfter(r.phy, c.p.kind == 2 ? "deviation (outer) " : "deviation ");
            CHECK(std::fabs(dv * 1000 - c.p.devHz) < 0.2 * c.p.devHz, "%s: deviation %.2f kHz, sent %.2f", c.label, dv, c.p.devHz / 1000);
        }
    }
}

static void impairments() {
    printf("impairments (C/N0 70 dB-Hz):\n");
    const auto mbt = modbusTraffic(6);
    struct P { const char* name; Phy p; Framing fm; };
    const P phys[] = {{"Bell 202 8E1", afsk(true), F8E1}, {"2-FSK 9600 8N1", fsk(1, 9600, 3000), F8N1}, {"4-FSK 19200 8N1", fsk(2, 19200, 4000), F8N1}};
    for (const P& ph : phys) {
        struct I { const char* what; Air a; Phy p; };
        std::vector<I> is;
        Air base; base.cn0 = 70;
        auto add = [&](const char* w, Air a, Phy p) { is.push_back({w, a, p}); };
        { Air a = base; a.offsetHz = 1500; add("offset +1500 Hz", a, ph.p); }
        { Air a = base; a.offsetHz = -1500; add("offset -1500 Hz", a, ph.p); }
        { Air a = base; a.offsetHz = -600; a.driftHzPerSec = 400; add("drift +400 Hz/s from -600 Hz", a, ph.p); }
        { Phy p = ph.p; if (p.kind == 0) p.fmDevHz *= 0.6; else p.devHz *= 0.7; add("deviation -30 % (AFSK -40 %)", base, p); }
        { Phy p = ph.p; if (p.kind == 0) p.fmDevHz *= 1.5; else p.devHz *= 1.3; add("deviation +30 % (AFSK +50 %)", base, p); }
        { Air a = base; a.amp = 0.001; add("level x0.001", a, ph.p); }
        { Air a = base; a.amp = 30; add("level x30", a, ph.p); }
        { Air a = base; a.clockPpm = 300; add("bit clock +300 ppm", a, ph.p); }
        { Air a = base; a.clockPpm = -300; add("bit clock -300 ppm", a, ph.p); }
        { Air a = base; a.fs = 192000; add("192 ksps", a, ph.p); }
        { Air a = base; a.fs = 1000000; add("1 Msps", a, ph.p); }
        { Air a = base; a.continuousCarrier = true; add("continuous carrier", a, ph.p); }
        if (ph.p.kind == 0) {
            { Phy p = ph.p; p.twistDb = -6; add("tone twist -6 dB (de-emphasis)", base, p); }
            { Phy p = ph.p; p.twistDb = 6; add("tone twist +6 dB (pre-emphasis)", base, p); }
        }
        for (const I& c : is) {
            char lab[96]; snprintf(lab, sizeof lab, "%s, %s", ph.name, c.what);
            const Score s = airCase(lab, c.p, c.a, ph.fm, mbt);
            CHECK(s.got == s.sent && s.wrong == 0, "%s: %d of %d frames, %d wrong", lab, s.got, s.sent, s.wrong);
        }
    }
}

static void noiseSweep() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") && std::atoi(std::getenv("ONAIR_TEST_LONG")) > 0;
    printf("AWGN sweep (C/N0 in dB-Hz; frames decoded of 10; 192 ksps):\n");
    const auto mbt = modbusTraffic(10);
    struct P { const char* name; Phy p; Framing fm; double must; };
    const P phys[] = {
        {"Bell 202 8E1", afsk(true), F8E1, 56},
        {"2-FSK 4800 8N1", fsk(1, 4800, 2000), F8N1, 56},
        {"2-FSK 9600 8N1", fsk(1, 9600, 3000), F8N1, 59},
        {"2-FSK 19200 8N1", fsk(1, 19200, 4800), F8N1, 62},
        {"4-FSK 4800 8N1", fsk(2, 4800, 1800), F8N1, 62},
        {"4-FSK 9600 8N1", fsk(2, 9600, 2400), F8N1, 67},
        {"4-FSK 19200 8N1", fsk(2, 19200, 4000), F8N1, 70},
    };
    for (const P& ph : phys) {
        printf("  %-16s", ph.name);
        double thr = -1;
        const double step = longRun ? 1 : 3;
        for (double cn0 = 72; cn0 >= 44; cn0 -= step) {
            Air a; a.fs = 192000; a.cn0 = cn0; a.seed = (unsigned)(cn0 * 7 + 3);
            const Score s = airCase("", ph.p, a, ph.fm, mbt, false);
            printf(" %.0f:%d", cn0, s.got);
            if (s.got >= 9) thr = cn0;
            CHECK(s.wrong == 0, "%s at %.0f dB-Hz: %d wrong frames (a frame with a valid CRC that was not sent)", ph.name, cn0, s.wrong);
        }
        printf("  | 90 %% point about %.0f dB-Hz\n", thr);
        Air a; a.fs = 192000; a.cn0 = ph.must; a.seed = 99;
        const Score s = airCase("", ph.p, a, ph.fm, mbt, false);
        CHECK(s.got >= 9, "%s at %.0f dB-Hz: %d of 10 frames", ph.name, ph.must, s.got);
    }
}

static void falseAndOdd() {
    printf("noise, unknown framings, CRC failures, pairing:\n");
    {   // noise alone, and an unmodulated carrier
        Air a; a.cn0 = 50;
        Phy p = fsk(1, 9600, 3000);
        const auto x = render(p, a, {}, 4.0, 0.1);
        Run r = runAir(x, a.fs);
        printf("  noise alone: %zu messages\n", r.msgs.size());
        dumpMsgs(r);
        CHECK(r.msgs.empty(), "noise alone gave %zu messages", r.msgs.size());
        Air c; c.cn0 = 70; c.continuousCarrier = true;
        auto y = render(p, c, {}, 4.0, 0.1);
        Run r2 = runAir(y, c.fs);
        printf("  idle carrier: %zu messages\n", r2.msgs.size());
        dumpMsgs(r2);
        CHECK(r2.msgs.empty(), "an idle carrier gave %zu messages", r2.msgs.size());
    }
    {   // unknown framings: async bytes without a Modbus or DNP3 structure, and plain bit-synchronous bursts
        std::mt19937 rng(11);
        Bytes junk(40); for (auto& v : junk) v = (uint8_t)rng();
        Air a; a.cn0 = 72;
        Run r;
        const Score s = airCase("2-FSK 9600 async bytes, no known frame", fsk(1, 9600, 3000), a, F8N1, {junk}, true, &r);
        CHECK(s.got == 0 && s.wrong == 0 && s.unknown == 1, "unknown async burst: %d unknown, %d wrong", s.unknown, s.wrong);
        if (s.unknown == 1) for (auto& m : r.msgs) if (m.proto == sc::Unknown) CHECK(m.raw.size() >= 40 && m.crc == -1 && hasText(m.summary, "not decoded"), "unknown burst raw %zu bytes", m.raw.size());
        Burst b; for (int i = 0; i < 600; i++) b.bits.push_back((uint8_t)(rng() & 1));
        const std::vector<cf32> x = render(fsk(2, 9600, 2400), a, {b}, 0.25, 0.9);
        Run r2 = runAir(x, a.fs);
        const Score s2 = score(r2, {});
        printf("  4-FSK bit-synchronous burst: %d not recognised, %d wrong\n", s2.unknown, s2.wrong);
        CHECK(s2.unknown == 1 && s2.wrong == 0, "bit-synchronous unknown burst: %d unknown, %d wrong", s2.unknown, s2.wrong);
    }
    {   // CRC failures through the air
        Bytes bad = mbReadReq(5, 3, 10, 2); bad[7] ^= 0x10;
        const Bytes dbad = dnpLink(0x44, 10, 1, Bytes(20, 0x33), 0);
        Air a; a.cn0 = 72;
        Run r;
        airCase("Bell 202 8N1 with damaged CRCs", afsk(true), a, F8N1, {bad, dbad}, true, &r);
        int mbBad = 0, dnpBad = 0;
        for (auto& m : r.msgs) { if (m.crc == 0 && m.proto == sc::Modbus) mbBad++; if (m.crc == 0 && m.proto == sc::Dnp3) dnpBad++; }
        if (mbBad != 1 || dnpBad != 1) dumpMsgs(r);
        CHECK(mbBad == 1 && dnpBad == 1, "CRC failures through the air: Modbus %d, DNP3 %d", mbBad, dnpBad);
    }
    {   // pairing through the air: response time against the generator's timing
        const auto mbt = modbusTraffic(8);
        Phy p = fsk(1, 9600, 3000);
        Air a; a.cn0 = 70;
        std::vector<double> st, en;
        const auto x = render(p, a, bursts(mbt, F8N1, 0.05), 0.25, 0.9, &st, &en);
        Run r = runAir(x, a.fs);
        int paired = 0; double worst = 0;
        for (auto& m : r.msgs) {
            if (m.dir != 2 || !m.pair) continue;
            paired++;
            size_t k = 0;
            for (; k < mbt.size(); k++) if (mbt[k] == m.raw) break;
            if (k == 0 || k >= mbt.size()) continue;
            const double expect = (en[k] - en[k - 1]) * 1000;
            worst = std::max(worst, std::fabs(m.rttMs - expect));
        }
        printf("  pairing: %d of 4 responses paired, response time error at most %.1f ms\n", paired, worst);
        CHECK(paired == 4 && worst < 15, "pairing through the air: %d paired, error %.1f ms", paired, worst);
        const sc::Device* d = dev(r.devs, sc::Modbus, 1);
        CHECK(d && d->polls == 2 && d->replies == 2 && point(d, 4, 100) == "0 (0x0000)" && point(d, 4, 102) == "31420 (0x7ABC)", "register map through the air: %s", point(d, 4, 102).c_str());
    }
}

int main() {
    crcChecks();
    finderChecks();
    modbusTracker();
    dnpTracker();
    cleanMatrix();
    impairments();
    falseAndOdd();
    noiseSweep();
    printf(fails ? "FAILED: %d\n" : "all SCADA telemetry checks passed\n", fails);
    return fails ? 1 : 0;
}
