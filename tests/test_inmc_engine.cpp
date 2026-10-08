// Inmarsat-C through the engine, as the interface and dect2cli use it: the synthetic source plays the NCS channel (8 bit samples, faster than a radio: the receiver
// is tested, not real time, and the run is counted in seconds of signal) with a carrier offset and a clock error; the engine must find the carrier, decode frames and messages, keep every sample and report through latestRx().
#include "dect2/engine.h"
#include "dect2/engine_testkit.h"
#include "dect2/inmc_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
using namespace dect2::enginetest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// the receiver makes a report every 0.25 s of signal: its number is a clock that does not depend on how often this thread looks
static const double kReportsPerSec = 4.0;

int main() {
    Engine e;
    DeviceInfo dev;                         // the synthetic source
    TuneSettings t;
    const ModeTuning* mt = modeTuningById("inmc");
    CHECK(mt && mt->stdMode == 19, "tuning entry");
    t.centerHz = 1537.1e6;
    t.bandwidthMhz = 0.005;
    t.sampleRate = 2e6;
    t.synth.mode = 19;
    t.synth.pace = 8.0;       // reports come about every 2 s of signal at this speed; the lock is expected within 14 s
    t.synth.snrDb = 28;                     // Eb/N0 8 dB: snrDb - 20
    t.synth.cfoHz = 6000;
    t.synth.sroPpm = 30;
    t.synth.modeOpt[0] = 5;
    FileOptions fo;
    e.setStandard(19);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 18, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true, locked = false;
    double firstLock = -1;
    const double secs = 26;
    const double s0 = signalSecs(e);
    int reports = 0;
    while (signalSecs(e) - s0 < secs) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            CHECK(rx.standard == 18, "report standard %d", rx.standard);
            if (rx.state == 2 && !locked) { locked = true; firstLock = rx.inmc.seq / kReportsPerSec; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // the report shown is the first one made after the last of the signal
    waitFor(5.0, [&] { return e.latestRx(rx, seq); });
    e.latestRx(rx, 0);
    const InmcTelemetry& m = rx.inmc;
    printf("  engine, 2 Msps, %.0f s: %d reports, frame sync after %.1f s: %s\n", secs, reports, firstLock, inmcSummary(m).c_str());
    printf("    carrier %+.1f Hz, Es/N0 %.1f dB, UW errors %.1f, %llu good frames, %zu messages, dropped samples %llu\n", m.cfoHz, m.esn0Db, m.uwErrorsAvg,
           (unsigned long long)m.blocksOk, m.messages.size(), (unsigned long long)e.droppedSamples());
    CHECK(locked && firstLock < 14.0, "frame sync after %.1f s", firstLock);
    CHECK(seqOk && reports >= 4, "report numbers: %d reports", reports);
    CHECK(m.seq > secs * 2, "the receiver made %llu reports in %.0f s of signal", (unsigned long long)m.seq, secs);   // 4 a second of signal
    CHECK(rx.state == 2 && m.frameLock && m.carrierLock && rx.rateOk, "state %d", rx.state);
    CHECK(m.blocksOk >= 2 && m.blocksBad == 0 && rx.blocksOk == m.blocksOk, "frames: %llu good, %llu bad", (unsigned long long)m.blocksOk, (unsigned long long)m.blocksBad);
    CHECK(m.ncs.valid && m.ncs.sat == 3 && m.ncs.lesId == 44, "NCS info");
    CHECK(m.dataValid && rx.dataValid && !m.messages.empty() && !m.messages[0].text.empty(), "messages: %zu", m.messages.size());
    CHECK(std::fabs(rx.cfoHz - 6000) < 4 && std::fabs(m.cfoHz - 6000) < 4, "carrier %+.1f Hz", rx.cfoHz);
    CHECK(rx.dataSnrDb > 0 && rx.dataSnrDb < 12, "SNR %.1f", rx.dataSnrDb);
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    // a retune forgets the frames; the report numbers go on growing
    TuneSettings t2 = t;
    t2.centerHz = 1537.6e6;
    const uint64_t before = rx.seq;
    e.retuneReset(t2);
    waitFor(5.0, [&] { e.latestRx(rx, 0); return rx.seq > before; });
    CHECK(rx.seq > before && rx.inmc.blocksOk == 0, "after the retune: report %llu after %llu, %llu frames", (unsigned long long)rx.seq, (unsigned long long)before, (unsigned long long)rx.inmc.blocksOk);
    e.stop();
    printf(fails ? "inmc engine: %d FAILED\n" : "inmc engine: all passed\n", fails);
    return fails ? 1 : 0;
}
