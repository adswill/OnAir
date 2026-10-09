// ADS-B under real-radio faults (REAL_WORLD_CHECKLIST.md), through tests/impair.h rather than adsb_sim: 400 extended squitters 1 ms apart,
// rendered by AdsbMixer, then the faults, then the real receiver. 50 ppm at 1090 MHz is 55 kHz. Long messages (DF17, 112 us) must survive
// a +-100 ppm sample clock; back-to-back frames and the 2.4 Msps rate of an RTL-SDR are covered too.
#include "dect2/adsb_encode.h"
#include "dect2/adsb_gen.h"
#include "dect2/adsb_rx.h"
#include "impair.h"
#include <cstdio>
#include <functional>
using namespace dect2;
using V = std::vector<cf32>;
static int fails = 0;

static V render(double rate, int n, bool pairs) {
    AdsbMixer mx(rate);
    for (int k = 0; k < n; k++) {
        AdsbTx t;
        // pairs: two frames back to back (8 us of silence between them) every 2 ms, as a busy sky gives
        t.t = pairs ? 0.001 + 0.002 * (k / 2) + 128e-6 * (k % 2) : 0.001 + 0.001 * k + 1e-7 * (k % 7);
        t.frame = adsb::encodeIdentification(0x400000 + (uint32_t)(k % 24), 5, 4, 3, "TST" + std::to_string(100 + k % 900));
        t.amp = 0.3f;
        t.phase = 0.37f * (float)k;
        mx.add(t);
    }
    V x((size_t)(rate * (0.003 + 0.001 * n)));
    mx.render(x.data(), x.size());
    impair::noise(x, 30, 7);   // the radio's own noise: the receiver's thresholds are set over it
    return x;
}

static uint64_t run(const V& x, double rate) {
    AdsbReceiver rx;
    rx.configure(rate);
    AdsbTelemetry t; uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 65536) {
        rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        AdsbTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    V z((size_t)(rate * 0.05));
    for (int k = 0; k < 20; k++) { rx.feed(z.data(), z.size()); AdsbTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; } }
    return t.blocksOk;
}

int main() {
    const int n = 400;
    for (double rate : {2.0e6, 2.4e6}) {
        const std::vector<std::pair<const char*, std::function<void(V&)>>> cases = {
            {"carrier +55 kHz", [&](V& x) { impair::shift(x, 55e3, rate); }},
            {"carrier -55 kHz", [&](V& x) { impair::shift(x, -55e3, rate); }},
            {"clock +100 ppm", [](V& x) { x = impair::clock(x, 100); }},
            {"clock -100 ppm", [](V& x) { x = impair::clock(x, -100); }},
            {"I/Q swapped", [](V& x) { impair::swapIq(x); }},
            {"combined: -55 kHz, +80 ppm, echo, 8-bit clipped", [&](V& x) { impair::shift(x, -55e3, rate); x = impair::clock(x, 80); impair::echo(x, 1, -8, 2.0); impair::noise(x, 25); impair::clip8(x, 2.5); }},
        };
        for (bool pairs : {false, true}) {
            const V base = render(rate, n, pairs);
            for (const auto& [name, f] : cases) {
                V x = base; f(x);
                const uint64_t ok = run(x, rate);
                printf("%.1f Msps, %s, %-48s %llu of %d\n", rate / 1e6, pairs ? "back to back" : "1 ms apart  ", name, (unsigned long long)ok, n);
                if (ok < (uint64_t)n) { printf("FAIL: %s\n", name); fails++; }
            }
        }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
