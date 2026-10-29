// Wi-SUN FAN through the engine (setStandard(86)): the synthetic source plays the test schedule at 20 Msps, the receiver reports through latestRx().
// No sound.
#include "dect2/wisun/wisun_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
// The message is glued to a literal: a format that is not a string literal does not compile
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

int main() {
    const ModeTuning mt = wisunTuning();
    CHECK(mt.stdMode == 86 && modeTuningById("wisun") && modeTuningById("wisun")->stdMode == 86, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 86;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(86);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 85, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    // 5 s, and on a slow or busy machine (the synthetic source then makes less signal in that time) on until the results below are in,
    // at most 20 s
    const auto done = [&] { return rx.seq > 10 && rx.wisun.nodes.size() == 3 && rx.wisun.blocksOk > 100 && rx.wisun.ppsHist.size() >= 3; };
    const auto t0 = std::chrono::steady_clock::now();
    // a while with elapsed() instead of a for that updates the time in its step: Cppcheck called that for condition always false
    // (knownConditionTrueFalse), a false positive, and Codacy then fails the PR
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    while (elapsed() < 5.0 || (elapsed() < 20.0 && !done())) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 85) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const WisunTelemetry& d = rx.wisun;
    printf("  %d reports: %s, %llu bad, %llu dropped, load %.0f%%, %.1f - %.1f MHz\n", reports, wisunSummary(d).c_str(), (unsigned long long)d.blocksBad,
           (unsigned long long)d.dropped, d.loadPct, d.loMhz, d.hiMhz);
    // reports made by the engine (its sequence number), not the polls that happened to see one: a fast or slow runner polls a different share of them
    CHECK(rx.seq > 10 && wrong == 0, "%llu reports made (%d seen), %d wrong", (unsigned long long)rx.seq, reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.region == "ANZ" && d.loMhz < 915.5 && d.hiMhz > 927.5, "coverage %s %.1f - %.1f", d.region.c_str(), d.loMhz, d.hiMhz);
    CHECK(d.pans.size() == 1 && d.nodes.size() == 3 && d.blocksOk > 100 && d.blocksBad == 0, "%zu PANs, %zu nodes, %llu frames", d.pans.size(), d.nodes.size(),
          (unsigned long long)d.blocksOk);
    CHECK(!d.pans.empty() && d.pans[0].netName == "OnAir-Test-FAN", "network name");
    CHECK(d.ppsHist.size() >= 3 && d.pps > 10, "pps %.0f, %zu history", d.pps, d.ppsHist.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.wisun().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    if (fails) printf("wisun engine: %d FAILED\n", fails);
    else printf("wisun engine: all passed\n");
    return fails ? 1 : 0;
}
