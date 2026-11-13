// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared helpers of the HaLow round-trip sweeps (test_halow_sweep_*.cpp): one frame sent repeatedly through the generator with a set of impairments,
// received at the radio rate the width needs, and the telemetry read after a flush. Every sweep covers all valid MCS x both preambles x guard intervals
// x impairment sets for its widths: a result from loopback alone is not claimed fixed, so the sets cover offsets, clock error, echo and a late start.
#pragma once
#include "dect2/halow/halow_gen.h"
#include "dect2/halow/halow_proto.h"
#include "dect2/halow/halow_rx.h"
#include "dect2/halow/halow_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace sweep {
using namespace dect2;
using namespace dect2::halow;

struct Impair { const char* name; double cfo, dial, ppm, echoUs, echoDb; bool midStart; };
inline const std::vector<Impair>& sets() {
    static const std::vector<Impair> s = {
        {"clean", 0, 0, 0, 0, -99, false},
        {"cfo +30 kHz, off centre, +40 ppm", 30e3, -400e3, 40, 0, -99, false},
        {"cfo -30 kHz, -40 ppm, echo 2 us -10 dB, mid-frame start", -30e3, 0, -40, 2.0, -10, true},
    };
    return s;
}

// a data frame whose size makes a few symbols at every MCS
inline std::vector<uint8_t> payloadFrame(size_t n) {
    const uint8_t bs[6] = {0x24, 0x0A, 0xC4, 1, 2, 3}, st[6] = {0xB8, 0x27, 0xEB, 4, 5, 6};
    return makeDataFrame(bs, st, n);
}

struct Result { double sent = 0; uint64_t ok = 0, bad = 0, sigOk = 0; HalowTelemetry t; };

// Sends `frame` repeatedly for about `count` PPDUs at the given radio rate and reads what the monitor got.
inline Result run(HalowGenFrame frame, const Impair& im, double rate, int count, double snrInBand = 40.0) {
    HalowGenConfig c;
    c.rate = rate;
    c.cfoHz = im.cfo; c.dialOffsetHz = im.dial; c.clockPpm = im.ppm;
    c.echoDelaySec = im.echoUs * 1e-6; c.echoGainDb = im.echoDb;
    c.snrDb = snrInBand >= 99 ? 99 : snrInBand + 10 * std::log10((double)(1 << frame.width));   // in-band SNR -> SNR in 1 MHz
    frame.gapSec = 0.0008;
    c.frames = {frame};
    const double per = halowFrameSeconds(frame) + frame.gapSec;
    if (im.midStart) c.skipSamples = (size_t)(c.leadSec * rate + 0.5 * halowFrameSeconds(frame) * rate);   // the stream starts in the middle of the first PPDU
    HalowGenerator g(c);
    HalowReceiver r;
    r.configure(rate);
    r.setSignalOffset(c.dialOffsetHz);
    std::vector<cf32> buf(8192);
    const size_t total = (size_t)((per * count + 0.002) * rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    r.flush();
    Result o;
    r.telemetry(o.t, 0);
    o.sent = count - (im.midStart ? 1 : 0);
    o.ok = o.t.blocksOk; o.bad = o.t.blocksBad; o.sigOk = o.t.sigOk;
    return o;
}

} // namespace sweep
