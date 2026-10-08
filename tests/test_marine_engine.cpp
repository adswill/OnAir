// Marine through the engine as the interface and dect2cli use it: the synthetic source (8 bit samples at 2 Msps, faster than a radio: the receiver
// is tested, not real time, and the run is counted in seconds of signal)
// plays NAVTEX, then an MF/HF DSC sequence, then a VHF one; the engine must decode, report and keep every sample.
#include "dect2/engine.h"
#include "dect2/engine_testkit.h"
#include "dect2/marine_gen.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

static void session(const char* name, int service, double centerHz, double secs, int msg, double idle) {
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = centerHz + marineTuning().tuneOffsetHz;       // the radio sits 20 kHz above the user's frequency
    t.bandwidthMhz = 0.01;
    t.sampleRate = 2e6;
    t.synth.mode = 17;
    t.synth.pace = 20.0;      // nothing here is timed
    t.synth.snrDb = 25;
    t.synth.modeOpt[0] = service;
    t.synth.modeOpt[2] = msg;
    t.synth.modeVal[1] = idle;
    FileOptions fo;
    e.setStandard(17);
    e.marine().setFrequencyHz(centerHz);
    CHECK(e.start(dev, t, fo), "%s: engine start", name);
    CHECK(e.activeStandard() == 16, "%s: active standard %d", name, e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true;
    int reports = 0;
    const double s0 = signalSecs(e);
    while (signalSecs(e) - s0 < secs) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            CHECK(rx.standard == 16, "%s: report standard %d", name, rx.standard);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // the report shown is the first one made after the last of the signal
    waitFor(5.0, [&] { return e.latestRx(rx, seq); });
    e.latestRx(rx, 0);
    const MarineTelemetry& m = rx.marine;
    printf("%s: %d reports, %s\n  NAVTEX %zu, DSC %zu, snr %.1f dB, dropped samples %llu\n", name, reports, marineSummary(m).c_str(), m.navtex.size(), m.dsc.size(), m.snrDb, (unsigned long long)e.droppedSamples());
    CHECK(seqOk && reports >= 4, "%s: %d reports", name, reports);
    CHECK(m.seq > secs * 2, "%s: the receiver made %llu reports in %.0f s of signal", name, (unsigned long long)m.seq, secs);   // about 4 a second of signal time
    CHECK(rx.rateOk, "%s: the engine says the rate is too low", name);
    CHECK(e.droppedSamples() == 0, "%s: dropped %llu samples", name, (unsigned long long)e.droppedSamples());
    CHECK(rx.dataValid && m.dataValid && rx.blocksOk == m.blocksOk && rx.blocksBad == m.blocksBad, "%s: telemetry fields", name);
    CHECK(rx.state >= 1, "%s: state %d", name, rx.state);
    if (service <= 1) {
        const std::string want = "010000 UTC JAN 26\nTEST MESSAGE FROM STATION A. 518 KHZ.";
        int exact = 0;
        for (const auto& n : m.navtex) if (n.complete && n.text == want && n.station == 'A' && n.subject == 'A' && n.number == 9) exact += n.repeats;
        CHECK(exact >= (SANITIZED ? 0 : 1), "%s: %d exact messages", name, exact);
        CHECK(m.serviceActive == 1, "%s: service %d", name, m.serviceActive);
    } else {
        int distress = 0;
        for (const auto& c : m.dsc) if (c.format == 112 && c.eccOk && c.fromMmsi == "232123456" && c.hasPos) distress++;
        CHECK(distress >= (SANITIZED ? 0 : 1), "%s: %d distress alerts", name, distress);
        CHECK(m.serviceActive == 2 || m.serviceActive == 4, "%s: service %d", name, m.serviceActive);
    }
    // a retune empties the tables, and the report numbers go on growing
    const uint64_t before = rx.seq;
    TuneSettings t2 = t; t2.centerHz += 1000;
    e.retuneReset(t2);
    waitFor(5.0, [&] { e.latestRx(rx, 0); return rx.seq > before; });
    CHECK(rx.seq > before, "%s: report numbers after the retune: %llu after %llu", name, (unsigned long long)rx.seq, (unsigned long long)before);
    e.stop();
}

int main() {
    // the signal must be long enough for one message or call; a slow (sanitised) build just takes longer, the source waits for it
    session("NAVTEX 518 kHz", 1, 518e3, 20, 4, 2);
    session("DSC 8414.5 kHz", 2, 8414.5e3, SANITIZED ? 12 : 14, 0, 0);
    session("DSC VHF 156.525 MHz", 4, 156.525e6, SANITIZED ? 4 : 9, 0, 0);
    printf(fails ? "marine engine: %d FAILED\n" : "marine engine: all passed\n", fails);
    return fails ? 1 : 0;
}
