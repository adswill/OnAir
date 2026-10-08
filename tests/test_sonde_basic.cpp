// Radiosonde: the tuning entry, and the receiver on silence and noise: it reports at a steady rate whatever the chunk size and mode,
// finds nothing, survives reset(), and stops its thread cleanly.
#include "dect2/modes.h"
#include "dect2/sonde_rx.h"
#include "dect2/gen_util.h"
#include <cstdio>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(size_t chunk, bool sync, bool noise) {
    SondeReceiver rx;
    rx.setSynchronous(sync);
    rx.configure(2e6);
    CHECK(rx.ready(), "ready at 2 Msps");
    std::vector<cf32> z(chunk, cf32(0, 0));
    genutil::NoiseSource ns(5);
    SondeTelemetry t; uint64_t last = 0;
    size_t left = 2000000, got = 0;
    while (left) {
        const size_t n = left < chunk ? left : chunk;
        if (noise) { for (auto& v : z) v = cf32(0, 0); ns.add(z.data(), n, 0.2f); }
        rx.feed(z.data(), n);
        left -= n;
        if (!sync && (left % 131072) < n) rx.flush();       // the radio paces the samples; here we keep the queue short
        if (sync && rx.telemetry(t, last)) { CHECK(t.seq > last, "seq grows"); last = t.seq; got++; }
    }
    if (!sync) { rx.flush(); if (rx.telemetry(t, 0)) { last = t.seq; got = (size_t)t.seq; } }
    CHECK(got >= 3, "reports arrive (chunk %zu sync %d): %zu", chunk, (int)sync, got);
    if (rx.telemetry(t, 0) || got) {
        rx.telemetry(t, 0);
        CHECK(t.state == 0 && t.sondes.empty() && t.carriers.empty(), "nothing found in %s: state %d sondes %zu carriers %zu", noise ? "noise" : "silence", t.state, t.sondes.size(), t.carriers.size());
        CHECK(t.bandHz == 2e6, "band %.0f", t.bandHz);
    }
    const uint64_t before = last;
    rx.reset();
    left = 1000000;
    while (left) { const size_t n = left < chunk ? left : chunk; rx.feed(z.data(), n); left -= n; if (!sync && (left % 131072) < n) rx.flush(); }
    rx.flush();
    CHECK(rx.telemetry(t, before) && t.seq > before, "seq keeps growing after reset (chunk %zu sync %d)", chunk, (int)sync);
    CHECK(rx.droppedSamples() == 0, "dropped %llu", (unsigned long long)rx.droppedSamples());
}

int main() {
    const ModeTuning* mt = modeTuningById("sonde");
    CHECK(mt != nullptr, "modeTuningById");
    if (mt) {
        CHECK(mt->stdMode == 15, "stdMode %d", mt->stdMode);
        CHECK(mt->minSampleRate <= 2e6 && mt->sampleRate >= 2e6, "rates");
    }
    run(1 << 12, false, false); run(1 << 12, true, true); run(7, true, false); run(7, false, true); run(65536, false, true);
    run(1, true, false);
    { SondeReceiver r; r.configure(1e6); CHECK(!r.ready(), "not ready at 1 Msps"); }
    { SondeReceiver r; /* destroyed before configure / feed: no thread to stop */ }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
