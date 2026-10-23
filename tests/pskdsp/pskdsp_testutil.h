// Shared by the PSK demodulator tests: one generated signal, the blind analysis, and the checks every case has to pass.
#pragma once
#include "dect2/pskdsp/pskdsp.h"
#include "../common/test_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace dect2::pskd;

#ifndef PSKTEST_SEED
#define PSKTEST_SEED 1
#endif
struct Case {
    Order order = Order::Bpsk;
    double fs = 48000, baud = 1200, off = 0, esn0 = 25, dop = 0, ppm = 0, alpha = 0.35;
    int symbols = 3000;
    uint32_t seed = PSKTEST_SEED;
    const char* tag = "";
};

// the analysis must find the order, the rate (0.4 %), the carrier (at the end of the block: the Doppler ramp has moved it) and the Doppler slope
inline bool runCase(const Case& c, Analysis* out = nullptr) {
    GenSpec g;
    g.order = c.order; g.fs = c.fs; g.baud = c.baud; g.offsetHz = c.off; g.esn0Db = c.esn0; g.dopplerHzPerSec = c.dop; g.clockPpm = c.ppm; g.alpha = c.alpha;
    g.payloadBits = 256; g.seed = c.seed;
    g.frames = std::max(4, c.symbols * orderBits(c.order) / 256);
    const std::vector<cf> x = generate(g);
    const Analysis a = analyse(x.data(), x.size(), c.fs);
    const double dur = (double)x.size() / c.fs;
    const double endOff = c.off + c.dop * dur;
    const double tol = 0.03 * c.baud + 3 + 0.12 * std::fabs(c.dop) * dur;
    const bool ok = a.rateFound && a.order == c.order && std::fabs(a.baud / c.baud - 1) < 0.004 && std::fabs(a.carrierHz - endOff) < tol &&
                    (c.dop == 0 || (c.order == Order::Oqpsk ? std::fabs(a.dopplerHzPerSec) < 2 * std::fabs(c.dop) : std::fabs(a.dopplerHzPerSec - c.dop) < 0.4 * std::fabs(c.dop)));   // OQPSK's loop frequency is too jittery for the slope
    if (!ok)
        printf("  %-6s %-5s fs %.0f baud %.0f off %.0f esn0 %.0f dop %.0f ppm %.0f alpha %.2f: got %s %.2f baud, carrier %.1f (want %.1f), doppler %.2f, evm %.3f, %s\n", c.tag,
               orderName(c.order), c.fs, c.baud, c.off, c.esn0, c.dop, c.ppm, c.alpha, orderName(a.order), a.baud, a.carrierHz, endOff, a.dopplerHzPerSec, a.evm, a.status.c_str());
    if (out) *out = a;
    return ok;
}

inline const Order* allOrders() {
    static const Order o[4] = {Order::Bpsk, Order::Qpsk, Order::Oqpsk, Order::Psk8};
    return o;
}
