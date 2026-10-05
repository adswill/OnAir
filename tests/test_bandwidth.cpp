// Bandwidth detection from modelled spectra: flat-top OFDM with ripple (multipath), noise, a DC spike, deep fades and neighbours.
#include "dect2/bandwidth.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static std::mt19937 rng(5);

// occupied = 0.952 * bw, edges roll off over ~0.1 MHz; the capture filter rolls off near the edge of the capture
// filterMhz: corner of a capture filter narrower than the channel (0 = none): the HackRF's 3.5 MHz setting measures as a 5th order
// lowpass with its corner at 2.1 MHz, 16 dB down at 3 MHz. It shapes the signal; the floor behind it is the converter's.
static std::vector<float> spectrum(double fsMhz, double bw, double snrDb, bool neighbours, bool fades, double offsetMhz = 0, double filterMhz = 0) {
    const int n = 4096;
    std::vector<float> d(n);
    std::normal_distribution<double> nd(0, 0.8);
    const double occ = 0.952 * bw, floorDb = -90;
    for (int i = 0; i < n; i++) {
        const double f = ((double)i / n - 0.5) * fsMhz;
        auto flat = [&](double c, double half) { return 1.0 / (1.0 + std::exp((std::fabs(f - c) - half) / 0.03)); };
        double lin = std::pow(10.0, floorDb / 10.0), sig = flat(offsetMhz, occ / 2);
        double level = std::pow(10.0, (floorDb + snrDb) / 10.0);
        double ripple = 1.0 + 0.3 * std::sin(f * 9.0) + 0.15 * std::sin(f * 31.0);
        if (fades) ripple *= 1.0 - 0.9 * std::exp(-std::pow((f - 1.3) / 0.12, 2));   // a deep notch inside the band
        lin += level * sig * ripple;
        if (neighbours) lin += level * 0.9 * (flat(offsetMhz + bw + 0.4, occ / 2) + flat(offsetMhz - bw - 0.4, occ / 2));
        if (filterMhz > 0) lin = std::pow(10.0, floorDb / 10.0) + (lin - std::pow(10.0, floorDb / 10.0)) / (1.0 + std::pow(f / filterMhz, 10));
        if (std::fabs(f) < 0.03) lin += level * 4;   // DC spike
        d[i] = (float)(10 * std::log10(lin) + nd(rng));
    }
    return d;
}

static BandwidthEstimate run(double fs, double bw, double snr, bool nb, bool fades, double off = 0, double filt = 0) {
    BandwidthDetector det;
    for (int k = 0; k < 15; k++) det.add(spectrum(fs, bw, snr, nb, fades, off, filt), fs);
    return det.estimate();
}

int main() {
    CHECK(snapBandwidth(7.61) == 8 && snapBandwidth(6.66) == 7 && snapBandwidth(5.71) == 6 && snapBandwidth(4.76) == 5 && snapBandwidth(1.54) == 1.7, "snap");
    const double bws[] = {5, 6, 7, 8};
    int n = 0, bad = 0;
    for (double fs : {20.0, 10.0}) for (double bw : bws) for (double snr : {12.0, 20.0, 30.0}) for (int nb = 0; nb < 2; nb++) for (int fd = 0; fd < 2; fd++) for (double off : {0.0, 0.3}) {
        if (fs < 15 && nb) continue;   // at 10 Msps neighbours are filtered out by the hardware
        BandwidthEstimate e = run(fs, bw, snr, nb, fd, off);
        n++;
        if (!e.valid || e.bwMhz != bw || false) { printf("FAIL: fs %.0f bw %.0f snr %.0f nb %d fade %d off %.1f -> valid %d occ %.2f bw %.1f\n", fs, bw, snr, nb, fd, off, e.valid, e.occupiedMhz, e.bwMhz); bad++; }
    }
    printf("%d cases, %d wrong\n", n, bad);
    fails += bad;
    {   // through the HackRF's filter at the app's rates (8 Msps for channels up to 6 MHz, 10 above): a 7 MHz channel read as 6
        int fn = 0, fbad = 0;
        for (double fs : {8.0, 10.0}) for (double bw : bws) for (double snr : {20.0, 30.0, 36.0}) for (int fd = 0; fd < 2; fd++) for (double off : {0.0, 0.3}) {
            if (bw == 8 && fs < 9) continue;   // no room at 8 Msps
            if (bw == 8 && snr < 25) continue;   // the edges of an 8 MHz channel are 25 dB down there, under the floor: nothing to find
            BandwidthEstimate e = run(fs, bw, snr, false, fd, off, 2.1);
            fn++;
            if (!e.valid || e.bwMhz != bw) { printf("FAIL: filtered fs %.0f bw %.0f snr %.0f fade %d off %.1f -> valid %d occ %.2f bw %.1f\n", fs, bw, snr, fd, off, e.valid, e.occupiedMhz, e.bwMhz); fbad++; }
        }
        printf("%d cases through the capture filter, %d wrong\n", fn, fbad);
        fails += fbad;
    }
    {   // noise only: nothing to report
        BandwidthDetector det;
        std::normal_distribution<double> nd(-90, 0.8);
        for (int k = 0; k < 15; k++) { std::vector<float> d(4096); for (auto& x : d) x = (float)nd(rng); det.add(d, 20); }
        CHECK(!det.estimate().valid && det.estimate().noFloor, "noise reported as a signal");
    }
    printf(fails ? "bandwidth tests FAILED\n" : "bandwidth tests passed\n");
    return fails ? 1 : 0;
}
