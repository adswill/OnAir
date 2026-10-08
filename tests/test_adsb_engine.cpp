// ADS-B through the engine, as the interface and dect2cli use it: the synthetic source plays the test airspace (8 bit samples),
// the engine must lock, decode, keep every sample, and report through latestRx(). The source plays faster than a radio (the receiver is what is
// tested, not real time) and the run is counted in seconds of signal.
#include "dect2/adsb_gen.h"
#include "dect2/engine.h"
#include "dect2/engine_testkit.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
using namespace dect2::enginetest;
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

static void run(double rate, double secs, bool checkRef) {
    Engine e;
    DeviceInfo dev;                         // the synthetic source
    TuneSettings t;
    t.centerHz = 1090e6;
    t.bandwidthMhz = 2;
    t.sampleRate = rate;
    t.synth.mode = 13;
    t.synth.pace = kFastPace;
    t.synth.snrDb = 30;
    t.synth.modeOpt[0] = 12;
    FileOptions fo;
    e.setStandard(13);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 12, "active standard %d", e.activeStandard());
    CHECK(e.sampleRate() > rate * 0.99 && e.sampleRate() < rate * 1.01, "engine sample rate %.0f", e.sampleRate());
    if (checkRef) e.adsb().setReference(25.25, 55.36);
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool grew = true, locked = false, seqOk = true;
    double firstLock = -1;
    // the run is counted by the receiver's own clock (the signal time of its last report), so the last report is the one at `secs`
    double now = 0;
    int reports = 0;
    const auto w0 = std::chrono::steady_clock::now();     // wall clock: only a guard against a hang
    while (now < secs && std::chrono::steady_clock::now() - w0 < std::chrono::seconds(120)) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            now = rx.adsb.timeSec;
            CHECK(rx.standard == 12, "report standard %d", rx.standard);
            if (rx.state == 2 && !locked) { locked = true; firstLock = now; }
            grew &= rx.blocksOk >= 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    e.latestRx(rx, 0);
    const AdsbTelemetry& a = rx.adsb;
    printf("  %.1f Msps: %d reports, locked after %.1f s: %s\n", rate / 1e6, reports, firstLock, adsbSummary(a).c_str());
    int withPos = 0, withName = 0;
    for (auto& ac : a.aircraft) { withPos += ac.hasPos; withName += !ac.callsign.empty(); }
    printf("    %u aircraft, %d with a position, %d with a callsign, noise %.1f dBFS, level %.1f dBFS, dropped samples %llu, max range %.0f NM\n", a.aircraftCount, withPos, withName, a.noiseDbfs, a.levelDbfs,
           (unsigned long long)e.droppedSamples(), a.maxRangeNm);
    CHECK(locked && firstLock < 4.0, "locked after %.1f s", firstLock);
    CHECK(seqOk && reports >= 4, "report numbers: %d reports, grew %d", reports, seqOk);
    CHECK(a.seq > secs * 2, "the receiver made %llu reports in %.0f s of signal", (unsigned long long)a.seq, secs);   // 4 a second of signal
    CHECK(rx.state == 2 && rx.dataValid && a.dataValid && a.state == 2, "state %d", rx.state);
    CHECK(a.aircraftCount >= 10 && a.aircraftCount <= 12, "%u aircraft in the table", a.aircraftCount);
    CHECK(withPos >= 9 && withName >= 9, "%d with a position, %d with a callsign", withPos, withName);
    CHECK(rx.blocksOk > (uint64_t)(secs * 40) && rx.blocksOk == a.blocksOk, "messages %llu", (unsigned long long)rx.blocksOk);
    // noise of this level rounded to 8 bits is a few single steps, which gives false preambles at 2 Msps (about 25 a second, see test_adsb_rx)
    CHECK(rx.blocksBad < (rate < 3e6 ? rx.blocksOk : rx.blocksOk / 5), "failed %llu against %llu good", (unsigned long long)rx.blocksBad, (unsigned long long)rx.blocksOk);
    CHECK(a.msgsPerSec > 30 && a.msgsPerSec < 200, "%.0f messages per second", a.msgsPerSec);
    CHECK(rx.dataSnrDb > 8 && rx.dataSnrDb < 60 && std::fabs(rx.dataSnrDb - a.snrDb) < 1e-3, "SNR %.1f", rx.dataSnrDb);
    CHECK(a.noiseDbfs < -35 && a.noiseDbfs > -70, "noise floor %.1f dBFS", a.noiseDbfs);
    CHECK(a.frames.size() == 64 && a.aircraft.size() <= 160, "frames %zu", a.frames.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    CHECK(rx.rateOk, "the engine says the rate is too low");
    if (checkRef) {
        CHECK(a.refValid && a.maxRangeNm > 100 && a.maxRangeNm < 230, "reference position and range: %d, %.0f NM", a.refValid, a.maxRangeNm);
        int ranged = 0;
        for (auto& ac : a.aircraft) if (ac.hasRange && ac.distNm < 230) ranged++;
        CHECK(ranged >= 9, "%d aircraft with a range", ranged);
    }
    // a retune empties the table, and the report numbers go on growing
    TuneSettings t2 = t;
    t2.centerHz = 1090.5e6;
    const uint64_t before = rx.seq;
    e.retuneReset(t2);
    waitFor(5.0, [&] { e.latestRx(rx, 0); return rx.seq > before; });
    CHECK(rx.seq > before, "report numbers after the retune: %llu after %llu", (unsigned long long)rx.seq, (unsigned long long)before);
    e.stop();
}

int main() {
    printf("engine, 4 Msps (the default of the mode), 12 s\n");
    run(4e6, 12, true);
    printf("engine, 2 Msps, 8 s\n");
    run(2e6, 8, false);
    // a sanitised build is about ten times slower than real time at 20 Msps, which this test (paced like a radio) cannot ask of it
    if (!SANITIZED) { printf("engine, 20 Msps, 8 s\n"); run(20e6, 8, false); }
    printf(fails ? "adsb engine: %d FAILED\n" : "adsb engine: all passed\n", fails);
    return fails ? 1 : 0;
}
