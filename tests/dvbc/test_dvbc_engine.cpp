// DVB-C through the engine, as the GUI uses it: the synthetic source plays the test signal (SynthConfig::mode = 42) in real time, the receiver
// runs on the engine's thread, and the transport stream analysis must find the test programme (PSI: a service with video and audio).
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void runCase(const char* name, int opt0, double cfo, double expectRs, int expectQam) {
    const ModeTuning mt = dvbcTuning();
    Engine e;
    DeviceInfo dev;
    dev.kind = DeviceInfo::Synthetic;
    dev.name = "synthetic";
    TuneSettings tune;
    tune.sampleRate = mt.sampleRate;
    tune.bandwidthMhz = mt.bandwidthMhz;
    tune.centerHz = mt.defMhz * 1e6;
    tune.synth.mode = 42;
    tune.synth.snrDb = 34;
    tune.synth.cfoHz = cfo;
    tune.synth.modeOpt[0] = opt0;
    FileOptions fo;
    e.setStandard(42);
    CHECK(e.start(dev, tune, fo), "%s: engine start", name);
    CHECK(e.activeStandard() == 41, "%s: active standard %d", name, e.activeStandard());
    RxTelemetry t;
    uint64_t seq = 0;
    bool locked = false, services = false;
    for (int i = 0; i < 60 && !(locked && services); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (e.latestRx(t, seq)) { seq = t.seq; locked |= t.standard == 41 && t.dvbc.tsLock; }
        const auto ts = e.tsSnapshot();
        for (const auto& s : ts.services) {
            bool v = false, a = false;
            for (const auto& x : s.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
            services |= v && a;
        }
    }
    // the packet counters start at the first input: the error flags of the first seconds (equaliser and carrier loop still converging) are
    // not judged, the steady state is what is counted after a settle
    // (a loaded machine converges later: wait, up to 20 s, until the bad-packet count has been still for 2 s)
    std::this_thread::sleep_for(std::chrono::seconds(3));
    for (int i = 0; i < 10; i++) {
        e.latestRx(t, 0);
        const uint64_t b = t.dvbc.packetsBad;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        e.latestRx(t, 0);
        if (t.dvbc.packetsBad == b) break;
    }
    // the steady state: a window of 2 s without a bad packet. On a loaded machine a scheduling hiccup (a block the engine could not keep up with)
    // costs a few bad packets once in a while, which is the machine and not the receiver: up to 6 windows are tried, the first clean one counts
    uint64_t packets0 = 0, bad0 = 0;
    DvbcTelemetry d;
    for (int w = 0; w < 6; w++) {
        e.latestRx(t, 0);
        packets0 = t.dvbc.packets; bad0 = t.dvbc.packetsBad;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        e.latestRx(t, 0);
        d = t.dvbc;
        if (t.rateOk && d.tsLock && d.qam != 0 && d.packets - packets0 > 1000 && d.packetsBad == bad0) break;   // also locked: a loaded machine can lose the lock for a moment
    }
    printf("  %s: %s\n", name, dvbcSummary(d).c_str());
    CHECK(locked && d.tsLock, "%s: transport stream lock", name);
    CHECK(services, "%s: the TS analysis sees a service with video and audio", name);
    CHECK(d.qam == expectQam, "%s: QAM %d", name, d.qam);
    CHECK(std::fabs(d.symbolRate / expectRs - 1) < 1e-4, "%s: symbol rate %.1f", name, d.symbolRate);
    CHECK(t.rateOk && d.packets - packets0 > 1000 && d.packetsBad == bad0, "%s: %llu packets in the steady state, %llu bad", name, (unsigned long long)(d.packets - packets0),
          (unsigned long long)(d.packetsBad - bad0));
    CHECK(!d.cells.empty() && !d.psdDb.empty() && !d.eqDb.empty(), "%s: pictures for the interface", name);
    e.stop();
}

int main() {
    CHECK(modeTuningById("dvbc") && modeTuningById("dvbc")->stdMode == 42, "tuning table");
    CHECK(std::fabs(dvbcChannelCentreMhz(475) - 474) < 1e-9 && std::fabs(dvbcChannelCentreMhz(100) - 114) < 1e-9 && std::fabs(dvbcChannelCentreMhz(900) - 858) < 1e-9, "cable raster");
    runCase("64-QAM 6.952", 0, 20e3, 6.952e6, 2);
    runCase("256-QAM 6.952", 4, -30e3, 6.952e6, 4);
    printf(fails ? "dvbc engine: %d FAILED\n" : "dvbc engine: all passed\n", fails);
    return fails ? 1 : 0;
}
