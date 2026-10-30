// PSK demodulator: the channel bandwidth set by hand (2.5 - 200 kHz, #308) cuts one signal out of several close together
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/pskdsp/pskdsp_tel.h"
#include "dect2/pskdsp/pskdsp.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

// the wanted signal sits on the dial; two stronger neighbours of the same kind sit `gapHz` either side (adjacent channels)
static std::vector<pskd::cf> threeSignals(double fs, double baud, double gapHz, double seconds) {
    auto mk = [&](double off, uint32_t seed, double esn0) {
        pskd::GenSpec g; g.fs = fs; g.baud = baud; g.order = pskd::Order::Bpsk; g.offsetHz = off; g.esn0Db = esn0; g.seed = seed;
        g.sync = pskd::parseBitsOrHex("1ACFFC1D"); g.payloadBits = 992 - 32;
        g.frames = (int)std::ceil(seconds * baud / 992.0) + 2;
        return pskd::generate(g);
    };
    auto w = mk(0, 3, 24), l = mk(-gapHz, 5, 40), r = mk(gapHz, 7, 40);
    const size_t n = std::min({w.size(), l.size(), r.size()});
    w.resize(n);
    for (size_t i = 0; i < n; i++) w[i] += 3.0f * l[i] + 3.0f * r[i];
    return w;
}

static PskdTelemetry run(const std::vector<pskd::cf>& x, double fs, double spanHz) {
    PskdReceiver rx;
    rx.configure(fs);
    rx.setSignalOffset(0);
    PskdSettings s; s.sync = "1ACFFC1D"; s.spanHz = spanHz;
    rx.setSettings(s);
    PskdTelemetry t; uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 4096) {
        const size_t n = std::min<size_t>(4096, x.size() - i);
        std::vector<cf32> buf(x.begin() + (long)i, x.begin() + (long)(i + n));
        rx.feed(buf.data(), n);
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    return t;
}

int main() {
    const double fs = 250000;
    // 1200 Bd BPSK on the dial between two 12 dB stronger neighbours 3 kHz away (their skirts touch it): automatic span sees one blob, a manual 3 kHz isolates it
    {
        auto x = threeSignals(fs, 1200, 3000, 16);
        PskdTelemetry t = run(x, fs, 3000);
        printf("3 kHz: state %d baud %.1f order %s span %.1f kHz cfo %+.1f\n", t.state, t.baud, pskdOrderName(t.order), t.spanHz / 1e3, t.cfoHz);
        CHECK(std::fabs(t.spanHz - 3000) < 1, "span %.1f", t.spanHz);
        CHECK(t.state == 2, "not locked");
        CHECK(std::fabs(t.baud - 1200) < 6, "baud %.1f", t.baud);
        CHECK(std::fabs(t.cfoHz) < 60, "carrier %+.1f: not the wanted channel", t.cfoHz);
        CHECK(t.syncs >= 3, "%llu syncs", (unsigned long long)t.syncs);
        PskdTelemetry t0 = run(x, fs, 0);
        printf("auto:    state %d baud %.1f cfo %+.1f syncs %llu (not asserted)\n", t0.state, t0.baud, t0.cfoHz, (unsigned long long)t0.syncs);
    }
    // no limits (the owner's rule): a span below 2.5 kHz is used as asked, not raised; 200 kHz is the automatic value
    {
        auto x = threeSignals(fs, 1200, 6000, 6);
        PskdTelemetry t = run(x, fs, 1000);
        CHECK(std::fabs(t.spanHz - 1000) < 1, "span %.1f", t.spanHz);
        PskdTelemetry t25 = run(x, fs, 2500);
        CHECK(std::fabs(t25.spanHz - 2500) < 1, "span %.1f", t25.spanHz);
        CHECK(t25.state == 2, "not locked at 2.5 kHz");
        PskdTelemetry t1 = run(x, fs, 200000);
        CHECK(std::fabs(t1.spanHz - 200000) < 1, "span %.1f", t1.spanHz);
        PskdTelemetry t0 = run(x, fs, 0);
        CHECK(std::fabs(t0.spanHz - 200000) < 1, "auto span %.1f", t0.spanHz);
    }
    return testSummary("pskdsp_span");
}
