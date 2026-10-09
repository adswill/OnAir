// Pagers through the engine, as the interface and dect2cli use it: the synthetic source plays the test signal (8 bit samples, paced like a radio) at
// 2 Msps and 15 dB, the engine reports through latestRx() with the mode's standard, and every page of a cycle must arrive with its exact text.
// About 20 s: the test signal needs that long for one cycle.
#include "dect2/engine.h"
#include "dect2/pager_gen.h"
#include "dect2/pager_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning mt = pagerTuning();
    CHECK(mt.stdMode == 25 && modeTuningById("pager") && modeTuningById("pager")->stdMode == 25, "tuning table");
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 25;
    t.synth.snrDb = 15;
    t.synth.cfoHz = 500;
    FileOptions fo;
    e.setStandard(25);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 24, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const double secs = pagerCycleSeconds() + 3.0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            reports++;
            if (rx.standard != 24) wrong++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.latestRx(rx, 0);
    e.stop();
    printf("Pagers: %d reports in %.0f s, last: %s\n", reports, secs, pagerSummary(rx.pager).c_str());
    CHECK(reports >= secs * 2, "%d reports", reports);
    CHECK(wrong == 0, "%d reports with another standard", wrong);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(rx.dataValid && rx.blocksOk > 500, "code words %llu", (unsigned long long)rx.blocksOk);
    const std::vector<PagerMessage> none;
    const std::vector<PagerMessage>& got = rx.pager.messages ? *rx.pager.messages : none;
    const auto want = pagerTestMessages();
    int found = 0;
    for (const auto& w : want) {
        bool ok = false;
        for (const auto& m : got) if (m.speed == w.speed && m.address == w.address && m.type == w.type && m.function == w.function && m.text == w.text) { ok = true; break; }
        CHECK(ok, "missing %s address %u '%s'", pagerSpeedName(w.speed), w.address, w.text.c_str());
        found += ok;
    }
    printf("  %d of %zu pages, %zu listed\n", found, want.size(), got.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
