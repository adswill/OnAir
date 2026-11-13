// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The HFDL receiver with its channels on the shared pool gives the same result as with the channels one after the other:
// two receivers get the same samples (traffic on three channels), the messages (order, serial numbers, frequencies, times), the
// counters and the channel table must be identical.
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include <chrono>
#include <cstdio>
#include <memory>
using namespace dect2;
static const double kRate = 48000, kCentre = 8.935e6;
static const double kChans[3] = {8.927e6, 8.942e6, 8.936e6};

int main() {
    HfdlGenOptions o;
    o.centerHz = kCentre;
    o.channelsHz = {kChans[0], kChans[1], kChans[2]};
    o.gsIds = {1, 7, 2};
    o.aircraft = 4;
    o.rateFactor = 4;
    o.log = std::make_shared<HfdlSentLog>();
    SynthConfig sc; sc.snrDb = 25; sc.cfoHz = 50;
    auto syn = makeHfdlSynthEx(o, sc, kRate);
    HfdlReceiver a, b;
    for (HfdlReceiver* r : {&a, &b}) { r->configure(kRate); r->setCenterHz(kCentre); r->setChannels(o.channelsHz); }
    a.setParallel(false);
    b.setParallel(true);
    std::vector<cf32> buf(4800);
    double ta = 0, tb = 0;
    for (int i = 0; i < 100 * 10; i++) {
        syn->generate(buf.data(), buf.size());
        auto t0 = std::chrono::steady_clock::now();
        a.feed(buf.data(), buf.size());
        auto t1 = std::chrono::steady_clock::now();
        b.feed(buf.data(), buf.size());
        auto t2 = std::chrono::steady_clock::now();
        ta += std::chrono::duration<double>(t1 - t0).count();
        tb += std::chrono::duration<double>(t2 - t1).count();
    }
    HfdlTelemetry x, y;
    a.telemetry(x, ~0ull);
    b.telemetry(y, ~0ull);
    int fails = 0;
    auto check = [&](bool c, const char* what) { if (!c) { printf("FAIL: %s\n", what); fails++; } };
    check(x.messages.size() == y.messages.size() && !x.messages.empty(), "message count");
    for (size_t i = 0; i < x.messages.size() && i < y.messages.size(); i++) {
        const auto &m = x.messages[i], &n = y.messages[i];
        if (m.serial != n.serial || m.freqHz != n.freqHz || m.timeSec != n.timeSec || m.kind != n.kind || m.icao != n.icao || m.acId != n.acId || m.gsId != n.gsId || m.snrDb != n.snrDb || m.levelDb != n.levelDb) { printf("FAIL: message %zu differs\n", i); fails++; break; }
    }
    check(x.blocksOk == y.blocksOk && x.blocksBad == y.blocksBad && x.squitters == y.squitters && x.lpdus == y.lpdus && x.acarsMsgs == y.acarsMsgs && x.positionsTotal == y.positionsTotal, "counters");
    check(x.aircraft.size() == y.aircraft.size() && x.stations.size() == y.stations.size(), "aircraft / stations");
    check(x.channels.size() == y.channels.size(), "channel table size");
    for (size_t i = 0; i < x.channels.size() && i < y.channels.size(); i++) check(x.channels[i].good == y.channels[i].good && x.channels[i].bad == y.channels[i].bad && x.channels[i].levelDb == y.channels[i].levelDb, "channel row");
    printf("hfdl threads: %zu messages, %llu good blocks; sequential %.2f s, parallel %.2f s wall\n", x.messages.size(), (unsigned long long)x.blocksOk, ta, tb);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
