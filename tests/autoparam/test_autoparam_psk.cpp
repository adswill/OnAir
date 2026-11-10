// Auto for PSK31 and friends (BPSK31 .. QPSK125): every modulation with offset, drift, clock error and a noise sweep, simulated signals only (PskGenerator).
#include "dect2/psk/psk_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

namespace {
constexpr double kRate = 250000, kOff = -20000;
const std::string kText = "CQ CQ CQ DE LY2ABC LY2ABC PSE K\nTHE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789\n";

struct Run { PskTelemetry t; int modeSeen = -1; };
PskTelemetry runLive(std::vector<PskGenSignal> sigs, double secs, double cfo, uint32_t seed, bool autoOn, int mode = 0, PskReceiver* rx = nullptr) {
    PskGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.signals = std::move(sigs); c.seed = seed;
    PskGenerator g(c);
    PskReceiver local;
    PskReceiver& r = rx ? *rx : local;
    if (!rx) { r.configure(kRate); r.setSignalOffset(kOff); r.setMode(mode); r.setAuto(autoOn); }
    PskTelemetry res;
    std::vector<cf32> x(25000);
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        PskTelemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res = std::move(tt); }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));   // the trial decoders' thread needs some of the time a real radio gives it
    }
    return res;
}
PskGenSignal sig(double hz, int mode, double snr) {
    PskGenSignal s;
    s.offsetHz = hz; s.mode = mode; s.snrDb = snr; s.text = kText; s.repeat = true; s.pauseSec = 1;
    return s;
}
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    // 1. every modulation, offset, drift and clock error
    int ok = 0, tried = 0;
    for (int m = 0; m < kPskModes; m++) {
        if (psk8psk(m)) continue;   // 8PSK is not in Auto (chosen by hand)
        tried++;
        PskGenSignal s = sig(1000 + 150.0 * m, m, 20);
        s.driftHzPerMin = 3;
        s.clockPpm = 15;
        const auto t = runLive({s}, 26, m % 2 ? 80 : -80, (uint32_t)(11 + m), true);
        const bool hit = t.autoLocked && t.autoMode == m && t.mode == m;
        printf("  %-8s offset, drift, clock: auto %s, locked %d, %llu chars\n", pskModeName(m), t.autoMode >= 0 ? pskModeName(t.autoMode) : "none", (int)t.autoLocked, (unsigned long long)t.blocksOk);
        ok += hit;
    }
    printf("PSK31 family, every modulation (20 dB in its bandwidth, 80 Hz offset, 3 Hz/min drift, 15 ppm clock): %d of %d right\n", ok, tried);
    CHECK(ok >= tried - 1, "%d of %d", ok, tried);
    // 2. noise sweep on BPSK31 and BPSK63
    for (double snr : {20.0, 12.0, 8.0, 5.0}) {
        int g = 0, tot = 0;
        for (int m : {0, 1}) for (int k = 0; k < 4; k++) {
            const auto t = runLive({sig(800 + 300.0 * k, m, snr)}, 20, 0, (uint32_t)(200 + 10 * m + k), true);
            tot++;
            g += t.autoLocked && t.autoMode == m;
        }
        printf("noise, %4.0f dB in the signal's bandwidth: %d of %d right\n", snr, g, tot);
        if (snr >= 12) CHECK(g * 100 >= tot * 75, "noise %g: %d of %d", snr, g, tot);
    }
    // 3. noise alone: no false lock
    {
        int bad = 0;
        for (int k = 0; k < 8; k++) {
            const auto t = runLive({}, 20, 0, (uint32_t)(300 + k), true);
            bad += t.autoLocked;
        }
        printf("noise alone: %d false locks of 8\n", bad);
        CHECK(bad == 0, "noise false locks %d", bad);
    }
    // 4. two signals in view, the mode with the most decoded text wins
    {
        const auto t = runLive({sig(700, 0, 22), sig(2000, 1, 12)}, 24, 0, 50, true);
        printf("two signals (BPSK31 strong, PSK63 weak): auto %s, locked %d\n", t.autoMode >= 0 ? pskModeName(t.autoMode) : "none", (int)t.autoLocked);
    }
    // 5. the signal changes: Auto follows; a fixed mode stays
    {
        PskReceiver r;
        r.configure(kRate); r.setSignalOffset(kOff); r.setMode(0); r.setAuto(true);
        auto t = runLive({sig(1200, 1, 20)}, 24, 0, 60, true, 0, &r);
        printf("live: first signal PSK63: auto %s, locked %d\n", t.autoMode >= 0 ? pskModeName(t.autoMode) : "none", (int)t.autoLocked);
        CHECK(t.autoLocked && t.autoMode == 1, "first signal");
        t = runLive({sig(1200, 4, 20)}, 28, 0, 61, true, 0, &r);
        printf("live: after the change to BPSK125: auto %s, locked %d\n", t.autoMode >= 0 ? pskModeName(t.autoMode) : "none", (int)t.autoLocked);
        CHECK(t.autoLocked && t.autoMode == 4, "relock");
        r.setAuto(false);
        r.setMode(5);
        t = runLive({sig(1200, 0, 20)}, 12, 0, 62, false, 0, &r);
        printf("manual BPSK250 kept while another signal plays: mode %s\n", pskModeName(t.mode));
        CHECK(t.mode == 5, "manual kept");
    }
    printf("%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("autoparam_psk");
}
