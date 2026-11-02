// KAWS through the engine on the synthetic source (setStandard(135)): the built-in test signal is the real transmitter's output (a chat message and a short beacon in the UTC slots of the computer clock) with a
// carrier offset of +30 Hz (after the 20 kHz tuning offset) and a little noise; the receiver must lock, decode and read the carrier offset back. Also: the transmit-mode and tuning tables name it,
// and a second start with the signal option set (modeOpt[0]) still decodes.
#include "dect2/engine/engine.h"
#include "dect2/kaws/kaws_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static bool run(int opt, double maxSec) {
    const ModeTuning mt = kawsTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 14.1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 135;
    t.synth.modeOpt[0] = opt;
    FileOptions fo;
    e.setStandard(135);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 134, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    const auto& k = rx.kaws;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    double doneAt = -1;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > maxSec) break;
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 134) wrong++;
            if (doneAt < 0 && (!k.chats.empty() || !k.beaconList.empty())) doneAt = el;
            if (doneAt >= 0 && el > doneAt + 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("  %s: %d reports, chats %zu beacons %zu, slots tried %llu, blocks ok %llu bad %llu, cfo %.0f Hz, clock err %.2f s, profile %d, SNR %.0f dB\n", kawsSummary(k).c_str(), reports, k.chats.size(), k.beaconList.size(), (unsigned long long)k.slotsTried, (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad, k.cfoHz, k.clockErrSec, k.profile, (double)k.snrDb);
    CHECK(reports > 5 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(doneAt > 0, "no decode within %.0f s", maxSec);
    CHECK(!k.chats.empty() || !k.beaconList.empty(), "no message");
    if (!k.chats.empty()) CHECK(k.chats[0].from == "ONAIR" && k.chats[0].text.find("ONAIR KAWS TEST SIGNAL") == 0, "chat from '%s': '%s'", k.chats[0].from.c_str(), k.chats[0].text.c_str());
    CHECK(k.profile == (opt > 0 ? opt - 1 : 3), "profile %d", k.profile);
    CHECK(std::fabs(k.cfoHz - 30.0) < 20.0, "carrier offset read %.0f Hz, sent 30", k.cfoHz);
    // the synthetic source runs on the computer clock through a paced queue, so a loaded machine delays the burst by up to a second or two;
    // the receiver accepts the nominal start +- 3 s (kaws_sched.h), so that is the bound that matters
    CHECK(std::fabs(k.clockErrSec) < 3.0, "burst %.2f s from slot start + 1.5 s", k.clockErrSec);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return doneAt > 0;
}

int main() {
    const ModeTuning mt = kawsTuning();
    CHECK(mt.stdMode == 135 && modeTuningById("kaws") && modeTuningById("kaws")->stdMode == 135, "tuning table");
    run(0, 450);   // returns as soon as the first message is decoded (about 60 s on a Mac); the Windows CI runner is several times slower
    run(5, 300);   // modeOpt[0] = 5: profile 4 (15 s slots)
    return testSummary("kaws synth");
}
