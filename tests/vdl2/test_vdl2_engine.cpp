// VDL Mode 2 through the engine (setStandard(29)): the synthetic source plays the test traffic, the receiver decodes it and reports through
// latestRx(): frames, aircraft with positions, ground stations with their locations, every channel in the band; a retune keeps the report
// numbers growing.
#include "dect2/engine/engine.h"
#include "dect2/vdl2/vdl2_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = vdl2Tuning();
    CHECK(mt.stdMode == 29 && modeTuningById("vdl2") && modeTuningById("vdl2")->stdMode == 29, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 29;
    t.synth.snrDb = 28;
    t.synth.modeVal[0] = 3;                  // busier traffic
    FileOptions fo;
    e.setStandard(29);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 28, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 28) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const Vdl2Telemetry& v = rx.vdl2;
    int inBand = 0, withPos = 0, gsPos = 0, acars = 0, x25 = 0, xid = 0;
    for (const auto& c : v.channels) inBand += c.inBand;
    for (const auto& c : v.aircraft) withPos += c.hasPos;
    for (const auto& g : v.stations) gsPos += g.hasPos;
    for (const auto& m : v.messages) { acars += m.acars; x25 += m.proto == "X.25" || m.proto == "CLNP" || m.proto == "COTP"; xid += m.proto == "XID"; }
    printf("  %d reports: %s\n  %zu messages (ACARS %d, X.25 %d, XID %d), %d aircraft placed, %d ground stations placed\n", reports, vdl2Summary(v).c_str(), v.messages.size(), acars, x25, xid, withPos, gsPos);
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(v.channels.size() == 8 && inBand == 8,   // the 3.4 European set has 136.675 and 136.850 too (#400)
           "%zu channels, %d in band", v.channels.size(), inBand);
    CHECK(v.blocksOk >= 20 && rx.blocksOk == v.blocksOk, "%llu frames", (unsigned long long)v.blocksOk);
    CHECK(v.blocksBad * 10 <= v.blocksOk, "%llu bad bursts", (unsigned long long)v.blocksBad);
    CHECK(acars > 0 && x25 > 0 && xid > 0, "kinds");
    CHECK(v.aircraft.size() >= 4 && withPos >= 2 && v.stations.size() >= 4 && gsPos >= 4, "%zu aircraft (%d placed), %zu stations (%d placed)", v.aircraft.size(), withPos, v.stations.size(), gsPos);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    const uint64_t before = rx.seq;
    TuneSettings t2 = t; t2.centerHz = 136.9e6;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    e.latestRx(rx, 0);
    CHECK(rx.seq > before && std::fabs(rx.vdl2.centerHz - 136.9e6) < 1, "after retune: seq %llu, centre %.0f", (unsigned long long)rx.seq, rx.vdl2.centerHz);
    e.stop();
    printf(fails ? "vdl2 engine: %d FAILED\n" : "vdl2 engine: all passed\n", fails);
    return fails ? 1 : 0;
}
