// DRM through the engine, as the app uses it: the synthetic source plays the mode's test signal at the rate the tuning table asks for; the engine
// must lock, decode the multiplex and report it through latestRx without dropping samples. Real time: about 20 s.
#include "dect2/engine.h"
#include "dect2/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning tu = drmTuning();
    CHECK(tu.stdMode == 12 && std::string(tu.id) == "drm", "tuning table: standard %d id %s", tu.stdMode, tu.id);
    CHECK(tu.sampleRate >= tu.minSampleRate && tu.sampleRate <= 20e6, "tuning table: rate %.0f", tu.sampleRate);

    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "Synthetic";
    TuneSettings tune;
    tune.sampleRate = tu.sampleRate;
    tune.centerHz = tu.defMhz * 1e6;
    tune.bandwidthMhz = tu.bandwidthMhz;
    tune.synth.mode = 12;
    tune.synth.snrDb = 28;
    tune.synth.cfoHz = 1500;
    FileOptions fo;
    e.setStandard(12);
    CHECK(e.start(dev, tune, fo), "engine started");
    CHECK(e.activeStandard() == 11, "DRM is the active standard (%d)", e.activeStandard());
    std::vector<std::string> log;
    RxTelemetry t;
    uint64_t seq = 0, lastSeq = 0;
    bool seqOk = true, locked = false;
    const auto t0 = std::chrono::steady_clock::now();
    double lockAt = -1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (e.latestRx(t, seq)) {
            if (t.seq < lastSeq) seqOk = false;
            lastSeq = t.seq; seq = t.seq;
            if (t.standard == 11 && t.drm.state == 2 && t.drm.dataValid && !locked) { locked = true; lockAt = el; }
        }
        if (el > 22) break;
    }
    e.latestRx(t, 0);
    const DrmTelemetry& d = t.drm;
    printf("  lock after %.1f s; mode %c %.1f kHz, state %d, SNR %.1f dB, CFO %+.1f Hz, FAC %llu/%llu SDC %llu/%llu, audio frames %llu ok %llu bad, dropped %llu\n", lockAt, d.modeName, d.bandwidthKhz,
           d.state, d.snrDb, d.cfoHz, (unsigned long long)d.facOk, (unsigned long long)d.facBad, (unsigned long long)d.sdcOk, (unsigned long long)d.sdcBad, (unsigned long long)d.blocksOk,
           (unsigned long long)d.blocksBad, (unsigned long long)e.droppedSamples());
    CHECK(t.standard == 11 && locked, "no lock through the engine");
    CHECK(t.state == 2 && t.dataValid && t.rateOk, "state %d dataValid %d rateOk %d", t.state, (int)t.dataValid, (int)t.rateOk);
    CHECK(std::fabs(t.cfoHz - 1500.0) < 5.0, "CFO %.1f Hz (sent 1500)", t.cfoHz);
    CHECK(d.mode == 1 && d.occupancy == 3 && d.mscQam == 64, "mode %d occupancy %d %d-QAM", d.mode, d.occupancy, d.mscQam);
    CHECK(d.services.size() == 1 && d.services[0].label == "OnAir DRM", "service label");
    CHECK(d.textMessage == "OnAir DRM test signal", "text message '%s'", d.textMessage.c_str());
    CHECK(d.timeValid && d.year == 2026, "time");
    CHECK(d.blocksOk > 20 && d.blocksBad == 0, "audio frames %llu ok %llu bad", (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad);
    CHECK(t.dataSnrDb > 24 && t.dataSnrDb < 32, "SNR %.1f dB", t.dataSnrDb);
    CHECK(seqOk, "telemetry sequence went backwards");
    CHECK(e.droppedSamples() == 0, "%llu samples dropped", (unsigned long long)e.droppedSamples());
    CHECK(lockAt > 0 && lockAt < 12, "lock after %.1f s", lockAt);
    e.stop();
    printf(fails ? "drm engine: FAILED\n" : "drm engine: ok\n");
    return fails ? 1 : 0;
}
