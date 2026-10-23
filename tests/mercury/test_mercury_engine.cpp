// Mercury through the engine (setStandard(232)): the synthetic source plays the test signal at 2 Msps, the receiver reports through
// latestRx(): the broadcast script (a message on DATAC3, a RaptorQ file on DATAC1 that needs repair symbols). The whole demo link is
// test_mercury_monitor's (at 8 kHz, without the engine: it is 90 s of signal).
#include "dect2/mercury/mercury_rx.h"
#include "dect2/mercury/mercury_gen.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = mercuryTuning();
    CHECK(mt.stdMode == 232 && modeTuningById("mercury") && modeTuningById("mercury")->stdMode == 232, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 232;
    t.synth.snrDb = 20;
    t.synth.cfoHz = 4;
    t.synth.modeOpt[0] = 1;   // broadcast only
    FileOptions fo;
    e.setStandard(232);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 231, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    // the script is about 26 s; the receiver reports a burst about 0.8 s after it ends
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 30.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 231) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const MercuryTelemetry& p = rx.mercury;
    printf("  %d reports: %s, %llu bursts, %llu failed\n", reports, mercurySummary(p).c_str(), (unsigned long long)p.bursts, (unsigned long long)p.fecFailed);
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    const size_t want = mercury::demoScript(1).size();
    CHECK(p.blocksOk >= want, "%llu frames decoded of %zu", (unsigned long long)p.blocksOk, want);
    CHECK(p.messages.find("LY2ONA: broadcast test from the OnAir generator") != std::string::npos, "message");
    std::vector<uint8_t> d;
    std::string name;
    CHECK(e.mercury().fileBytes(0, d, name) && d == mercury::demoFileData(), "file bytes");
    CHECK(!p.files.empty() && p.files[0].done && p.files[0].name == mercury::demoFileName(), "broadcast file");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("mercury engine");
}
