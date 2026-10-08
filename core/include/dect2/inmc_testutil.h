// Helpers of the Inmarsat-C tests: play the test signal into the receiver with the impairments the tests need.
#pragma once
#include "inmc_gen.h"
#include "inmc_rx.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

struct InmcRunOpts {
    InmcGenConfig gen;
    double secs = 30;
    size_t chunk = 4096;
    bool quantize8 = false;       // round to 8 bits like a HackRF
    float dc = 0;                 // DC offset added to both components (fraction of full scale, before quantising)
    double gapAt = -1, gapLen = 0;   // seconds: replace this stretch by zeros
    double resetAt = -1;          // call reset() at this time
    double flipAt = -1;           // from this time on the signal is negated (a cycle slip of the carrier loop: the polarity flips inside a frame)
    double flipBack = -1;         // negated again at this time
    double offsetHz = -50000;     // what the receiver is told
};

struct InmcRunResult {
    InmcTelemetry t;
    double firstSyncSec = -1;     // when the first good bulletin board was seen
    double wallSec = 0;           // CPU time spent in feed()
    double rtf = 0;               // signal seconds per CPU second in feed()
    int completeMatches = 0;      // test messages received complete and equal
    int partial = 0;
};

inline int inmcCountMatches(const InmcTelemetry& t, const std::vector<InmcTestMessage>& truth) {
    int n = 0;
    for (const auto& m : truth) {
        std::string want;                      // the receiver drops carriage returns
        for (char c : m.text) if (c != '\r') want += c;
        for (const auto& r : t.messages)
            if (r.complete && r.id == m.id && r.serviceCode == m.service && r.priority == m.priority && r.text == want) { n++; break; }
    }
    return n;
}

inline InmcRunResult runInmc(const InmcRunOpts& o) {
    InmcRunResult res;
    auto g = makeInmcGenerator(o.gen);
    InmcReceiver rx;
    rx.configure(o.gen.rate);
    rx.setSignalOffset(o.offsetHz);
    std::vector<cf32> buf(o.chunk);
    const size_t total = (size_t)(o.secs * o.gen.rate);
    size_t done = 0;
    uint64_t seq = 0;
    bool didReset = false;
    double wall = 0;
    InmcTelemetry t;
    while (done < total) {
        const size_t n = std::min(o.chunk, total - done);
        g->generate(buf.data(), n);
        for (size_t k = 0; k < n; k++) {
            const double ts = (double)(done + k) / o.gen.rate;
            cf32 v = buf[k] + cf32(o.dc, o.dc);
            if (o.quantize8) v = cf32(std::round(v.real() * 127.f) / 127.f, std::round(v.imag() * 127.f) / 127.f);
            if (o.gapAt >= 0 && ts >= o.gapAt && ts < o.gapAt + o.gapLen) v = cf32(0, 0);
            if (o.flipAt >= 0 && ts >= o.flipAt && (o.flipBack < 0 || ts < o.flipBack)) v = -v;
            buf[k] = v;
        }
        if (o.resetAt >= 0 && !didReset && (double)done / o.gen.rate >= o.resetAt) { rx.reset(); didReset = true; }
        const std::clock_t t0 = std::clock();      // CPU time, so that other programs on the machine do not distort it
        rx.feed(buf.data(), n);
        wall += (double)(std::clock() - t0) / CLOCKS_PER_SEC;
        done += n;
        if (res.firstSyncSec < 0 && rx.telemetry(t, seq)) {
            seq = t.seq;
            if (t.blocksOk > 0) res.firstSyncSec = (double)done / o.gen.rate;
        }
    }
    rx.telemetry(res.t, 0);
    res.wallSec = wall;
    res.rtf = wall > 0 ? o.secs / wall : 0;
    res.completeMatches = inmcCountMatches(res.t, g->source().messages());
    for (const auto& m : res.t.messages) if (!m.complete) res.partial++;
    return res;
}

} // namespace dect2
