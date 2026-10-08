// AIS: the tuning entry exists, and the receiver reports at a steady rate whatever the chunk size.
#include "dect2/modes.h"
#include "dect2/ais_rx.h"
#include <cstdio>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(size_t chunk) {
    AisReceiver rx;
    rx.configure(2e6);
    CHECK(rx.ready(), "ready at 2 Msps");
    std::vector<cf32> z(4096, cf32(0, 0));
    AisTelemetry t; uint64_t last = 0;
    size_t left = 2000000, got = 0;
    while (left) {
        const size_t n = left < chunk ? left : chunk;
        rx.feed(z.data(), n);
        left -= n;
        if (rx.telemetry(t, last)) { CHECK(t.seq > last, "seq grows"); CHECK(t.state == 0, "state 0"); last = t.seq; got++; }
    }
    CHECK(got >= 3, "reports arrive (chunk %zu): %zu", chunk, got);
    const uint64_t before = last;
    rx.reset();
    left = 1000000;
    while (left) { const size_t n = left < chunk ? left : chunk; rx.feed(z.data(), n); left -= n; }
    CHECK(rx.telemetry(t, before) && t.seq > before, "seq keeps growing after reset (chunk %zu)", chunk);
}

int main() {
    const ModeTuning* mt = modeTuningById("ais");
    CHECK(mt != nullptr, "modeTuningById");
    if (mt) CHECK(mt->stdMode == 16, "stdMode %d", mt->stdMode);
    run(1 << 12); run(7);
    { // chunk of 1 sample: 2 million calls
        run(1);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
