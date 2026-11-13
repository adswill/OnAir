// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP receiver on the generator's signals: every frame type of the test script, then the real-world impairments one by one: tuning
// errors across the whole search range, noise, a sender clock off by 100 ppm, short and long leaders, a mis-tuned sender, a session ID,
// and pure noise (nothing may be reported).
#include "dect2/ardop/ardop_gen.h"
#include "dect2/ardop/ardop_proto.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/ardop/ardop_tel.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 250000;

static ArdopTelemetry run(ArdopGenConfig c, double extraSec = 1.5) {
    c.rate = kRate;
    c.dialOffsetHz = -20000;
    c.repeat = false;
    ArdopGenerator g(c);
    ArdopReceiver r;
    r.setSilent(true);
    r.configure(kRate);
    r.setSignalOffset(-20000);
    std::vector<cf32> buf(25000);
    const size_t tot = (size_t)((g.scriptSeconds() + extraSec) * kRate);
    for (size_t i = 0; i < tot; i += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    r.flush();
    ArdopTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const ArdopFrameLog* find(const ArdopTelemetry& t, const char* name, int nth = 0) {
    int k = 0;
    for (auto it = t.frames.rbegin(); it != t.frames.rend(); ++it)
        if (it->name == name && k++ == nth) return &*it;
    return nullptr;
}

static std::vector<ArdopGenFrame> mini() {   // an ID, a connect request and a 500 Hz data frame: the slow-to-run subset for the sweeps
    using namespace ardop;
    std::vector<ArdopGenFrame> v(3);
    v[0].code = 0x30; v[0].body = makeIdFrame("LY2ABC", "KO24HL");
    v[1].code = 0x32; v[1].body = makeCallFrame(0x32, "LY2ABC", "LY1XYZ");
    const std::string m = "ARDOP SWEEP";
    makeDataBlock(0x4C, std::vector<uint8_t>(m.begin(), m.end()), v[2].body);
    v[2].code = 0x4C; v[2].sid = 0x5A;
    return v;
}

static bool miniOk(const ArdopTelemetry& t) {
    const ArdopFrameLog *a = find(t, "IDFrame"), *b = find(t, "ConReq500M"), *c = find(t, "4FSK.500.100S.E");
    return a && a->bodyOk && a->from == "LY2ABC" && a->locator == "KO24HL" && b && b->bodyOk && b->to == "LY1XYZ" && c && c->bodyOk && c->text == "ARDOP SWEEP" && c->sid == 0x5A;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    {   // the whole script, clean
        ArdopGenConfig c;
        c.snrDb = 99;
        c.frames = ardopTestFrames();
        const ArdopTelemetry t = run(c);
        printf("  %zu frames, %llu leaders, %zu stations, %llu data ok / %llu bad\n", t.frames.size(), (unsigned long long)t.leaders, t.stations.size(), (unsigned long long)t.dataOk, (unsigned long long)t.dataBad);
        CHECK(t.frames.size() == ardopTestFrames().size(), "%zu frames", t.frames.size());
        const ArdopFrameLog* f = find(t, "IDFrame");
        CHECK(f && f->bodyOk && f->from == "LY2ABC" && f->locator == "KO24HL" && f->sid == 255 && f->kind == "ID", "ID frame");
        f = find(t, "ConReq500M");
        CHECK(f && f->bodyOk && f->from == "LY2ABC" && f->to == "LY1XYZ", "connect request");
        f = find(t, "ConAck500");
        CHECK(f && f->bodyOk && f->leaderMs == 160, "connect acknowledge leader %d", f ? f->leaderMs : -1);
        f = find(t, "Ping");
        CHECK(f && f->bodyOk && f->from == "LY1XYZ" && f->to == "LY2ABC", "ping");
        f = find(t, "PingAck");
        CHECK(f && f->bodyOk && f->snrDb == 12 && f->quality == 80, "ping ack %d %d", f ? f->snrDb : -1, f ? f->quality : -1);
        f = find(t, "4FSK.200.50S.E");
        CHECK(f && f->bodyOk && f->text == "CQ CQ DE LY2ABC" && f->sid == 0x5A && f->baud == 50, "50 baud data frame");
        f = find(t, "4FSK.500.100S.E");
        CHECK(f && f->bodyOk && f->text == "HELLO FROM LY2ABC" && f->baud == 100, "short 500 Hz data frame");
        f = find(t, "4FSK.500.100.O");
        CHECK(f && f->bodyOk && f->text == "ARDOP 4FSK 500 Hz monitor test, 100 baud, odd frame.", "long odd data frame");
        f = find(t, "DataACK");
        CHECK(f && f->quality == 70 && f->sid == 0x5A && f->bodyOk == false && !f->bodyRead, "ACK quality %d", f ? f->quality : -1);
        f = find(t, "DataNAK");
        CHECK(f && f->quality == 48, "NAK quality");
        f = find(t, "8PSK.200.100.E");
        CHECK(f && f->headerOk && !f->bodyRead && f->mod == "8PSK", "PSK frame listed by its header");
        CHECK(find(t, "DISC") && find(t, "END"), "disconnect, end");
        CHECK(t.stations.size() == 2 && t.dataOk == 3 && t.dataBad == 0, "stations %zu data %llu/%llu", t.stations.size(), (unsigned long long)t.dataOk, (unsigned long long)t.dataBad);
        CHECK(t.text.find("HELLO FROM LY2ABC") != std::string::npos && t.text.find("CQ CQ DE LY2ABC") != std::string::npos, "text \"%s\"", t.text.c_str());
        CHECK(t.blocksBad == 0, "%llu unreadable leaders", (unsigned long long)t.blocksBad);
        CHECK(t.frames.size() && std::fabs(t.frames[0].offsetHz) < 3, "offset");
    }
    // tuning errors across the search range (positive and negative, between and on the trial offsets), in noise
    for (double cfo : {-40.0, -22.0, -7.0, 0.0, 9.0, 21.0, 38.0}) {
        ArdopGenConfig c;
        c.snrDb = 18;
        c.cfoHz = cfo;
        c.frames = mini();
        const ArdopTelemetry t = run(c);
        CHECK(miniOk(t), "cfo %+.0f Hz: %zu frames", cfo, t.frames.size());
        if (!t.frames.empty()) CHECK(std::fabs(t.frames[0].offsetHz - cfo) < 4, "cfo %+.0f read as %+.1f", cfo, t.frames[0].offsetHz);
    }
    {   // symbol clock error
        for (double ppm : {-120.0, 120.0}) {
            ArdopGenConfig c; c.snrDb = 20; c.clockPpm = ppm; c.frames = mini();
            CHECK(miniOk(run(c)), "clock %+.0f ppm", ppm);
        }
    }
    {   // leader lengths 100 ms to 1 s
        for (int lead : {100, 160, 600, 1000}) {
            ArdopGenConfig c; c.snrDb = 20; c.frames = mini();
            for (auto& f : c.frames) f.leaderMs = lead;
            CHECK(miniOk(run(c)), "leader %d ms", lead);
        }
    }
    {   // weaker signals: the header and the Reed-Solomon code carry the frames down to a few dB
        for (double snr : {12.0, 8.0}) {
            ArdopGenConfig c; c.snrDb = snr; c.frames = mini();
            const ArdopTelemetry t = run(c);
            const ArdopFrameLog* a = find(t, "IDFrame");
            const ArdopFrameLog* d = find(t, "4FSK.500.100S.E");
            printf("  snr %.0f dB: id %d data %d\n", snr, a && a->bodyOk, d && d->bodyOk);
            CHECK(a && a->headerOk && d && d->headerOk, "snr %.0f: headers", snr);
            if (snr >= 12) CHECK(a && d && a->bodyOk && d->bodyOk, "snr %.0f: bodies", snr);
        }
    }
    {   // noise only: no frames
        ArdopGenConfig c; c.snrDb = 20; c.frames.clear();
        ArdopGenFrame f; f.code = 0x30; f.body = ardop::makeIdFrame("X", "Y"); f.leaderMs = 100; c.frames.push_back(f);
        c.startSec = 40;   // the frame never comes within the run
        ArdopGenerator g(c);
        (void)g;
        ArdopGenConfig n = c;
        n.frames.clear();
        const ArdopTelemetry t = run(n);   // an empty script: only noise
        CHECK(t.frames.empty() && t.blocksOk == 0, "frames from noise: %zu", t.frames.size());
    }
    {   // a gap between the leader and the end: truncated frame does not wedge the receiver (the next frame is still read)
        ArdopGenConfig c; c.snrDb = 25; c.frames = mini();
        c.frames[1].body.resize(5);   // a connect request cut short: the RS code cannot fix it
        const ArdopTelemetry t = run(c);
        const ArdopFrameLog *a = find(t, "IDFrame"), *d = find(t, "4FSK.500.100S.E");
        CHECK(a && a->bodyOk && d && d->bodyOk, "frames around a damaged one");
    }
    return testSummary("ardop rx");
}
