// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT receiver on the test signal: OQPSK 72k (N2-3 / N2-4), QPSK 72k and OQPSK 80k interleaved, with carrier offsets, a
// symbol clock off, noise and other input rates; the picture strips must come back exactly as the strip coder makes them (the lossy
// step is only the coder's). Automatic modulation finds QPSK. Noise alone gives nothing. No real recording was available.
#include "dect2/meteor/meteor_rx.h"
#include "dect2/common/stable_random.h"
#include "dect2/meteor/meteor_tel.h"
#include "dect2/meteor/meteor_gen.h"
#include "dect2/meteor/meteor_proto.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

// the strip the receiver should show for picture row r, block k, channel c
static void expected(int c, int r, int k, uint8_t out[8][112]) {
    uint8_t px[8][112];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 112; x++) px[y][x] = meteorTestPixel(c, k * 112 + x, r * 8 + y);
    meteor::McuStrip s;
    meteor::decodeImagePacket(meteor::encodeImagePacket(px, k * 14, 80, 20000, 0), s);
    std::memcpy(out, s.px, sizeof s.px);
}

struct Result { MeteorTelemetry t; int rowsChecked = 0, rowsWrong = 0, offsets = 0; };

// fadeAt / fadeSec: noise only for a while, as when the antenna loses the satellite
static Result run(const char* name, MeteorGenConfig g, double seconds, int modSetting, double offsetHz = 0, double fadeAt = 0, double fadeSec = 0) {
    MeteorGenerator gen(g);
    MeteorReceiver rx;
    rx.setModulation(modSetting);
    if (getenv("METEOR_LOG")) rx.setLogCallback([](const std::string& m) { printf("    log: %s\n", m.c_str()); });
    rx.setSignalOffset(offsetHz);
    rx.configure(g.rate);
    std::vector<cf32> buf(65536);
    MeteorTelemetry t;
    uint64_t seq = 0;
    std::mt19937 frng(5); dect2::normal_distribution<float> fnd(0, 0.05f);
    for (double done = 0; done < seconds; done += (double)buf.size() / g.rate) {
        gen.generate(buf.data(), buf.size());
        if (fadeSec > 0 && done >= fadeAt && done < fadeAt + fadeSec) for (auto& v : buf) v = cf32{fnd(frng), fnd(frng)};
        if (offsetHz != 0) {   // the signal sits offsetHz from the centre
            static double ph = 0;
            for (auto& v : buf) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * 3.14159265358979 * offsetHz / g.rate; if (ph > 3.14159265358979) ph -= 2 * 3.14159265358979; }
        }
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    Result r; r.t = t;
    // complete strips must match a picture row exactly
    std::map<int, int> rowOf;
    if (t.image) {
        for (size_t c = 0; c < 6; c++) {
            int hint = 0;   // the test picture repeats itself: look first where the last strip's offset puts this one
            for (const auto& s : t.image->ch[c]) {
                if (!s || s->got != 0x3FFF) continue;
                int match = -1;
                uint8_t e[8][112];
                for (int i = 0; i < g.rows && match < 0; i++) {
                    const int pr = ((s->row - hint + i) % g.rows + g.rows) % g.rows;
                    bool all = true;
                    for (int k = 0; k < 14 && all; k++) {
                        expected((int)c, pr, k, e);
                        for (int y = 0; y < 8 && all; y++) all = !std::memcmp(e[y], &s->px[(size_t)y * kMeteorWidth + (size_t)k * 112], 112);
                    }
                    if (all) match = pr;
                }
                r.rowsChecked++;
                if (match < 0) r.rowsWrong++;
                else hint = s->row - match;
                if (match >= 0 && c == (size_t)(g.apids[0] - 64)) rowOf[s->row] = match;
            }
        }
    }
    // rows follow each other
    int seqWrong = 0;
    for (auto it = rowOf.begin(); it != rowOf.end(); ++it) {
        auto nx = std::next(it);
        if (nx != rowOf.end() && nx->first == it->first + 1 && nx->second != (it->second + 1) % g.rows) seqWrong++;
    }
    r.rowsWrong += seqWrong;
    // each strip in its own row: one offset between picture row and stored row, also across a fade
    std::map<int, int> offs;
    for (const auto& kv : rowOf) offs[((kv.first - kv.second) % g.rows + g.rows) % g.rows]++;
    r.offsets = (int)offs.size();
    if (getenv("METEOR_DBG")) for (const auto& kv : rowOf) printf("    row %d = picture %d\n", kv.first, kv.second);
    printf("  %-34s %s | frames %llu ok %llu bad, RS fixed %llu, cfo %+.0f Hz, SNR %.1f dB, clock %+.0f ppm, rows %d, strips %d checked, %d wrong\n", name,
           meteorSummary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.rsCorrected, t.cfoHz, t.snrDb,
           t.clockPpm, t.image ? t.image->rows : 0, r.rowsChecked, r.rowsWrong);
    return r;
}

static int only = -1, idx = 0;
static bool want() { return only < 0 || idx++ == only; }

int main(int argc, char** argv) {
    if (argc > 1) only = atoi(argv[1]);
    const ModeTuning mt = meteorTuning();
    CHECK(mt.stdMode == 61 && mt.defMhz == 137.9, "tuning");
    if (want()) {
        MeteorGenConfig g; g.modulation = 1; g.snrDb = 14; g.cfoHz = 1800; g.clockPpm = 40;
        const Result r = run("OQPSK 72k, +1.8 kHz, 14 dB", g, 8, 1);
        CHECK(r.t.blocksOk > 30 && r.rowsChecked >= 20 && r.rowsWrong == 0, "OQPSK 72k");
        CHECK(std::fabs(r.t.cfoHz - 1800) < 60, "cfo %.0f", r.t.cfoHz);
    }
    if (want()) {
        MeteorGenConfig g; g.modulation = 1; g.snrDb = 25; g.cfoHz = -4500; g.rate = 2.4e6;
        const Result r = run("OQPSK 72k, -4.5 kHz, 2.4 Msps, +200k", g, 6, 0, -200e3);
        CHECK(r.t.blocksOk > 20 && r.rowsWrong == 0 && r.t.modulation == 1, "OQPSK 72k at 2.4 Msps, offset");
    }
    if (want()) {
        MeteorGenConfig g; g.modulation = 2; g.snrDb = 14; g.cfoHz = -2500; g.clockPpm = -60; g.apids = {64, 65, 68};
        const Result r = run("QPSK 72k, -2.5 kHz, 14 dB, night", g, 8, 2);
        CHECK(r.t.blocksOk > 30 && r.rowsChecked >= 20 && r.rowsWrong == 0, "QPSK 72k");
        CHECK(r.t.image && r.t.image->packets[4] > 0 && r.t.image->packets[2] == 0, "night channels");
    }
    if (want()) {
        MeteorGenConfig g; g.modulation = 2; g.snrDb = 20; g.cfoHz = 700;
        const Result r = run("QPSK 72k found by automatic", g, 18, 0);
        CHECK(r.t.blocksOk > 20 && r.t.modulation == 2 && r.rowsWrong == 0, "automatic finds QPSK (running %d)", r.t.modulation);
    }
    if (want()) {   // Doppler at both ends of a pass, 10 dB
        for (double off : {5000.0, -5000.0}) {
            MeteorGenConfig g; g.modulation = 1; g.snrDb = 10; g.cfoHz = off;
            const Result r = run("OQPSK 72k, +-5 kHz, 10 dB", g, 26, 1, 0);
            CHECK(r.t.blocksOk > 20 && r.rowsWrong == 0 && std::fabs(r.t.cfoHz - off) < 100, "OQPSK 72k at %+.0f Hz, 10 dB: %llu frames, cfo %.0f", off, (unsigned long long)r.t.blocksOk, r.t.cfoHz);
            MeteorGenConfig q; q.modulation = 2; q.snrDb = 10; q.cfoHz = off;
            const Result r2 = run("QPSK 72k, +-5 kHz, 10 dB", q, 26, 2, 0);
            CHECK(r2.t.blocksOk > 20 && r2.rowsWrong == 0, "QPSK 72k at %+.0f Hz, 10 dB: %llu frames", off, (unsigned long long)r2.t.blocksOk);
        }
    }
    if (want()) {   // issue #79: Doppler sweeping through zero over a pass, tuner not correcting it (+3.5 kHz -> -3.5 kHz in 600 s)
        const double frameSec = 8192 / 72000.0;
        struct Case { const char* name; int mod; double snr, rate, start, perSec, secs; };
        const Case cs[] = {
            {"OQPSK 72k, Doppler 10x fast (60 s)", 1, 14, 250e3, 3500, -7000.0 / 60, 60},
            {"QPSK 72k, Doppler 10x fast (60 s)", 2, 14, 250e3, 3500, -7000.0 / 60, 60},
            {"OQPSK 72k, Doppler real rate through 0", 1, 14, 250e3, 350, -7000.0 / 600, 60},
            {"OQPSK 72k, Doppler real rate, 10 dB", 1, 10, 250e3, 1200, -7000.0 / 600, 60},
            {"OQPSK 72k, Doppler 10x fast, 1 Msps, 11 dB", 1, 11, 1e6, -3500, 7000.0 / 40, 40},   // rising instead; decimating front end
            {"OQPSK 80k il., Doppler 10x fast, 400k", 3, 22, 400e3, 3500, -7000.0 / 60, 60},
        };
        for (const Case& c : cs) {
            MeteorGenConfig g; g.modulation = c.mod; g.snrDb = c.snr; g.rate = c.rate; g.cfoHz = c.start; g.cfoDriftHzPerSec = c.perSec;
            if (c.mod == 2) g.apids = {64, 65, 68};
            const Result r = run(c.name, g, c.secs, c.mod);
            const double expect = (c.secs - (c.mod == 3 ? 24 : 3)) / (c.mod == 3 ? 8192 / 80000.0 : frameSec);
            CHECK(r.t.blocksOk > 0.9 * expect && r.rowsWrong == 0, "%s: %llu frames of ~%.0f", c.name, (unsigned long long)r.t.blocksOk, expect);
        }
    }
    if (want()) {   // issue #79: lock lost for 90 s mid-pass (more packets than half the 14-bit counter); the rows after it stay in place
        MeteorGenConfig g; g.modulation = 1; g.snrDb = 14; g.cfoHz = 900; g.rate = 250e3; g.rows = 2000;
        const Result r = run("OQPSK 72k, 90 s fade", g, 130, 1, 0, 20, 90);
        CHECK(r.t.blocksOk > 250 && r.rowsWrong == 0 && r.offsets == 1, "fade: %llu frames, %d row offsets", (unsigned long long)r.t.blocksOk, r.offsets);
    }
    if (want()) {
        MeteorGenConfig g; g.modulation = 3; g.snrDb = 22; g.cfoHz = 1200; g.rate = 400e3;
        const Result r = run("OQPSK 80k interleaved, 400 ksps", g, 26, 3);
        CHECK(r.t.blocksOk > 20 && r.rowsChecked >= 10 && r.rowsWrong == 0, "OQPSK 80k interleaved");
    }
    if (want()) {
        MeteorGenConfig g; g.modulation = getenv("TM") ? atoi(getenv("TM")) : 1; g.snrDb = getenv("TS") ? atof(getenv("TS")) : 8; if (getenv("TR")) g.rate = atof(getenv("TR"));
        const Result r = run("OQPSK 72k at 8 dB", g, getenv("TT") ? atof(getenv("TT")) : 8, g.modulation);
        CHECK(r.t.blocksOk > 10 && r.rowsWrong == 0, "8 dB");
    }
    if (want()) {   // the threshold: QPSK 72k at 2.5 dB and OQPSK 72k (N2-x, NRZ-M) at 4 dB Es/N0 (they failed at 4 and 5 dB before 2026-10)
        MeteorGenConfig q; q.modulation = 2; q.snrDb = 2.5; q.rate = 250e3;
        const Result r = run("QPSK 72k at 2.5 dB", q, 12, 2);
        CHECK(r.t.blocksOk > 80 && r.rowsWrong == 0, "QPSK 72k at 2.5 dB: %llu frames", (unsigned long long)r.t.blocksOk);
        MeteorGenConfig o; o.modulation = 1; o.snrDb = 4; o.rate = 250e3;
        const Result r2 = run("OQPSK 72k at 4 dB", o, 12, 1);
        CHECK(r2.t.blocksOk > 80 && r2.rowsWrong == 0, "OQPSK 72k at 4 dB: %llu frames", (unsigned long long)r2.t.blocksOk);
    }
    if (want()) {
        MeteorGenConfig g; g.amp = 0.0; g.snrDb = 99; g.modulation = 1;
        MeteorGenerator gen(g);
        MeteorReceiver rx; rx.configure(1e6);
        std::vector<cf32> buf(65536);
        std::mt19937 rng(3); dect2::normal_distribution<float> nd(0, 0.05f);
        MeteorTelemetry t; uint64_t seq = 0;
        for (int i = 0; i < 60; i++) { for (auto& v : buf) v = cf32{nd(rng), nd(rng)}; rx.feed(buf.data(), buf.size()); if (rx.telemetry(t, seq)) seq = t.seq; }
        CHECK(t.blocksOk == 0 && (!t.image || t.image->rows == 0), "noise: %llu frames", (unsigned long long)t.blocksOk);
    }
    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
