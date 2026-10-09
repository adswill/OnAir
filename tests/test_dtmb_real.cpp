// DTMB against real-world faults (REAL_WORLD_CHECKLIST.md) applied with tests/impair.h, written apart from the receiver and the test kit:
// a UHF radio 50 ppm off (+-43 kHz at 860 MHz), a sample clock 80 ppm off, and everything at once with an echo and an overdriven 8-bit radio.
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_rx.h"
#include "impair.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::dtmb;
static int fails = 0;

struct Case { const char* name; Header h; Mapping m; bool c1; double offHz, ppm; bool echo, clip, nan; };

static uint64_t run(const Case& c) {
    SignalConfig sc;
    sc.rate = 10e6; sc.snrDb = 200; sc.tx.header = c.h; sc.tx.profile.map = c.m; sc.tx.profile.rate = Rate::R06;
    sc.tx.phaseRotate = c.h != Header::Pn595;
    if (c.c1) sc.tx.carriers = 1;
    Signal sig(sc, testPacketSource(1));
    std::vector<cf32> x((size_t)(1.6 * sc.rate));
    sig.generate(x.data(), x.size());
    if (c.echo) impair::echo(x, 60, -6.0, 2.0);
    if (c.ppm != 0) x = impair::clock(x, c.ppm);
    impair::shift(x, c.offHz, sc.rate);
    impair::noise(x, 28.0, 5);
    if (c.clip) impair::clip8(x, 3.0);
    if (c.nan) for (size_t i = x.size() / 3; i < x.size() / 3 + 64; i++) x[i] = cf32(NAN, INFINITY);
    DtmbReceiver rx;
    rx.configure(sc.rate, 8);
    rx.setDecoderThreads(0);
    uint64_t good = 0, wrong = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { for (size_t i = 0; i < n; i++) { uint32_t k = 0xFFFFFFFFu; if (checkTestPacket(p + i * 188, 1, &k)) good++; else wrong++; } });
    for (size_t i = 0; i < x.size(); i += 16384) rx.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
    rx.flush();
    DtmbTelemetry t; rx.telemetry(t, 0);
    const bool ok = good > 10000 && wrong == 0;
    printf("  %-48s good %6llu wrong %llu cfo %.0f Hz clock %.1f ppm  %s\n", c.name, (unsigned long long)good, (unsigned long long)wrong, t.cfoHz, t.clockPpm, ok ? "OK" : "FAILED");
    if (!ok) fails++;
    return good;
}

int main() {
    const Case cases[] = {
        {"PN945 16QAM, +43 kHz", Header::Pn945, Mapping::Qam16, false, 43e3, 0, false, false, false},
        {"PN420 16QAM, -43 kHz", Header::Pn420, Mapping::Qam16, false, -43e3, 0, false, false, false},
        {"PN595 16QAM, +38 kHz", Header::Pn595, Mapping::Qam16, false, 38e3, 0, false, false, false},
        {"PN945 16QAM, clock +80 ppm", Header::Pn945, Mapping::Qam16, false, 0, 80, false, false, false},
        {"PN420 16QAM, NaN samples", Header::Pn420, Mapping::Qam16, false, 0, 0, false, false, true},
        {"PN945 16QAM, +43 kHz +80 ppm echo clip8", Header::Pn945, Mapping::Qam16, false, 43e3, 80, true, true, false},
    };
    for (const Case& c : cases) run(c);
    printf(fails ? "dtmb_real: %d FAILED\n" : "dtmb_real: all passed\n", fails);
    return fails ? 1 : 0;
}
