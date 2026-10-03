// T2-Lite and MISO signalling: the receiver must lock on a T2-Lite SISO signal, and flag a MISO signal as unsupported.
#include "dect2/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdint>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static RxTelemetry run(int s1, double secs) {
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.synth.snrDb = 30;
    t.synth.tx.s1 = s1;
    t.synth.tx.s2field1 = 1;
    t.synth.tx.giIdx = 3;
    FileOptions fo;
    e.start(dev, t, fo);
    RxTelemetry rx; uint64_t seq = 0, best = 0;
    RxTelemetry last;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.l1preOk) { last = rx; best = seq; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.stop();
    (void)best;
    return last;
}

int main() {
    RxTelemetry sa = run(0, 7);
    printf("T2-Base SISO: L1-pre ok %d, s1 %d, unsupported %zu\n", sa.l1preOk, sa.l1pre.s1, sa.unsupported.size());
    CHECK(sa.l1preOk && sa.unsupported.empty(), "base SISO should lock with nothing flagged");
    RxTelemetry lite = run(3, 7);
    printf("T2-Lite SISO: L1-pre ok %d, s1 %d, unsupported %zu\n", lite.l1preOk, lite.l1pre.s1, lite.unsupported.size());
    CHECK(lite.l1preOk && lite.l1pre.s1 == 3, "T2-Lite SISO did not lock / wrong S1 (%d)", lite.l1pre.s1);
    CHECK(lite.unsupported.empty(), "T2-Lite SISO flagged as unsupported");
    RxTelemetry miso = run(1, 7);
    printf("T2-Base MISO: L1-pre ok %d, s1 %d, unsupported %zu\n", miso.l1preOk, miso.l1pre.s1, miso.unsupported.size());
    for (auto& u : miso.unsupported) printf("   ! %s\n", u.c_str());
    CHECK(miso.l1preOk ? !miso.unsupported.empty() : true, "MISO must be flagged when L1-pre decodes");
    printf(fails ? "mode tests FAILED\n" : "mode tests passed\n");
    return fails ? 1 : 0;
}
