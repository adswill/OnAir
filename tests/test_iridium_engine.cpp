// Iridium through the engine as the interface and dect2cli use it: the synthetic source plays the test signal at 10 Msps (8-bit
// samples, in real time), the engine must decode the satellites, ring alerts, time and pager messages, keep every sample and
// report through latestRx().
#include "dect2/engine.h"
#include "dect2/iridium_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rate = SANITIZED ? 4e6 : 10e6;          // the sanitised build is too slow for 10 Msps in real time
    const double pace = SANITIZED ? 0.25 : 1.0;
    const double signalSecs = SANITIZED ? 4 : 15;
    Engine e;
    DeviceInfo dev;                                      // the synthetic source
    TuneSettings t;
    t.centerHz = 1622e6;
    t.bandwidthMhz = 10.5;
    t.sampleRate = rate;
    t.synth.mode = 21;
    t.synth.pace = pace;
    t.synth.snrDb = 25;
    t.synth.modeVal[1] = SANITIZED ? 1626.25 : 0;       // 4 Msps: the simplex channels
    FileOptions fo;
    e.setStandard(21);
    CHECK(e.start(dev, t, fo), "engine start");
    if (SANITIZED) e.iridium().setCenterMhz(1626.25);
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true;
    int reports = 0;
    double firstDecode = -1;
    const auto t0 = std::chrono::steady_clock::now();
    const double wall = signalSecs / pace;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < wall) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            CHECK(rx.standard == 20, "report standard %d", rx.standard);
            if (rx.state == 2 && firstDecode < 0) firstDecode = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * pace;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.latestRx(rx, 0);
    const IridiumTelemetry& g = rx.iridium;
    printf("engine, %.0f Msps, %.0f s of signal (pace %.2f): %d reports, decoding after %.1f s\n  %s\n  %llu bursts, %llu unique words, %llu dropped bursts, %llu frames ok, "
           "%llu failed, %zu satellites, %zu messages, dropped samples %llu\n", rate / 1e6, signalSecs, pace, reports, firstDecode, iridiumSummary(g).c_str(),
           (unsigned long long)g.bursts, (unsigned long long)g.uwOk, (unsigned long long)g.dropped, (unsigned long long)g.blocksOk, (unsigned long long)g.blocksBad,
           g.sats.size(), g.messages.size(), (unsigned long long)e.droppedSamples());
    CHECK(firstDecode >= 0 && firstDecode < 2, "decoding after %.1f s", firstDecode);
    CHECK(seqOk && reports > signalSecs * 2, "report numbers: %d reports, grew %d", reports, seqOk);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(e.droppedSamples() == 0 && g.dropped == 0, "dropped %llu samples, %llu bursts", (unsigned long long)e.droppedSamples(), (unsigned long long)g.dropped);
    CHECK(rx.blocksOk == g.blocksOk && rx.blocksBad == g.blocksBad, "copied counters");
    CHECK(g.blocksBad <= g.blocksOk / 200 && g.blocksOk > signalSecs * (SANITIZED ? 15 : 80), "frames %llu good, %llu bad", (unsigned long long)g.blocksOk, (unsigned long long)g.blocksBad);
    CHECK(g.state == 2 && g.dataValid && g.sats.size() >= 2, "state %d, %zu satellites", g.state, g.sats.size());
    CHECK(g.messages.size() >= (SANITIZED ? 1u : 4u), "%zu pager messages", g.messages.size());
    if (!SANITIZED) CHECK(g.hasTime && std::fabs(g.iridiumUtc - (kIridiumSynthEpoch + g.timeSec)) < 0.5, "Iridium time %.2f s after the epoch at %.2f s", g.iridiumUtc - kIridiumSynthEpoch, g.timeSec);
    CHECK(g.inputRate == rate && g.positions.size() > 20 && !g.scatter.empty(), "rate %.0f, %zu positions, %zu dots", g.inputRate, g.positions.size(), g.scatter.size());
    // a retune empties the tables; the report numbers go on growing
    TuneSettings t2 = t;
    t2.centerHz = 1621.5e6;
    const uint64_t before = rx.seq;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(600 / pace)));
    e.latestRx(rx, 0);
    CHECK(rx.seq > before && rx.iridium.timeSec < 1.0, "after the retune: report %llu after %llu, time %.2f", (unsigned long long)rx.seq, (unsigned long long)before, rx.iridium.timeSec);
    e.stop();
    printf(fails ? "iridium engine: %d FAILED\n" : "iridium engine: all passed\n", fails);
    return fails ? 1 : 0;
}
