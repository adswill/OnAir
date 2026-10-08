// ACARS through the engine, as the interface and dect2cli use it: the synthetic source plays the test signal (8 bit samples, paced like a radio),
// the engine must decode, keep every sample, and report through latestRx().
#include "dect2/acars_gen.h"
#include "dect2/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    Engine e;
    DeviceInfo dev;                         // the synthetic source
    TuneSettings t;
    t.centerHz = 131.725e6;                 // not the receiver's default: the engine must pass the tuned frequency on
    t.bandwidthMhz = 2;
    t.sampleRate = 2e6;
    t.synth.mode = 18;
    t.synth.snrDb = 30;
    t.synth.modeVal[0] = 3;                 // a block every 5 s per aircraft
    FileOptions fo;
    e.setStandard(18);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 17, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true;
    int reports = 0;
    const double secs = 20;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            CHECK(rx.standard == 17, "report standard %d", rx.standard);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.latestRx(rx, 0);
    const AcarsTelemetry& a = rx.acars;
    printf("  %d reports: %s\n", reports, acarsSummary(a).c_str());
    CHECK(seqOk && reports > secs * 2, "report numbers: %d reports", reports);
    CHECK(rx.state == 2 && rx.dataValid && a.dataValid, "state %d", rx.state);
    CHECK(rx.blocksOk > 20 && rx.blocksOk == a.blocksOk, "blocks %llu", (unsigned long long)rx.blocksOk);
    CHECK(rx.blocksBad <= rx.blocksOk / 20, "failed blocks %llu against %llu good", (unsigned long long)rx.blocksBad, (unsigned long long)rx.blocksOk);
    CHECK(a.aircraft.size() == 6 && a.channels.size() == 3, "%zu aircraft, %zu channels", a.aircraft.size(), a.channels.size());
    CHECK(rx.dataSnrDb > 5 && rx.dataSnrDb < 60, "SNR %.1f", rx.dataSnrDb);
    CHECK(a.messages.size() > 20 && a.messages.size() <= 200, "%zu messages", a.messages.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(std::fabs(a.centerHz - 131.725e6) < 1, "receiver centre %.0f Hz, tuned 131725000", a.centerHz);
    bool onGrid = !a.channels.empty();
    for (const auto& c : a.channels) onGrid &= std::fabs(c.freqHz - 131.725e6) < 1e6 && std::fabs(std::fmod(c.freqHz, 25e3)) < 1;
    CHECK(onGrid, "channel frequencies are not on the 25 kHz grid around the tuned frequency");
    // a retune empties the tables, and the report numbers go on growing
    const uint64_t before = rx.seq;
    TuneSettings t2 = t;
    t2.centerHz = 131.55e6;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    e.latestRx(rx, 0);
    CHECK(rx.seq > before, "report numbers after the retune: %llu after %llu", (unsigned long long)rx.seq, (unsigned long long)before);
    CHECK(std::fabs(rx.acars.centerHz - 131.55e6) < 1, "receiver centre after the retune %.0f Hz, tuned 131550000", rx.acars.centerHz);
    e.stop();
    printf(fails ? "acars engine: %d FAILED\n" : "acars engine: all passed\n", fails);
    return fails ? 1 : 0;
}
