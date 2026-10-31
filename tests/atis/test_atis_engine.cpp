// ATIS through the engine (setStandard(213)): the synthetic source plays voice and ATIS bursts, the receiver reports through latestRx().
#include "dect2/atis/atis_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = atisTuning();
    CHECK(mt.stdMode == 213 && modeTuningById("atis") && modeTuningById("atis")->stdMode == 213 && modeTuning(213), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 213;
    t.synth.snrDb = 22;
    t.synth.cfoHz = 400;
    FileOptions fo;
    e.setStandard(213);
    e.atis().setSilent(true);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 212, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 14.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 212) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const AtisTelemetry& s = rx.atis;
    printf("  %d reports: %s\n", reports, atisSummary(s).c_str());
    for (const auto& st : s.stations) printf("    %s %s %s x%llu\n", st.digits.c_str(), st.callsign.c_str(), st.country.c_str(), (unsigned long long)st.count);
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(s.blocksOk >= 4 && s.stations.size() >= 3, "%llu messages, %zu stations", (unsigned long long)s.blocksOk, s.stations.size());
    e.stop();
    return testSummary("atis engine");
}
