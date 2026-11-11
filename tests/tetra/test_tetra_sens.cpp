// TETRA demodulator sensitivity (3.9): one carrier of the test cell at 72 kHz (4 samples per symbol), with a tuning error, a symbol clock
// error, a random start and AWGN measured in the 25 kHz channel, through tetra::decodeCarrier. A capture counts as decoded when it gives at
// least half the signalling blocks (CRC good) the clean capture gives, the criterion of tools/sens/sens_sweep. Simulated signals only.
//
// Default (about a second): the clean capture decodes every block, 5 dB passes under every impairment, noise alone gives nothing.
// ONAIR_TEST_LONG=1: the threshold sweep (TETRA_SENS_HI = 10 dB down to TETRA_SENS_LO = 0 dB in 1 dB steps, 8 trials per point, stopping
// below 90 %) for AWGN, +-1.2 kHz and +-50 ppm. docs/TETRA-3.9.md has the results.
#include "dect2/tetra/tetra_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/tetra/tetra_phy.h"
#include "dect2/tetra/tetra_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2;
using namespace dect2::tetra;
#include "../common/test_util.h"

namespace {

constexpr size_t kCapture = (size_t)(2.3 * kRate);

// One capture: cell `cellIdx` of scene 1, starting at a random slot and sample, cfoHz, ppm clock error, SNR in 25 kHz (>= 99: no noise)
std::vector<cf32> capture(int cellIdx, double snrDb, double cfoHz, double ppm, uint32_t seed, bool signal = true) {
    std::mt19937 g(seed);
    TetraCellCfg c = tetraTestCell(1, cellIdx);
    c.startSlot = (uint32_t)(g() % (60u * 72u));
    Modulator mod;
    std::vector<cf32> sym;
    const int slots = (int)(kCapture / kSlotSamples) + 4;
    for (int k = 0; k < slots; k++) mod.push(cellSlot(c, c.startSlot + (uint32_t)k), sym);
    std::vector<cf32> x(kCapture);
    cell::Resampler rs(0.45);
    const double step = 1 + ppm * 1e-6;
    const double start = rs.halfLen() + 2 + (double)(g() % 1000) + (double)(g() % 1000) / 1000.0;
    rs.run(sym.data(), sym.size(), start, step, x.data(), x.size());
    dect2::normal_distribution<float> nd(0.f, 1.f);
    const float sigma = snrDb >= 99 ? 0.f : (float)std::sqrt(kRate / 25e3 / std::pow(10.0, snrDb / 10) / 2);
    const double w = 2 * M_PI * cfoHz / kRate;
    double ph = (double)(g() % 6283) / 1000.0;
    for (auto& v : x) {
        if (!signal) v = 0;
        v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sigma * nd(g), sigma * nd(g));
        ph = std::fmod(ph + w, 2 * M_PI);
    }
    return x;
}

struct Point { int ok = 0, trials = 0; double blocks = 0; };

Point run(double snr, double cfo, double ppm, int trials, int clean, uint32_t seed0) {
    Point p;
    for (int t = 0; t < trials; t++) {
        const double sgn = (t & 1) ? -1 : 1;
        const std::vector<cf32> x = capture(t % 2, snr, sgn * cfo, sgn * ppm, seed0 + 101u * (uint32_t)t);
        const CarrierDecode d = decodeCarrier(x.data(), x.size());
        p.trials++;
        p.blocks += d.blocksOk;
        if (2 * d.blocksOk >= clean) p.ok++;
    }
    p.blocks /= trials;
    return p;
}

} // namespace

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;
    // the clean captures: every signalling block of both cells
    int clean = 1 << 30;
    for (int cidx = 0; cidx < 2; cidx++) {
        const std::vector<cf32> x = capture(cidx, 200, 0, 0, 7u + (uint32_t)cidx);
        const CarrierDecode d = decodeCarrier(x.data(), x.size());
        CHECK(d.tetra && d.synced && d.blocksBad == 0 && d.blocksOk > 100, "clean cell %d: synced %d ok %d bad %d", cidx, d.synced, d.blocksOk, d.blocksBad);
        CHECK(d.sync.mcc == 1 && d.sync.mnc == 1, "clean cell %d identity", cidx);
        clean = std::min(clean, d.blocksOk);
    }
    printf("clean: %d blocks per capture\n", clean);
    // noise alone: nothing decodes
    {
        const std::vector<cf32> x = capture(0, 0, 0, 0, 99, false);
        const CarrierDecode d = decodeCarrier(x.data(), x.size());
        CHECK(d.blocksOk == 0 && !d.synced, "noise decoded %d blocks", d.blocksOk);
    }
    struct Imp { const char* name; double cfo, ppm; };
    const Imp imps[] = {{"awgn", 0, 0}, {"cfo 1.2 kHz", 1200, 0}, {"clock 50 ppm", 0, 50}};
    if (!longRun) {
        // 3.9: the weak point every impairment must still pass (the 3.8 demodulator decoded 5, 1 and 4 of 8 captures here)
        const double snr = std::getenv("TETRA_SENS_SNR") ? std::atof(std::getenv("TETRA_SENS_SNR")) : 5.0;
        for (const Imp& im : imps) {
            const Point p = run(snr, im.cfo, im.ppm, 4, clean, 1000);
            printf("%-14s %4.1f dB: %d / %d decoded, %.1f blocks\n", im.name, snr, p.ok, p.trials, p.blocks);
            CHECK(p.ok == p.trials, "%s at %.1f dB: %d / %d", im.name, snr, p.ok, p.trials);
        }
        return testSummary("tetra_sens");
    }
    const double hi = std::getenv("TETRA_SENS_HI") ? std::atof(std::getenv("TETRA_SENS_HI")) : 10;
    const double lo = std::getenv("TETRA_SENS_LO") ? std::atof(std::getenv("TETRA_SENS_LO")) : 0;
    for (const Imp& im : imps) {
        double thr = NAN;
        for (double snr = hi; snr >= lo - 1e-9; snr -= 1) {
            const Point p = run(snr, im.cfo, im.ppm, 8, clean, 5000 + (uint32_t)(snr * 10));
            printf("%-14s %5.1f dB: %d / %d decoded, %.1f blocks\n", im.name, snr, p.ok, p.trials, p.blocks);
            fflush(stdout);
            if (10 * p.ok >= 9 * p.trials) thr = snr; else break;
        }
        printf("THRESHOLD %s: %.1f dB\n", im.name, thr);
    }
    return testSummary("tetra_sens");
}
