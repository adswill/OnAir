// APRS / Packet through the engine, as the interface uses it: the synthetic source plays the mode's test signal (faster than a radio: the receiver
// is tested, not real time), Engine::setStandard(26) selects the mode, the frames come back through latestRx() with the mode's standard.
#include "dect2/engine.h"
#include "dect2/engine_testkit.h"
#include "dect2/packet_gen.h"
#include "dect2/packet_rx.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::enginetest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning mt = packetTuning();
    CHECK(mt.stdMode == 26 && modeTuningById("packet") && modeTuningById("packet")->stdMode == 26, "tuning table");
    const std::vector<PacketGenFrame> want = packetGenCycle(0);
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 26;
    t.synth.pace = kFastPace;
    t.synth.snrDb = 20;
    t.synth.modeOpt[0] = 0;
    FileOptions fo;
    e.setStandard(26);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 25, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    size_t seen = 0;
    waitFor(120, [&] {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            reports++;
            if (rx.standard != 25) wrong++;
            seen = rx.packet.frames.size();
        }
        return seen >= want.size();
    });
    e.stop();
    int found = 0;
    for (const PacketGenFrame& g : want)
        for (const PacketFrameInfo& r : rx.packet.frames)
            if (r.from == g.frame.from.str() && r.to == g.frame.to.str() && r.path == g.frame.pathStr() && r.info == g.frame.info && r.baud == g.baud) { found++; break; }
    printf("APRS / Packet: %d reports, %d of %zu frames, %s\n", reports, found, want.size(), packetSummary(rx.packet).c_str());
    CHECK(found == (int)want.size(), "%d of %zu frames", found, want.size());
    CHECK(wrong == 0, "%d reports with another standard", wrong);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(rx.state == 2 && rx.dataValid, "state %d", rx.state);
    CHECK(rx.blocksOk >= want.size(), "copied counters %llu", (unsigned long long)rx.blocksOk);
    CHECK(rx.packet.ok1200 > 0 && rx.packet.ok9600 > 0 && rx.packet.stations.size() >= 6, "1200: %llu, 9600: %llu, stations %zu", (unsigned long long)rx.packet.ok1200,
          (unsigned long long)rx.packet.ok9600, rx.packet.stations.size());
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
