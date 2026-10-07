// DVB-S/S2 through the engine, as the GUI uses it: the synthetic source plays the mode's test signal (SynthConfig::mode = 8) in real time, the engine runs
// the receiver on its analysis thread, and the transport stream analysis must find the test programme (a video and an audio stream). The signal options
// are the ones of dvbs_gen.h.
#include "dect2/dvbs_gen.h"
#include "dect2/engine.h"
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case {
    const char* name;
    int opt[8];
    double symbolRate;
    double snr;
    int expectStd;                  // 1 DVB-S, 2 DVB-S2
    const char* expectMod;
    const char* expectRate;
    double sampleRate;
};

static void runCase(const Case& c) {
    Engine e;
    DeviceInfo dev;
    dev.kind = DeviceInfo::Synthetic;
    dev.name = "synthetic";
    TuneSettings tune;
    tune.sampleRate = c.sampleRate;
    tune.bandwidthMhz = 8;
    tune.centerHz = 1500e6;
    tune.synth.mode = 8;
    tune.synth.snrDb = c.snr;
    for (int i = 0; i < 8; i++) tune.synth.modeOpt[i] = c.opt[i];
    tune.synth.modeVal[0] = c.symbolRate;
    FileOptions fo;
    e.setStandard(8);
    CHECK(e.start(dev, tune, fo), "%s: engine started", c.name);
    CHECK(e.activeStandard() == 7, "%s: DVB-S/S2 is the active standard (%d)", c.name, e.activeStandard());
    bool locked = false, services = false;
    RxTelemetry t;
    uint64_t seq = 0;
    double firstLock = -1;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 80 && !(locked && services); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (e.latestRx(t, seq)) {
            seq = t.seq;
            if (t.standard == 7 && t.dvbs.tsLock && !locked) { locked = true; firstLock = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
        }
        const auto ts = e.tsSnapshot();
        for (const auto& s : ts.services) {
            bool v = false, a = false;
            for (const auto& x : s.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
            services |= v && a;
        }
    }
    // let it run a little longer and look at the steady state
    std::this_thread::sleep_for(std::chrono::seconds(4));
    e.latestRx(t, 0);
    const DvbsTelemetry& d = t.dvbs;
    printf("  %s: %s (lock after %.1f s)\n", c.name, dvbsSummary(d).c_str(), firstLock);
    CHECK(locked, "%s: transport stream lock", c.name);
    CHECK(services, "%s: the transport stream analysis sees a video and an audio stream", c.name);
    CHECK(d.standard == c.expectStd, "%s: standard %d", c.name, d.standard);
    CHECK(d.modulationName == c.expectMod && d.codeRate == c.expectRate, "%s: %s %s", c.name, d.modulationName.c_str(), d.codeRate.c_str());
    CHECK(std::fabs(d.symbolRate / c.symbolRate - 1.0) < 3e-4, "%s: symbol rate %.1f Hz", c.name, d.symbolRate);
    CHECK(d.snrDb > c.snr - 2.5 && d.snrDb < c.snr + 2.5, "%s: Es/N0 %.1f dB (signal %.1f dB)", c.name, d.snrDb, c.snr);
    CHECK(d.dataValid && d.state == 2 && t.rateOk, "%s: state %d, data valid %d, rate ok %d", c.name, d.state, (int)d.dataValid, (int)t.rateOk);
    CHECK(d.psdDb.size() >= 256 && !d.cells.empty() && d.cells.size() <= 2048, "%s: spectrum (%zu points) and constellation (%zu cells) for the interface", c.name, d.psdDb.size(), d.cells.size());
    // blocks the receiver could not decode, and TS packets with the error flag: none in steady state
    // (a CI machine that cannot keep up with the real-time source loses frames: not checked there)
    if (!std::getenv("CI")) CHECK(d.blocksBad <= 2, "%s: %llu bad frames or packets", c.name, (unsigned long long)d.blocksBad);
    CHECK(d.packets > 1000 && d.packetsBad == 0, "%s: %llu packets, %llu with the error flag", c.name, (unsigned long long)d.packets, (unsigned long long)d.packetsBad);
    e.stop();
}

int main() {
    // defaults: DVB-S2 QPSK 2/3, 5 Msym/s at 10 Msps
    runCase({"default DVB-S2 QPSK 2/3", {}, 5e6, 20.0, 2, "QPSK", "2/3", 10e6});
    // DVB-S2 8PSK 3/4, short frames and pilots, 3 Msym/s, roll-off 0.25
    runCase({"DVB-S2 8PSK 3/4 short pilots", {0, 1, 7, 1, 1, 1}, 3e6, 20.0, 2, "8PSK", "3/4", 8e6});
    // DVB-S QPSK 3/4, spectrum inverted
    runCase({"DVB-S QPSK 3/4 inverted", {1, 0, 3, 0, 0, 0, 1}, 4e6, 16.0, 1, "QPSK", "3/4", 10e6});
    printf(fails ? "dvbs engine: FAILED\n" : "dvbs engine: ok\n");
    return fails ? 1 : 0;
}
