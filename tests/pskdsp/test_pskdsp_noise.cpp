// PSK demodulator analysis near the noise limit: the least Es/N0 at which the order, rate and carrier are still found (with a margin of
// 3 dB over what was measured), pure noise, a block that is too short, and the forced options.
#include "pskdsp_testutil.h"

int main() {
    // measured limits on generated signals: BPSK 5 dB, QPSK 8 dB, 8PSK 14 dB, OQPSK 14 dB (Es/N0); tested at 3 dB over
    const double limit[4] = {8, 11, 17, 17};
    for (int oi = 0; oi < 4; oi++) {
        Case c; c.order = allOrders()[oi]; c.off = 150; c.tag = "noise";
        for (double add : {0.0, 6.0}) {
            c.esn0 = limit[oi] + add;
            CHECK(runCase(c), "%s at %.0f dB Es/N0", orderName(c.order), c.esn0);
        }
        c.seed = 5; c.esn0 = limit[oi]; c.baud = 4800; c.fs = 96000; c.off = -3000;
        CHECK(runCase(c), "%s at %.0f dB Es/N0, 4800 baud, seed 5", orderName(c.order), c.esn0);
    }
    // nothing but noise, and too little of it
    {
        GenSpec g; g.fs = 48000; g.baud = 1200; g.esn0Db = -60; g.frames = 8;
        const std::vector<cf> x = generate(g);
        const Analysis a = analyse(x.data(), x.size(), g.fs);
        CHECK(!a.rateFound && !a.locked, "noise only gives no lock: %s", a.status.c_str());
        const Analysis s = analyse(x.data(), 1000, g.fs);
        CHECK(!s.signal && s.status == "need more samples", "short block: %s", s.status.c_str());
    }
    // forced settings are obeyed
    {
        GenSpec g; g.order = Order::Qpsk; g.fs = 48000; g.baud = 1200; g.offsetHz = 400; g.esn0Db = 22; g.frames = 40;
        const std::vector<cf> x = generate(g);
        AnalyseOptions o;
        o.forceOrder = Order::Qpsk; o.forceBaud = 1200; o.forceAlpha = 0.35;
        const Analysis a = analyse(x.data(), x.size(), g.fs, o);
        CHECK(a.rateFound && a.order == Order::Qpsk && a.baud == 1200 && std::fabs(a.alpha - 0.35) < 0.01 && a.locked, "forced QPSK 1200 baud RRC 0.35: %s", a.status.c_str());
        o = AnalyseOptions(); o.snapStandard = false;
        const Analysis b = analyse(x.data(), x.size(), g.fs, o);
        CHECK(b.rateFound && !b.baudSnapped && std::fabs(b.baud - 1200) < 5, "snap off keeps the measured rate (%.3f)", b.baud);
        CHECK(a.merDb > 15 && a.ebn0Db < a.esn0Db && a.bitRate == 2400, "quality figures: MER %.1f, Es/N0 %.1f, Eb/N0 %.1f, %.0f bit/s", a.merDb, a.esn0Db, a.ebn0Db, a.bitRate);
    }
    // snapRate
    CHECK(snapRate(1201.5) == 1200 && snapRate(9590) == 9600 && snapRate(75.2) == 75 && snapRate(38390) == 38400, "snapRate to the standard rates");
    CHECK(snapRate(1300) == 0 && snapRate(5555) == 0, "snapRate leaves the others");
    return testSummary("test_pskdsp_noise");
}
