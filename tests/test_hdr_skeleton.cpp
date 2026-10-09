// HD Radio skeleton through the engine: the synthetic source plays the mode's test signal, the engine reports telemetry through latestRx()
// with the mode's standard. About 2 s; quick and silent.
#include "dect2/engine.h"
#include "dect2/hdr_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning mt = hdrTuning();
    CHECK(mt.stdMode == 23 && modeTuningById("hdr") && modeTuningById("hdr")->stdMode == 23, "tuning table");
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 23;
    FileOptions fo;
    e.setStandard(23);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 22, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 2.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            reports++;
            if (rx.standard != 22) wrong++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.stop();
    printf("HD Radio: %d reports in 2 s, last: %s\n", reports, hdrSummary(rx.hdr).c_str());
    CHECK(reports >= 3, "%d reports", reports);
    CHECK(wrong == 0, "%d reports with another standard", wrong);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(rx.hdr.inputRate > 0 && rx.hdr.levelDb > -60 && rx.hdr.levelDb < -10, "input %.0f, level %.1f dBFS", rx.hdr.inputRate, rx.hdr.levelDb);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
