// PSK demodulator analysis under real-world impairments: carrier far from the centre of the capture, symbol clock error of +-300 ppm,
// Doppler ramps, every roll-off from 0.2 to 1 and rectangular pulses.
#include "pskdsp_testutil.h"

int main() {
    for (int oi = 0; oi < 4; oi++) {
        const Order o = allOrders()[oi];
        Case c; c.order = o;
        c.tag = "offc"; c.off = -0.4 * 24000; CHECK(runCase(c), "off-centre low %s", orderName(o));
        c.off = 0.45 * 24000; CHECK(runCase(c), "off-centre high %s", orderName(o));
        c.off = 100; c.tag = "ppm"; c.ppm = 300; CHECK(runCase(c), "+300 ppm %s", orderName(o));
        c.ppm = -300; CHECK(runCase(c), "-300 ppm %s", orderName(o));
        c.ppm = 0; c.tag = "dop"; c.dop = 30; CHECK(runCase(c), "Doppler 30 Hz/s at 1200 baud %s", orderName(o));
        c.dop = -25; CHECK(runCase(c), "Doppler -25 Hz/s at 1200 baud %s", orderName(o));
        c.fs = 96000; c.baud = 9600; c.dop = 400; CHECK(runCase(c), "Doppler 400 Hz/s at 9600 baud %s", orderName(o));
        Case a; a.order = o; a.baud = 2400; a.off = 200; a.tag = "alpha";
        for (double al : {0.2, 0.5, 1.0, -1.0}) { a.alpha = al; CHECK(runCase(a), "roll-off %.1f %s", al, orderName(o)); }
    }
    // a DC spike and a second, weaker signal beside the wanted one
    {
        GenSpec g; g.order = Order::Qpsk; g.fs = 48000; g.baud = 2400; g.offsetHz = 7000; g.esn0Db = 25; g.frames = 40;
        std::vector<cf> x = generate(g);
        GenSpec h = g; h.offsetHz = -9000; h.esn0Db = 25; h.seed = 9; h.baud = 1200; h.frames = 20;
        const std::vector<cf> y = generate(h);
        for (size_t i = 0; i < x.size(); i++) x[i] += 0.3f * (i < y.size() ? y[i] : cf(0, 0)) + cf(0.05f, 0.02f);
        const Analysis a = analyse(x.data(), x.size(), g.fs);
        CHECK(a.rateFound && a.order == Order::Qpsk && std::fabs(a.baud - 2400) < 10 && std::fabs(a.carrierHz - 7000) < 80, "strongest signal wins over a weaker one and a DC spike: %s", a.status.c_str());
        AnalyseOptions o;
        o.haveCarrier = true; o.carrierHz = -9000; o.bwHz = 1800;
        const Analysis b = analyse(x.data(), x.size(), g.fs, o);
        CHECK(b.rateFound && std::fabs(b.baud - 1200) < 6 && std::fabs(b.carrierHz + 9000) < 60, "a given carrier picks the other signal: %s", b.status.c_str());
    }
    return testSummary("test_pskdsp_impair");
}
