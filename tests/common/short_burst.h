// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Short voice transmissions (0.3 to 1 s) through a P25 or NXDN receiver, from a cold start and under real-world faults (3.2 quality pass).
// A schedule of short calls with different gaps (the start falls at a different symbol phase each time) is run once clean as the reference, then at
// each fault; per call the voice frames counted are compared with the clean run.
#pragma once
#include "dect2/common/ring.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace shortburst {

struct Fault { const char* name; double snr, cfo, ppm; double minShare; bool noHeader = false; double neighbourDb = -999; };

struct Result { int calls = 0, decoded = 0, extra = 0; double ratio = 0; double lostFrames = 0; };

// Gen: the generator config type; Tx: its transmission type; Rx/Tel: the receiver and its telemetry. durs: call lengths. fill sets the call's ids.
template <class Gen, class GenCfg, class Tx, class Rx, class Tel>
Result run(const GenCfg& base, const std::vector<Tx>& txs, const std::vector<unsigned>& refFrames, double tuneOffsetHz, double secs, std::vector<unsigned>* framesOut, double neighbourDb = -999) {
    GenCfg c = base;
    c.txs = txs;
    c.loop = false;
    c.dialOffsetHz = -tuneOffsetHz;
    Gen gen(c);
    // a strong voice transmitter on the next channel (12.5 kHz up), neighbourDb above the wanted one
    std::unique_ptr<Gen> nb;
    if (neighbourDb > -900) {
        GenCfg n = base;
        n.dialOffsetHz = c.dialOffsetHz + 12500; n.cfoHz = 0; n.snrDb = 99; n.loop = true; n.seed = base.seed + 50;
        n.amp = base.amp * std::pow(10.0, neighbourDb / 20);
        Tx t = txs[0];
        t.durSec = 3; t.gapSec = 0.2; t.src = 9;
        n.txs = {t};
        nb = std::make_unique<Gen>(n);
    }
    std::vector<dect2::cf32> nbuf;
    Rx rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setSignalOffset(c.dialOffsetHz);
    std::vector<dect2::cf32> buf((size_t)(c.rate * 0.01));
    for (double s = 0; s < secs; s += (double)buf.size() / c.rate) {
        gen.generate(buf.data(), buf.size());
        if (nb) { nbuf.resize(buf.size()); nb->generate(nbuf.data(), nbuf.size()); for (size_t i = 0; i < buf.size(); i++) buf[i] += nbuf[i]; }
        for (auto& v : buf) v = dect2::cf32(std::round(v.real() * 127) / 127, std::round(v.imag() * 127) / 127);
        rx.feed(buf.data(), buf.size());
    }
    Tel t;
    rx.telemetry(t, 0);
    if (getenv("SB_DEBUG2")) { if constexpr (requires { t.lcBad; }) printf("  frames %llu nidBad %llu lcOk %llu lcBad %llu esOk %llu esBad %llu ldu1 %llu ldu2 %llu late %llu\n", (unsigned long long)t.frames, (unsigned long long)t.nidBad, (unsigned long long)t.lcOk, (unsigned long long)t.lcBad, (unsigned long long)t.esOk, (unsigned long long)t.esBad, (unsigned long long)t.ldu1, (unsigned long long)t.ldu2, (unsigned long long)t.lateEntries); }
    Result r;
    // log is newest first; voice calls only
    std::vector<unsigned> got;
    for (size_t i = t.log.size(); i-- > 0;) if (t.log[i].kind == 0) got.push_back(t.log[i].voiceFrames);
    if (framesOut) *framesOut = got;
    if (getenv("SB_DEBUG")) { printf("  log %zu blocksOk %llu:", t.log.size(), (unsigned long long)t.blocksOk); for (size_t i = t.log.size(); i-- > 0;) printf(" [k%d f%u s%d]", t.log[i].kind, (unsigned)t.log[i].voiceFrames, (int)t.log[i].src); printf("\n"); }
    r.calls = (int)txs.size();
    for (size_t i = 0; i < txs.size(); i++) {
        const unsigned g = i < got.size() ? got[i] : 0, ref = i < refFrames.size() ? refFrames[i] : g;
        const double q = ref ? std::min(1.0, (double)g / ref) : 0;
        r.ratio += q;
        r.lostFrames += ref > g ? (double)(ref - g) : 0;
        if (q >= 0.9) r.decoded++;
    }
    if (got.size() > txs.size()) r.extra = (int)(got.size() - txs.size());
    r.ratio /= std::max(1, r.calls);
    r.lostFrames /= std::max(1, r.calls);
    return r;
}

} // namespace shortburst
