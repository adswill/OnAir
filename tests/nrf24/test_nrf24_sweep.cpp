// nRF24 receiver sweep: the share of frames found against Eb/N0 for each data rate (prints the table), a carrier offset and a data-rate error,
// and the three rates in one channel. The thresholds are the yields the receiver must reach at a realistic Eb/N0.
#include "dect2/nrf24/nrf24_gen.h"
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/nrf24/nrf24_tel.h"
#include <cstdio>
#include <cmath>
using namespace dect2;
#include "../common/test_util.h"

static double yield(int kbps, double ebn0, double cfo, double ppm, double fs, double sec) {
    Nrf24GenConfig c;
    c.rate = fs; c.rateKbps = kbps; c.dialOffsetHz = -1.5e6; c.cfoHz = cfo; c.ebn0Db = ebn0; c.clockPpm = ppm;
    c.frames = nrf24TestFrames(0);
    c.gapSec = 0.0015;
    Nrf24Receiver rx;
    rx.configure(fs);
    rx.setSignalOffset(c.dialOffsetHz);
    Nrf24Generator g(c);
    std::vector<cf32> buf(1 << 14);
    size_t sent = 0;
    // count the frames the generator made: every frame is ~ (bits / rate + gap) long
    for (size_t done = 0; done < (size_t)(sec * fs); done += buf.size()) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    rx.flush();
    Nrf24Telemetry t;
    rx.telemetry(t, 0);
    uint64_t heard = 0;
    for (const auto& f : t.frames) heard += (uint64_t)f.repeats;
    (void)sent;
    return (double)heard;
}

// per address at one Eb/N0: the short frames are the hard ones (the detector averages over a few microseconds)
// returns the number of CRC-16 addresses (a CRC-8 address that is a chance match is possible but rare, and is listed with its CRC size)
static size_t perAddress(int kbps, double ebn0) {
    Nrf24GenConfig c;
    c.rate = 8e6; c.rateKbps = kbps; c.dialOffsetHz = -1.5e6; c.ebn0Db = ebn0; c.frames = nrf24TestFrames(0); c.gapSec = 0.0015;
    Nrf24Receiver rx;
    rx.configure(c.rate);
    rx.setSignalOffset(c.dialOffsetHz);
    Nrf24Generator g(c);
    std::vector<cf32> buf(1 << 14);
    for (size_t done = 0; done < (size_t)(1.0 * c.rate); done += buf.size()) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    rx.flush();
    Nrf24Telemetry t;
    rx.telemetry(t, 0);
    printf("  %4d k at %.0f dB:", kbps, ebn0);
    for (const auto& d : t.devices) printf("  %s%s x%llu", d.address.c_str(), d.crcBits == 8 ? " (CRC-8)" : "", (unsigned long long)d.count);
    printf("\n");
    size_t n16 = 0;
    for (const auto& d : t.devices) if (d.crcBits == 16) n16++;
    return n16;
}

int main() {
    for (int r : {250, 1000, 2000}) { const size_t n = perAddress(r, 16); CHECK(n == 4, "%d k at 16 dB: %zu addresses, 4 were sent", r, n); }
    printf("  frames decoded in 1.0 s (about 300 are sent; copies count):\n  Eb/N0 dB:");
    const double eb[] = {10, 12, 14, 16, 18, 20, 30};
    for (double e : eb) printf("%7.0f", e);
    printf("\n");
    double y[3][7];
    const int rates[3] = {250, 1000, 2000};
    for (int r = 0; r < 3; r++) {
        printf("  %4d k   :", rates[r]);
        for (int k = 0; k < 7; k++) { y[r][k] = yield(rates[r], eb[k], 20e3, 0, 8e6, 1.0); printf("%7.0f", y[r][k]); }
        printf("\n");
    }
    for (int r = 0; r < 3; r++) {
        const double top = y[r][6];
        CHECK(top > 100, "%d k: only %.0f frames at 30 dB", rates[r], top);
        CHECK(y[r][3] >= 0.85 * top, "%d k: %.0f of %.0f frames at 16 dB Eb/N0", rates[r], y[r][3], top);
        CHECK(r != 0 || y[r][2] >= 0.85 * top, "250 k: %.0f of %.0f frames at 14 dB Eb/N0", y[r][2], top);
        CHECK(y[r][1] <= y[r][5] + 1, "%d k: more frames at 12 dB than at 20 dB", rates[r]);
    }
    const double a = yield(1000, 20, 100e3, 0, 8e6, 1.0), b = yield(1000, 20, -100e3, 0, 8e6, 1.0), c = yield(1000, 20, 0, 1000, 8e6, 1.0), d = yield(2000, 20, 0, -1000, 8e6, 1.0);
    printf("  1 M at 20 dB: cfo +100 kHz %.0f, -100 kHz %.0f, clock +1000 ppm %.0f; 2 M clock -1000 ppm %.0f\n", a, b, c, d);
    CHECK(a > 0.8 * y[1][5] && b > 0.8 * y[1][5] && c > 0.8 * y[1][5] && d > 0.8 * y[2][5], "carrier offset or clock error costs frames");
    return testSummary("nrf24 sweep");
}
