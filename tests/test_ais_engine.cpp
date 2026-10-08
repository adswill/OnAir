// AIS through the engine, as the interface and dect2cli use it: the synthetic source plays the test port (8 bit samples, faster than a radio: the receiver
// is tested, not real time, and the run is counted in seconds of signal), the engine must decode, keep every sample and report through latestRx().
#include "dect2/ais_gen.h"
#include "dect2/engine.h"
#include "dect2/engine_testkit.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
using namespace dect2::enginetest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(double rate, double secs) {
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 162.0e6;
    t.bandwidthMhz = 0.1;
    t.sampleRate = rate;
    t.synth.mode = 16;
    t.synth.pace = kFastPace;
    t.synth.snrDb = 34;
    FileOptions fo;
    e.setStandard(16);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 15, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true, decoding = false;
    double firstDecode = -1;
    int reports = 0;
    // the run is counted by the receiver's own clock (the signal time of its last report), so the last report is the one at `secs`
    double now = 0;
    const auto w0 = std::chrono::steady_clock::now();     // wall clock: only a guard against a hang
    while (now < secs && std::chrono::steady_clock::now() - w0 < std::chrono::seconds(120)) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            now = rx.ais.timeSec;
            CHECK(rx.standard == 15, "report standard %d", rx.standard);
            if (rx.state == 2 && !decoding) { decoding = true; firstDecode = now; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    e.latestRx(rx, 0);
    const AisTelemetry& a = rx.ais;
    int named = 0, positioned = 0, tracks = 0;
    for (const AisVessel& v : a.vessels) { named += !v.name.empty(); positioned += v.hasPos; tracks += v.track.size() > 1; }
    printf("  %.1f Msps: %d reports, decoding after %.1f s: %s\n", rate / 1e6, reports, firstDecode, aisSummary(a).c_str());
    printf("    %d named, %d with a position, %d with a track, per minute %.0f / %.0f, snr %.1f dB, noise %.1f dBFS, dropped samples %llu\n", named, positioned, tracks,
           a.burstsPerMin[0], a.burstsPerMin[1], a.snrDb, a.noiseDbfs[0], (unsigned long long)e.droppedSamples());
    CHECK(decoding && firstDecode < 3.0, "decoding after %.1f s", firstDecode);
    CHECK(seqOk && reports >= 4, "report numbers: %d reports", reports);
    CHECK(a.seq > secs * 2, "the receiver made %llu reports in %.0f s of signal", (unsigned long long)a.seq, secs);   // about 4 a second of signal
    CHECK(rx.state == 2 && rx.dataValid && a.dataValid && a.state == 2, "state %d", rx.state);
    CHECK(rx.blocksOk == a.blocksOk && rx.blocksBad == a.blocksBad, "common prefix copied");
    CHECK(a.blocksOk > (uint64_t)(secs * 1.5), "messages %llu", (unsigned long long)a.blocksOk);
    CHECK(a.blocksBad * 20 <= a.blocksOk, "bad %llu of %llu", (unsigned long long)a.blocksBad, (unsigned long long)a.blocksOk);
    CHECK(a.vesselCount >= (secs > 15 ? 14u : 8u) && a.vesselCount <= 19, "%u stations", a.vesselCount);
    CHECK(positioned >= (secs > 15 ? 13 : 8) && named >= (secs > 15 ? 6 : 2), "%d positions, %d names", positioned, named);
    CHECK(a.burstsPerMin[0] > 20 && a.burstsPerMin[1] > 20, "rates %.0f / %.0f", a.burstsPerMin[0], a.burstsPerMin[1]);
    CHECK(a.nmea.size() >= 25 && a.nmea.size() <= 50 && a.vessels.size() == a.vesselCount, "sentences %zu, vessels %zu", a.nmea.size(), a.vessels.size());
    CHECK(rx.dataSnrDb > 8 && rx.dataSnrDb < 60 && std::fabs(rx.dataSnrDb - a.snrDb) < 1e-3, "SNR %.1f", rx.dataSnrDb);
    CHECK(a.noiseDbfs[0] < -30 && a.noiseDbfs[0] > -100, "noise floor %.1f dBFS", a.noiseDbfs[0]);
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    CHECK(rx.rateOk, "the engine says the rate is too low");
    // a retune empties the table, and the report numbers go on growing
    TuneSettings t2 = t;
    t2.centerHz = 162.01e6;
    const uint64_t before = rx.seq;
    const uint32_t stationsBefore = a.vesselCount;
    e.retuneReset(t2);
    waitFor(5.0, [&] { e.latestRx(rx, 0); return rx.seq > before; });
    CHECK(rx.seq > before, "report numbers after the retune: %llu after %llu", (unsigned long long)rx.seq, (unsigned long long)before);
    CHECK(rx.ais.vesselCount < stationsBefore, "the table starts again after a retune (%u after %u)", rx.ais.vesselCount, stationsBefore);
    e.stop();
}

int main() {
    printf("engine, 2 Msps (the default of the mode), 16 s\n");
    run(2e6, 16);
    printf("engine, 10 Msps, 8 s\n");
    run(10e6, 8);
    printf(fails ? "ais engine: %d FAILED\n" : "ais engine: all passed\n", fails);
    return fails ? 1 : 0;
}
