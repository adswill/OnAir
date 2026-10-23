// PSK demodulator analyser on generated signals: the signal, its symbol rate (within 0.5 %), the order and the pulse shape are found blind,
// from 75 Bd at 8 kHz up to 250 kBd at 2 Msps, BPSK / QPSK / OQPSK / 8PSK, root-raised cosine and rectangular pulses, with carrier offsets,
// off-centre signals, a Doppler ramp, a symbol clock error and noise; a carrier given by a click is used; noise alone never locks.
#include "dect2/pskdsp/pskdsp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2::pskd;
#include "../common/test_util.h"

struct Case { const char* name; Order o; double fs, baud, alpha, off, dop, ppm, esn0, secs; };

int main() {
    const Case cases[] = {
        {"BPSK 75 Bd at 8 kHz, rectangular", Order::Bpsk, 8000, 75, -1, 300, 0, 0, 20, 12},
        {"BPSK 1200 Bd, +700 Hz, 50 ppm", Order::Bpsk, 48000, 1200, 0.35, 700, 0, 50, 18, 2},
        {"BPSK 1200 Bd rectangular", Order::Bpsk, 48000, 1200, -1, -1500, 0, 0, 18, 2},
        {"BPSK 9600 Bd off centre, Doppler 120 Hz/s", Order::Bpsk, 250000, 9600, 0.5, -42000, 120, 0, 16, 1.5},
        {"QPSK 2400 Bd", Order::Qpsk, 48000, 2400, 0.35, 0, 0, 0, 20, 1.5},
        {"QPSK 38400 Bd at 1 Msps, +150 kHz", Order::Qpsk, 1e6, 38400, 0.2, 150000, 0, -30, 20, 0.4},
        {"OQPSK 4800 Bd", Order::Oqpsk, 96000, 4800, 0.35, 1200, 0, 0, 20, 1},
        {"8PSK 4800 Bd", Order::Psk8, 96000, 4800, 0.35, -800, 0, 0, 26, 1},
        {"BPSK 250 kBd at 2 Msps", Order::Bpsk, 2e6, 250000, 0.35, 300000, 0, 0, 16, 0.12},
        {"BPSK 300 Bd at 2 Msps (narrow in a wide capture)", Order::Bpsk, 2e6, 300, 0.35, -250000, 0, 0, 20, 2},
        {"BPSK 2400 Bd weak (8 dB)", Order::Bpsk, 48000, 2400, 0.35, 400, 0, 0, 8, 3},
    };
    for (const Case& c : cases) {
        GenSpec g;
        g.order = c.o; g.fs = c.fs; g.baud = c.baud; g.alpha = c.alpha < 0 ? 0 : c.alpha; g.offsetHz = c.off; g.dopplerHzPerSec = c.dop;
        g.clockPpm = c.ppm; g.esn0Db = c.esn0;
        g.payloadBits = 512;
        g.frames = (int)std::ceil(c.secs * c.baud * orderBits(c.o) / 512.0) + 1;
        std::vector<cf> x = generate(g);
        Analysis a = analyse(x.data(), x.size(), c.fs);
        const double err = a.baud > 0 ? std::fabs(a.baud - c.baud) / c.baud : 1;
        printf("%-50s %s baud %.2f (%.3f %%) %s %s EVM %.2f Eb/N0 %.1f carrier %+.1f (true %+.1f) doppler %.1f Hz/s\n", c.name, a.status.c_str(), a.baud, err * 100,
               orderName(a.order), a.shape.c_str(), a.evm, a.ebn0Db, a.carrierHz, c.off + c.dop * (double)x.size() / c.fs, a.dopplerHzPerSec);
        if (getenv("PSK_VERBOSE") || !a.locked) for (const auto& l : a.log) printf("    %s\n", l.c_str());
        CHECK(a.signal, "%s: no signal", c.name);
        CHECK(a.rateFound && err < 0.005, "%s: baud %.2f", c.name, a.baud);
        CHECK(a.order == c.o, "%s: order %s", c.name, orderName(a.order));
        CHECK(a.locked, "%s: not locked (EVM %.2f)", c.name, a.evm);
        if (c.alpha < 0) CHECK(a.rect, "%s: rectangular pulses not seen", c.name);
        if (c.dop == 0) CHECK(std::fabs(a.carrierHz - c.off) < std::max(3.0, 0.01 * c.baud), "%s: carrier %.1f", c.name, a.carrierHz);
        if (c.dop != 0) CHECK(std::fabs(a.dopplerHzPerSec - c.dop) < 0.35 * std::fabs(c.dop), "%s: Doppler %.1f Hz/s", c.name, a.dopplerHzPerSec);
    }
    // the carrier picked by a click: a weaker signal next to a stronger one
    {
        GenSpec g1, g2;
        g1.fs = g2.fs = 96000; g1.baud = 1200; g2.baud = 2400; g1.offsetHz = -20000; g2.offsetHz = 15000; g1.esn0Db = 30; g2.esn0Db = 18; g2.order = Order::Qpsk;
        g1.frames = 12; g2.frames = 12; g2.seed = 5;
        std::vector<cf> a1 = generate(g1), a2 = generate(g2);
        const size_t n = std::min(a1.size(), a2.size());
        std::vector<cf> x(n);
        for (size_t i = 0; i < n; i++) x[i] = a1[i] * 2.f + a2[i];
        AnalyseOptions o; o.haveCarrier = true; o.carrierHz = 15200; o.bwHz = 3000;
        Analysis a = analyse(x.data(), x.size(), g1.fs, o);
        printf("click on the weaker one: %s\n", a.status.c_str());
        CHECK(a.locked && a.order == Order::Qpsk && std::fabs(a.baud - 2400) < 12, "clicked signal: %s %.1f", orderName(a.order), a.baud);
        Analysis b = analyse(x.data(), x.size(), g1.fs);
        CHECK(b.locked && b.order == Order::Bpsk && std::fabs(b.baud - 1200) < 6, "strongest signal: %s %.1f", orderName(b.order), b.baud);
    }
    // noise alone (and a plain carrier) never locks
    {
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf> x(200000);
        for (auto& v : x) v = cf(nd(rng), nd(rng));
        Analysis a = analyse(x.data(), x.size(), 48000);
        printf("noise: %s\n", a.status.c_str());
        CHECK(!a.locked, "noise locked: %s", a.status.c_str());
        for (size_t i = 0; i < x.size(); i++) x[i] += 3.f * cf((float)std::cos(0.1 * (double)i), (float)std::sin(0.1 * (double)i));
        Analysis b = analyse(x.data(), x.size(), 48000);
        printf("carrier: %s\n", b.status.c_str());
        CHECK(!b.locked, "unmodulated carrier locked: %s", b.status.c_str());
    }
    // a seeded sweep of everything at once: every order, roll-offs 0.2 .. 0.8 and rectangular, odd rates, offsets up to a quarter of the
    // span, clock errors and Es/N0 14 .. 24 dB
    {
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> u(0, 1);
        const Order ords[] = {Order::Bpsk, Order::Qpsk, Order::Oqpsk, Order::Psk8};
        const double alphas[] = {-1, 0.2, 0.35, 0.5, 0.8};
        int ok = 0, total = 0;
        for (int i = 0; i < 28; i++) {
            GenSpec g;
            g.order = ords[i % 4];
            const double al = alphas[(i / 4) % 5];
            g.alpha = al < 0 ? 0 : al;
            g.fs = 48000 * (1 + (int)(u(rng) * 4));
            g.baud = std::round(g.fs / (6 + u(rng) * 40));
            g.offsetHz = (u(rng) - 0.5) * 0.5 * g.fs;
            g.clockPpm = (u(rng) - 0.5) * 200;
            g.esn0Db = (g.order == Order::Psk8 ? 20 : 14) + u(rng) * 10;
            g.seed = (uint32_t)(100 + i);
            g.payloadBits = 512;
            g.frames = (int)std::ceil(2500.0 * orderBits(g.order) / 512.0);
            std::vector<cf> x = generate(g);
            AnalyseOptions o; o.snapStandard = false;
            Analysis a = analyse(x.data(), x.size(), g.fs, o);
            const bool good = a.locked && a.order == g.order && std::fabs(a.baud - g.baud) / g.baud < 0.005;
            total++; ok += good;
            if (!good) {
                printf("sweep %d: %s %.0f Bd at %.0f Hz, alpha %.2f, %+.0f Hz, %.0f ppm, %.1f dB -> %s %.2f Bd, %s\n", i, orderName(g.order), g.baud, g.fs, al, g.offsetHz,
                       g.clockPpm, g.esn0Db, orderName(a.order), a.baud, a.status.c_str());
                for (const auto& l : a.log) printf("    %s\n", l.c_str());
            }
        }
        printf("sweep: %d of %d found\n", ok, total);
        CHECK(ok == total, "sweep: %d of %d", ok, total);
    }
    CHECK(snapRate(1199.0) == 1200 && snapRate(1210.0) == 0 && snapRate(9598) == 9600, "snapRate");
    return testSummary("pskdsp_analyse");
}
