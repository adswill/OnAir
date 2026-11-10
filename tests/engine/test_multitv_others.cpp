// Whole span TV for the raster standards other than DVB-T/T2: two channels of one standard from one capture, each with its own receiver. Simulated (the mode generators).
#include "dect2/engine/multitv.h"
#include "dect2/engine/mode_synth.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Case { const char* name; int stdMode, recv; double widthMhz; double secs; };

int main() {
    const Case cases[] = {{"DVB-C", 42, 41, 8, 3}, {"DTMB", 9, 8, 8, 5}, {"ISDB-T", 6, 5, 6, 5}, {"ATSC", 3, 2, 6, 4}};
    for (const Case& c : cases) {
        const double width = c.widthMhz * 1e6, rate = c.stdMode == 3 ? 2.0 * width + 1.0e6 : 2.5 * width;   // the ATSC generator stops at 13 Msps
        SynthConfig sc; sc.snrDb = 32;
        auto a = makeModeSynth(c.stdMode, sc, rate);
        auto b = makeModeSynth(c.stdMode, sc, rate);
        if (!a || !b) { printf("%s: no generator at %.1f Msps\n", c.name, rate / 1e6); fails++; continue; }
        MultiTvOptions o; o.otherStd = c.recv; o.spanRate = rate; o.widthMhz = c.widthMhz; o.gridOffsetHz = 0.5 * width; o.maxChannels = 4; o.blocking = true; o.thresholdDb = 6;
        MultiTv m; CHECK(m.configure(o), "%s configure", c.name);
        const size_t chunk = 1 << 18;
        std::vector<cf32> x(chunk), y(chunk), s(chunk);
        const double wa = 2 * M_PI * (-0.5 * width) / rate, wb = 2 * M_PI * (0.5 * width) / rate;
        uint64_t n = 0; const uint64_t total = (uint64_t)(c.secs * rate);
        RxTelemetry t;
        while (n < total) {
            a->generate(x.data(), chunk); b->generate(y.data(), chunk);
            for (size_t i = 0; i < chunk; i++) { const double k = (double)(n + i); s[i] = x[i] * cf32((float)std::cos(wa * k), (float)std::sin(wa * k)) + y[i] * cf32((float)std::cos(wb * k), (float)std::sin(wb * k)); }
            m.feed(s.data(), chunk); n += chunk; m.poll(t);
        }
        m.wait(); m.poll(t);
        auto rows = m.channels();
        int occupied = 0, locked = 0;
        for (const auto& r : rows) { if (r.occupied) occupied++; if (r.locked) locked++; printf("  %s slot %d %+.1f MHz above %.0f dB state %d locked %d SNR %.1f\n", c.name, r.slot, r.offsetHz / 1e6, r.aboveDb, r.state, r.locked, r.snrDb); }
        CHECK(occupied == 2, "%s: occupied %d", c.name, occupied);
        CHECK(locked == 2, "%s: locked %d", c.name, locked);
        m.reset();
    }
    return testSummary("multitv_others");
}
