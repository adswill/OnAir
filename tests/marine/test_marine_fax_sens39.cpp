// 3.9 sensitivity pass D (docs/SENSITIVITY-3.9-D.md): the 90 % threshold of the weather fax decoder on its test chart (tests/data/marine/fax
// fax_util.h): a whole transmission of 100 lines at 120 lpm, white noise over 0 .. 6 kHz (the SNR definition of test_marine_fax_weak.cpp;
// the fax occupies about a quarter of it), a +-30 Hz tuning error and a 20 ppm audio clock error, a new noise each trial. A trial passes when
// the line lock holds (101 rows, 120 lpm, drift under one pixel) and the mean row correlation against the chart is over 0.5.
// Default: three trials at the recorded threshold + 2 dB. ONAIR_TEST_LONG=1: the sweep (8 trials a point, 1 dB steps). FAX_MT / FAX_PPM
// set the tuning (Hz) and clock (ppm) errors, FAX_V=1 prints every trial. Simulated signals only.
#include "../data/marine/fax/fax_util.h"
#include <cstdlib>
#include <string>
#include <ctime>
using namespace faxt;

static double cpuSeconds() { return (double)std::clock() / CLOCKS_PER_SEC; }   // process CPU time (wall time on Windows)

static bool trial(double snr, uint32_t seed, double* cpu = nullptr) {
    Opts o; o.lines = 100; o.snrDb = snr; o.seed = seed; o.mistuneHz = ((seed & 1) ? 1 : -1) * (getenv("FAX_MT") ? atof(getenv("FAX_MT")) : 30.0); o.ppm = ((seed & 2) ? 1 : -1) * (getenv("FAX_PPM") ? atof(getenv("FAX_PPM")) : 20.0);
    const auto a = makeAudio(o);
    FaxDecoder d;
    d.configure(o.rate);
    const double c0 = cpuSeconds();
    feedAll(d, a, 4096);
    if (cpu) *cpu = (cpuSeconds() - c0) / ((double)a.size() / o.rate);
    FaxImage im; uint64_t seq = 0;
    d.latestImage(im, seq);
    const FaxStatus st = d.status();
    const Quality q = compare(im, faxTestChart(faxImageWidth(576), 100, seed), 1, 20);
    if (getenv("FAX_V")) printf("    %.0f dB seed %u: rows %d lpm %d drift %.2f corr %.3f\n", snr, seed, im.height, st.lpm, q.drift, q.meanCorr);
    return im.height == 101 && st.lpm == 120 && std::fabs(q.drift) < 1.0 && q.meanCorr > 0.5;
}

int main() {
    const bool longRun = getenv("ONAIR_TEST_LONG") && atoi(getenv("ONAIR_TEST_LONG")) > 0;
    const double kThr = 2;   // recorded 90 % point, dB over 0 .. 6 kHz
    double cpu = 0;
    FCHECK(trial(30, 1, &cpu), "no chart at 30 dB");
    printf("weather fax: CPU %.4f s per audio second\n", cpu);
    if (!longRun) {
        int ok = 0;
        for (uint32_t i = 0; i < 3; i++) ok += trial(kThr + 2, 100 + i);
        FCHECK(ok == 3, "at %.0f dB: %d of 3", kThr + 2, ok);
    } else {
        double thr = NAN;
        std::string pts;
        for (double snr = kThr + 6; snr >= kThr - 8; snr -= 1) {
            int ok = 0;
            for (uint32_t i = 0; i < 8; i++) ok += trial(snr, (uint32_t)(snr * 10 + 1000) * 7 + i);
            char b[32]; snprintf(b, sizeof b, " %g:%d/8", snr, ok); pts += b;
            if (ok == 8) thr = snr; else if (!std::isnan(thr) || snr < kThr - 3) break;
        }
        printf("  weather fax threshold %s dB (0 .. 6 kHz) |%s\n", std::isnan(thr) ? "-" : std::to_string((int)thr).c_str(), pts.c_str());
    }
    printf(fails ? "marine_fax_sens39: %d FAILED\n" : "marine_fax_sens39: all passed\n", fails);
    return fails ? 1 : 0;
}
