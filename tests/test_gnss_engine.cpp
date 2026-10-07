// GNSS through the engine as the interface and dect2cli use it: the synthetic source plays the simulated sky (8 bit samples, paced twice as fast as a
// radio), the engine must lock, decode, keep every sample, find the position and report through latestRx().
#include "dect2/engine.h"
#include "dect2/gnss_gen.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
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
    const double rate = 4e6;
    // faster than a radio when it can, slower under a sanitiser; real time on the shared CI machines, which run several tests at once
    const double pace = SANITIZED ? 0.2 : std::getenv("CI") ? 1.0 : 2.0;
    const double signalSecs = SANITIZED ? 6 : 36;            // the fix comes after 26 s
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = 1575.42e6;
    t.bandwidthMhz = 2.046;
    t.sampleRate = rate;
    t.synth.mode = 14;
    t.synth.pace = pace;
    t.synth.modeVal[3] = 44;
    FileOptions fo;
    e.setStandard(14);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 13, "active standard %d", e.activeStandard());
    CHECK(std::fabs(e.sampleRate() - rate) < rate * 0.01, "engine sample rate %.0f", e.sampleRate());
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true, tracked = false, fixed = false;
    double firstTrack = -1, firstFix = -1;
    int reports = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const double wall = signalSecs / pace;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < wall) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            CHECK(rx.standard == 13, "report standard %d", rx.standard);
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * pace;
            if (rx.state >= 1 && !tracked) { tracked = true; firstTrack = now; }
            if (rx.state == 2 && !fixed) { fixed = true; firstFix = now; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.latestRx(rx, 0);
    const GnssTelemetry& g = rx.gnss;
    printf("engine, 4 Msps, %.0f s of signal (pace %.1f): %d reports, first satellite tracked after %.1f s, fix after %.1f s of signal\n  %s\n  dropped samples %llu\n", signalSecs, pace, reports, firstTrack, firstFix,
           gnssSummary(g).c_str(), (unsigned long long)e.droppedSamples());
    CHECK(tracked && firstTrack < 8.0 * (pace > 1 ? 1.0 : 5.0), "first satellite after %.1f s", firstTrack);
    CHECK(seqOk && reports > wall * 2, "report numbers: %d reports, grew %d", reports, seqOk);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    CHECK(g.nTracked >= 5 && rx.dataSnrDb > 38 && rx.dataSnrDb < 46 && std::fabs(rx.dataSnrDb - g.snrDb) < 1e-3, "tracked %d, best C/N0 %.1f", g.nTracked, rx.dataSnrDb);
    CHECK(g.blocksBad == 0 && rx.blocksBad == g.blocksBad && rx.blocksOk == g.blocksOk, "subframes %llu good, %llu bad", (unsigned long long)g.blocksOk, (unsigned long long)g.blocksBad);
    if (!SANITIZED) {
        CHECK(fixed && rx.state == 2 && rx.dataValid && g.dataValid && g.fix.valid, "no fix");
        // the simulated receiver is in Dubai: the fix is near 25.2 N 55.36 E
        CHECK(std::fabs(g.fix.latDeg - 25.2) < 5e-4 && std::fabs(g.fix.lonDeg - 55.36) < 5e-4, "position %.5f %.5f", g.fix.latDeg, g.fix.lonDeg);
        CHECK(g.fix.nSats >= 6 && g.fix.hdop > 1 && g.fix.hdop < 4, "%d satellites, HDOP %.2f", g.fix.nSats, g.fix.hdop);
        CHECK(firstFix > 22 && firstFix < 36, "fix after %.1f s", firstFix);
    }
    CHECK(g.inputRate == rate && std::fabs(g.centerMhz - 1575.42) < 1e-9, "set-up fields %.0f %.3f", g.inputRate, g.centerMhz);
    // a retune empties the tables, and the report numbers go on growing
    TuneSettings t2 = t;
    t2.centerHz = 1575.5e6;
    const uint64_t before = rx.seq;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    e.latestRx(rx, 0);
    CHECK(rx.seq > before, "report numbers after the retune: %llu after %llu", (unsigned long long)rx.seq, (unsigned long long)before);
    e.stop();
    printf(fails ? "gnss engine: %d FAILED\n" : "gnss engine: all passed\n", fails);
    return fails ? 1 : 0;
}
