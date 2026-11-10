// Pagers and APRS / Packet on HF through the engine (report: HackRF at 2 Msps on 6.1625 MHz, Whole span off showed -200 dBFS, on never
// decoded a strong POCSAG 512). The synthetic source plays the test signal at the mode's own rate and offset at an HF frequency and at the
// usual VHF/UHF one, with Whole span off and on; the receiver must get samples (a real level) and decode every page / frame each time.
// Simulated signal only: the synthetic source is not a HackRF, so this covers the engine's frequency, offset and rate path, not the radio.
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include <cstdio>
#include <string>
using namespace dect2;
using namespace dect2::enginetest;
#include "../common/test_util.h"

static void pagerRun(double hz, bool wide) {
    const ModeTuning mt = pagerTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = hz;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 25;
    t.synth.snrDb = 25;
    t.synth.cfoHz = 300;
    t.synth.pace = kFastPace;
    t.synth.modeOpt[2] = kPocsag512 + 1;
    FileOptions fo;
    e.setStandard(25);
    e.setWideVoice(wide);
    CHECK(e.start(dev, t, fo), "pager %.4f MHz: engine start", hz / 1e6);
    const auto want = pagerTestMessages(0, kPocsag512 + 1);
    RxTelemetry rx; uint64_t seq = 0;
    auto found = [&] {
        int n = 0;
        if (!rx.pager.messages) return 0;
        for (const auto& w : want) for (const auto& m : *rx.pager.messages) if (m.address == w.address && m.text == w.text) { n++; break; }
        return n;
    };
    waitFor(90, [&] { if (e.latestRx(rx, seq)) seq = rx.seq; return found() == (int)want.size() && signalSecs(e) > 2; });
    e.latestRx(rx, 0);
    e.stop();
    printf("pager %.4f MHz whole span %d: level %.1f dBFS, %.3f Msps in, %d of %zu pages, %zu carriers\n", hz / 1e6, wide, rx.pager.levelDb,
           rx.pager.inputRate / 1e6, found(), want.size(), rx.wide.size());
    if (!wide) CHECK(rx.pager.levelDb > -150, "pager %.4f MHz: level %.1f dBFS (no samples)", hz / 1e6, rx.pager.levelDb);
    CHECK(found() == (int)want.size(), "pager %.4f MHz whole span %d: %d of %zu pages", hz / 1e6, wide, found(), want.size());
}

static void packetRun(double hz, bool wide) {
    const ModeTuning mt = packetTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = hz;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 26;
    t.synth.snrDb = 20;
    t.synth.cfoHz = -400;
    t.synth.pace = kFastPace;
    t.synth.modeOpt[0] = 1;   // 1200 baud AFSK
    FileOptions fo;
    e.setStandard(26);
    e.setWideVoice(wide);
    CHECK(e.start(dev, t, fo), "packet %.4f MHz: engine start", hz / 1e6);
    const auto want = packetGenCycle(1);
    RxTelemetry rx; uint64_t seq = 0;
    auto found = [&] {
        int n = 0;
        for (const PacketGenFrame& g : want) for (const PacketFrameInfo& r : rx.packet.frames) if (r.from == g.frame.from.str() && r.info == g.frame.info) { n++; break; }
        return n;
    };
    waitFor(90, [&] { if (e.latestRx(rx, seq)) seq = rx.seq; return found() == (int)want.size(); });
    e.latestRx(rx, 0);
    e.stop();
    printf("packet %.4f MHz whole span %d: %d of %zu frames, %zu carriers\n", hz / 1e6, wide, found(), want.size(), rx.wide.size());
    CHECK(found() == (int)want.size(), "packet %.4f MHz whole span %d: %d of %zu frames", hz / 1e6, wide, found(), want.size());
}

int main() {
    for (double hz : {466.075e6, 6.1625e6, 3.5e6}) for (bool wide : {false, true}) pagerRun(hz, wide);
    for (double hz : {144.8e6, 10.1476e6, 29.25e6}) for (bool wide : {false, true}) packetRun(hz, wide);
    return testSummary("pager_hf");
}
