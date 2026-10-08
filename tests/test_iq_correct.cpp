// DC spike removal and IQ imbalance correction (iq_correct.h): a tone with a DC offset and a 1 dB / 5 degree imbalance must come out with
// the DC and the mirror image of the tone at least 30 dB lower than before, the tone itself untouched, and nothing changed while off.
#include "dect2/iq_correct.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// the power of the frequency f (cycles per sample) in x, by correlation
static double power(const std::vector<cf32>& x, size_t from, double f) {
    std::complex<double> s = 0;
    for (size_t i = from; i < x.size(); i++) s += std::complex<double>(x[i]) * std::polar(1.0, -2 * M_PI * f * (double)i);
    return std::norm(s / (double)(x.size() - from));
}

int main() {
    const size_t n = 1 << 22;
    const double f = 0.0371;   // the tone; its mirror is at -f
    std::vector<cf32> x(n);
    const double g = std::pow(10.0, 1.0 / 20), ph = 5 * M_PI / 180;
    for (size_t i = 0; i < n; i++) {
        const double t = 2 * M_PI * f * (double)i;
        x[i] = cf32((float)(0.3 * std::cos(t) + 0.05), (float)(0.3 * g * std::sin(t + ph) - 0.03));   // imbalance and DC
    }
    const size_t tail = n - (1 << 19);   // measured after the estimates settle
    const double dc0 = power(x, tail, 0), img0 = power(x, tail, -f), tone0 = power(x, tail, f);

    IqCorrector off;
    std::vector<cf32> y = x;
    for (size_t o = 0; o < n; o += 65536) off.process(y.data() + o, 65536);
    CHECK(y == x, "off changes nothing");

    {   // an FM station tuned to the centre: the receiver asks for the DC part to be skipped, so a carrier at DC comes through untouched
        IqCorrector keep; keep.dc = true;
        std::vector<cf32> c(1 << 20, cf32(0.5f, 0.2f)), c0 = c;
        for (size_t o = 0; o < c.size(); o += 65536) keep.process(c.data() + o, 65536, true);
        CHECK(c == c0, "keepDc leaves a carrier at DC alone");
    }

    IqCorrector on; on.dc = true; on.iq = true;
    for (size_t o = 0; o < n; o += 65536) on.process(y.data() + o, 65536);
    const double dc1 = power(y, tail, 0), img1 = power(y, tail, -f), tone1 = power(y, tail, f);
    printf("DC %.1f -> %.1f dB, image %.1f -> %.1f dB, tone %.1f -> %.1f dB (measured %.2f dB, %.1f deg)\n", 10 * log10(dc0), 10 * log10(dc1 + 1e-30),
           10 * log10(img0), 10 * log10(img1 + 1e-30), 10 * log10(tone0), 10 * log10(tone1), on.gainDb(), on.phaseDeg());
    CHECK(dc1 < dc0 * 1e-3, "the DC is 30 dB lower");
    CHECK(img1 < img0 * 1e-3, "the mirror image is 30 dB lower");
    CHECK(std::fabs(10 * log10(tone1 / tone0)) < 1.0, "the tone keeps its level");
    printf(fails ? "iq correct: FAILED\n" : "iq correct: ok\n");
    return fails ? 1 : 0;
}
