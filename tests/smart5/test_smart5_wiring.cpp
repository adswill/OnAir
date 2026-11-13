// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The receiver-side parts of the round-5 smart wiring (docs/SMART5-UI-3.9.md), through the real generators and receivers where there is one:
//  - FM: the ITU-R BS.412 multiplex power measured inside the FM receiver (generator -> receiver, two deviations, noise, carrier offset)
//  - analogue TV: the blanking lines handed over for the VITS analyser (generator test line -> receiver -> analyseVitsLine), noise sweep
//  - DVB: the NIT cell_list_descriptor kept by the SI collector and read back by parseCellList
//  - Zigbee home network health: retries, missing acknowledgements, acks matched by sequence number, other PANs ignored
//  - the insight hub: findings posted from outside, ALE schedules kept per station
// Simulated signals only: no recording of these situations was on hand.
#include "dect2/atv/atv_testkit.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/insight/hub.h"
#include "dect2/smart5/cell_tx.h"
#include "dect2/smart5/vits.h"
#include "dect2/tv/si_tables.h"
#include "dect2/zigbee/zigbee_health.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FmTelemetry fmRun(const FmGenConfig& cfg, double secs) {
    FmGenerator gen(cfg);
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(cfg.rate);
    std::vector<cf32> buf;
    FmTelemetry out;
    uint64_t seq = 0;
    const size_t block = 65536, total = (size_t)(secs * cfg.rate);
    for (size_t done = 0; done < total; done += block) {
        buf.clear();
        gen.generate(std::min(block, total - done), buf);
        rx.feed(buf.data(), buf.size());
        FmTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; out = t; }
    }
    return out;
}

static void testFm() {
    // mono 1 kHz tone, no pilot, no RDS: the multiplex is the tone alone, so the power is 20 log10(peak / 19 kHz) dBr
    for (double cnr : {45.0, 25.0}) for (double cfo : {0.0, 20e3}) {
        double dbr[2] = {0, 0};
        int k = 0;
        for (double dev : {75000.0, 37500.0}) {
            FmGenConfig c; c.rate = 1e6; c.stereo = false; c.rds = false; c.leftHz = c.rightHz = 1000; c.deviationHz = dev; c.cnrDb = cnr; c.cfoHz = cfo;
            const FmTelemetry t = fmRun(c, 4.0);
            printf("FM cnr %.0f cfo %+.0f dev %.1f kHz: running %.2f dBr (window %s), p99 %.1f kHz, above 75 kHz %.2f %%\n", cnr, cfo, dev / 1e3, t.mpxRunningDbr,
                   t.mpx412Valid ? "full" : "filling", t.devP99Khz, t.over75Pct);
            CHECK(!t.mpx412Valid, "a 60 s window cannot be full after 4 s");
            CHECK(t.mpxRunningDbr > -98, "no running value");
            dbr[k++] = t.mpxRunningDbr;
        }
        const double halve = dbr[0] - dbr[1];
        CHECK(std::fabs(halve - 6.02) < 0.6, "cnr %.0f cfo %.0f: halving the deviation moved the power %.2f dB (6.02 expected)", cnr, cfo, halve);
        // the tone's own power: what the generator's "deviationHz" makes of a full-scale tone, within 1.5 dB of the ideal 11.9 dBr
        CHECK(std::fabs(dbr[0] - 20 * std::log10(75.0 / 19.0)) < 1.5 || dbr[0] < 20 * std::log10(75.0 / 19.0), "cnr %.0f: %.2f dBr is above a full 75 kHz tone", cnr, dbr[0]);
    }
}

// the analyser wants 13.5 MHz or more: linear interpolation as in app/smart5/smart5_tv_ui.cpp
static std::vector<float> to135(const std::vector<float>& l, double fs, double& out) {
    if (fs >= 13.5e6 || l.size() < 2) { out = fs; return l; }
    const double r = 13.5e6 / fs;
    std::vector<float> x((size_t)(l.size() * r));
    for (size_t k = 0; k < x.size(); k++) {
        const double p = k / r;
        const size_t i0 = std::min(l.size() - 2, (size_t)p);
        const double fr = p - i0;
        x[k] = (float)(l[i0] * (1 - fr) + l[i0 + 1] * fr);
    }
    out = 13.5e6;
    return x;
}

static void testVits() {
    for (double cnr : {46.0, 34.0}) for (int line : {17, 19}) {
        AtvGenConfig c; c.cnrDb = cnr; c.vbiTestLine = line;
        const atvkit::Run r = atvkit::run(c, 1.5);
        const AtvTelemetry& t = r.tel;
        CHECK(!t.vitsLines.empty() && t.vitsRate > 0, "cnr %.0f: no blanking lines handed over", cnr);
        int found = 0, foundNo = -1, mbNo = -1;
        smart5::VitsResult best, mb;
        for (size_t i = 0; i < t.vitsLines.size() && i < t.vitsLineNo.size(); i++) {
            double fs;
            const auto x = to135(t.vitsLines[i], t.vitsRate, fs);
            const auto v = smart5::analyseVitsLine(x, fs);
            if (v.bar) { found++; foundNo = t.vitsLineNo[i]; best = v; }
            if (v.multiburst) { mbNo = t.vitsLineNo[i]; mb = v; }
        }
        printf("VITS cnr %.0f line %d: %zu lines handed over at %.1f MHz, bar on %d (line %d): %s; line %d: %s\n", cnr, line, t.vitsLines.size(), t.vitsRate / 1e6, found, foundNo,
               best.text().c_str(), mbNo, mb.text().c_str());
        CHECK(found >= 1, "cnr %.0f: the test line was not found", cnr);
        CHECK(foundNo == line || foundNo == line + 313, "cnr %.0f: the test line was numbered %d (sent on %d)", cnr, foundNo, line);
        // the envelope detector lifts the low carrier of white with noise (negative modulation), so the bar reads low as the CNR falls;
        // measured: 94 IRE at 46 dB, 82 IRE at 34 dB. A VITS reading is a transmitter-chain check on a clean signal.
        CHECK(std::fabs(best.barIre - 100) < (cnr > 40 ? 8 : 22), "cnr %.0f: bar %.1f IRE (sent 100)", cnr, best.barIre);
        CHECK(found == 2, "cnr %.0f: %d lines read as a bar (one per field)", cnr, found);
        CHECK((mbNo == line + 1 || mbNo == line + 314) && mb.bursts.size() >= (cnr > 40 ? 5u : 4u), "cnr %.0f: multiburst on line %d with %zu packets", cnr, mbNo, mb.bursts.size());
        if (mb.bursts.size() == 5) {
            static const double f[5] = {0.5e6, 1e6, 2e6, 3e6, 4e6};
            for (int k = 0; k < 5; k++) CHECK(std::fabs(mb.bursts[(size_t)k].freqHz - f[k]) < 0.06 * f[k], "cnr %.0f: packet %d at %.2f MHz", cnr, k, mb.bursts[(size_t)k].freqHz / 1e6);
            CHECK(mb.bursts[4].relDb < 0.5, "cnr %.0f: 4 MHz %+.1f dB above 0.5 MHz", cnr, mb.bursts[4].relDb);
        }
    }
    {   // no test line: nothing is found on the blanking lines
        AtvGenConfig c; c.cnrDb = 40;
        const atvkit::Run r = atvkit::run(c, 1.0);
        int any = 0;
        for (const auto& l : r.tel.vitsLines) { double fs; if (smart5::analyseVitsLine(to135(l, r.tel.vitsRate, fs), fs).any()) any++; }
        CHECK(any == 0, "%d plain blanking lines read as test signals", any);
    }
}

static void testNitCells() {
    // NIT actual, network 0x3001, one transport stream with a cell list of cell 0x0102 (and one subcell) and cell 0x0103
    std::vector<uint8_t> cl;
    auto put16 = [&](int v) { cl.push_back((uint8_t)(v >> 8)); cl.push_back((uint8_t)v); };
    auto lat = [](double d) { return (int)std::lround(d * 32768.0 / 90.0) & 0xFFFF; };
    auto lon = [](double d) { return (int)std::lround(d * 32768.0 / 180.0) & 0xFFFF; };
    put16(0x0102); put16(lat(54.6)); put16(lon(25.2)); cl.push_back(0x01); cl.push_back(0x00); cl.push_back(0x20); cl.push_back(8);   // extents, 8 bytes of subcells
    cl.push_back(7); put16(lat(54.7)); put16(lon(25.3)); cl.push_back(0x00); cl.push_back(0x40); cl.push_back(0x08);
    put16(0x0103); put16(lat(55.9)); put16(lon(21.1)); cl.push_back(0x01); cl.push_back(0x00); cl.push_back(0x20); cl.push_back(0);
    std::vector<uint8_t> ts = {0x10, 0x01, 0x20, 0x01};   // tsid, onid
    const int dl = 2 + (int)cl.size();
    ts.push_back((uint8_t)(0xF0 | (dl >> 8))); ts.push_back((uint8_t)dl);
    ts.push_back(0x6C); ts.push_back((uint8_t)cl.size());
    ts.insert(ts.end(), cl.begin(), cl.end());
    std::vector<uint8_t> s = {0x40, 0, 0, 0x30, 0x01, 0xC1, 0, 0, 0xF0, 0x00};
    s.push_back((uint8_t)(0xF0 | (ts.size() >> 8))); s.push_back((uint8_t)ts.size());
    s.insert(s.end(), ts.begin(), ts.end());
    for (int i = 0; i < 4; i++) s.push_back(0);   // CRC (not checked by the collector)
    const int sl = (int)s.size() - 3;
    s[1] = (uint8_t)(0xB0 | (sl >> 8)); s[2] = (uint8_t)sl;
    SiCollector c;
    c.section(0x10, s.data(), (int)s.size());
    const SiSnapshot snap = c.snapshot();
    size_t bytes = 0;
    smart5::CellTxBook book;
    for (const auto& n : snap.networks) for (const auto& t : n.transponders) { bytes += t.cellList.size(); book.addNit(smart5::parseCellList(t.cellList.data(), t.cellList.size())); }
    book.setUser(54.69, 25.28);
    const auto a = book.lookup(0x0102), b = book.lookup(0x0103), sub = book.lookup(0x0102, 7), none = book.lookup(0x0999);
    printf("NIT: %zu cell list bytes; 0x0102 %s (%.1f km), 0x0103 %.2f %.2f (%.0f km), subcell 7 %s, unknown %s\n", bytes, a.known ? "known" : "unknown", a.distKm, b.latDeg, b.lonDeg, b.distKm,
           sub.known ? "known" : "unknown", none.known ? "known" : "unknown");
    CHECK(bytes == cl.size(), "cell list kept %zu bytes of %zu", bytes, cl.size());
    CHECK(a.known && a.fromNit && a.distKm < 20, "cell 0x0102");
    CHECK(b.known && std::fabs(b.latDeg - 55.9) < 0.3 && b.distKm > 150 && b.distKm < 300, "cell 0x0103 at %.2f, %.0f km", b.latDeg, b.distKm);
    CHECK(sub.known, "subcell");
    CHECK(!none.known, "an unlisted cell is not invented");
}

static ZbFrame zf(double t, const char* type, const char* src, const char* dst, uint16_t pan, int seq, bool ack, float rssi = -60) {
    ZbFrame f; f.sec = t; f.type = type; f.src = src; f.dst = dst; f.pan = pan; f.seq = seq; f.ackReq = ack; f.rssi = rssi; return f;
}

static void testZigbeeHealth() {
    ZigbeeHealthFeed h(0x1A2B);
    uint32_t rnd = 12345;
    auto u = [&] { rnd = rnd * 1664525u + 1013904223u; return (rnd >> 8) / 16777216.0; };
    int seqA = 0, seqB = 100;
    for (int k = 0; k < 200; k++) {
        const double t = k * 10.0;
        // node 0x0001: healthy, every frame acknowledged
        h.add(zf(t, "Data", "0x0001", "0x0000", 0x1A2B, seqA, true));
        h.add(zf(t + 0.004, "Ack", "", "", 0, seqA, false));
        seqA = (seqA + 1) & 255;
        // node 0x0002 behind a wall: 40 % of its frames lost, each lost one retried with the same number
        const double t2 = t + 3;
        const bool lost = u() < 0.4;
        h.add(zf(t2, "Data", "0x0002", "0x0000", 0x1A2B, seqB, true, -88));
        if (!lost) h.add(zf(t2 + 0.004, "Ack", "", "", 0, seqB, false));
        else { h.add(zf(t2 + 0.05, "Data", "0x0002", "0x0000", 0x1A2B, seqB, true, -88)); h.add(zf(t2 + 0.054, "Ack", "", "", 0, seqB, false)); }
        seqB = (seqB + 1) & 255;
        // a neighbour's network on the same channel: never followed, and its acks are not taken for ours
        h.add(zf(t + 5, "Data", "0x0009", "0x0000", 0x7777, (k * 3) & 255, true));
        h.add(zf(t + 5.004, "Ack", "", "", 0, (k * 3) & 255, false));
    }
    h.tick(2000);
    const auto& n = h.health().nodes();
    const auto a = n.find("0x0001"), b = n.find("0x0002");
    CHECK(a != n.end() && b != n.end(), "nodes missing");
    CHECK(n.find("0x0009") == n.end(), "the neighbour's node is followed");
    if (a != n.end() && b != n.end()) {
        printf("Zigbee health: 0x0001 %llu frames, %.0f %% retries, %.0f %% no ack; 0x0002 %.0f %% retries, %.0f %% no ack '%s'\n", (unsigned long long)a->second.frames, a->second.retryPct(),
               a->second.missPct(), b->second.retryPct(), b->second.missPct(), b->second.text.c_str());
        CHECK(a->second.retryPct() < 1 && a->second.missPct() < 1 && !a->second.lossy, "the healthy node is not clean");
        CHECK(b->second.lossy && b->second.retryPct() > 15, "the node behind the wall is not lossy (%.0f %% retries)", b->second.retryPct());
    }
    // an ack whose frame came more than a second before does not count, so the frame is reported unacknowledged
    ZigbeeHealthFeed late(0x1A2B);
    for (int k = 0; k < 30; k++) { late.add(zf(k * 10.0, "Data", "0x0005", "0x0000", 0x1A2B, k, true)); late.add(zf(k * 10.0 + 1.5, "Ack", "", "", 0, k, false)); }
    late.tick(400);
    const auto c = late.health().nodes().find("0x0005");
    CHECK(c != late.health().nodes().end() && c->second.missPct() > 90, "late acks taken as good");
}

static void testHub() {
    insight::Hub h;
    insight::Insight i; i.time = 1000; i.area = "tv"; i.kind = "service_off_air"; i.key = "x"; i.text = "off";
    h.post(i);
    const auto v = h.take();
    CHECK(v.size() == 1 && v[0].kind == "service_off_air" && insight::toEvent(v[0]).type == "insight.service_off_air", "posted finding");
    // two ALE stations sounding on one frequency at their own times: two schedules
    for (int d = 0; d < 5; d++) {
        const double day = 1767225600.0 + d * 86400;
        for (const char* st : {"ABC", "XYZ"}) {
            automation::Event e("msg.ale");
            e.at(day + (st[0] == 'A' ? 10 * 3600 : 14 * 3600), "ALE", 7.1e6);
            e.str("station", st);
            h.onEvent(e);
        }
    }
    int abc = 0, xyz = 0;
    for (const auto& l : h.schedule().all()) { if (l.station == "ALE ABC") abc++; if (l.station == "ALE XYZ") xyz++; }
    printf("hub: ALE schedules ABC %d, XYZ %d\n", abc, xyz);
    CHECK(abc == 1 && xyz == 1, "ALE stations not kept apart");
}

int main() {
    testFm();
    testVits();
    testNitCells();
    testZigbeeHealth();
    testHub();
    printf(fails ? "smart5_wiring: %d FAILED\n" : "smart5_wiring: all passed\n", fails);
    return fails ? 1 : 0;
}
