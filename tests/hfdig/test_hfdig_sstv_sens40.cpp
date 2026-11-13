// 4.0 sensitivity guard (docs/SENSITIVITY-4.0-X2.md): the SSTV picture filter (along-line Wiener filter with the noise measured on the
// sync pulses, then a 3x3 Lee filter on the picture) against the same decoder with the filter off (the 3.9 picture), same noise.
// Test audio with a +-50 Hz tuning error and a +-50 ppm clock error, SNR in 3 kHz (as test_hfdig_sens39d.cpp):
//  - Martin M1 at 6 dB and Robot 36 at 10 dB: the filtered picture's mean error is under a fixed bound and well under the unfiltered one
//  - a textured picture at 30 dB (stripes, edges, text): the filter costs no more than 0.3 dB of PSNR
//  - a weak Robot 36 picture (4 dB) does not crash the decoder (3.9: a first line read as odd dereferenced an empty chroma line)
//  - 60 s of noise alone gives no picture
// Simulated signals only.
#include "sstv_sens40_util.h"
#include <cstdio>
using namespace sstvbench;
#include "../common/test_util.h"

int main() {
    struct G { const char* mode; double snr, bound; };
    for (const G g : {G{"Martin M1", 6, 14}, G{"Robot 36", 10, 18}}) {
        const int m = modeIndex(g.mode);
        CHECK(m >= 0, "%s: mode not found", g.mode);
        if (m < 0) continue;
        double on = 0, off = 0;
        int done = 0;
        for (int t = 0; t < 3; t++) {
            const uint64_t seed = 40 + (uint64_t)t;
            const double oh = (t & 1) ? 50 : -50, pp = (t & 2) ? 50 : -50;
            const Result a = trial(m, 0, g.snr, seed, oh, pp, true), b = trial(m, 0, g.snr, seed, oh, pp, false);
            done += a.complete;
            on += a.mae / 3; off += b.mae / 3;
        }
        printf("%s %.0f dB: mean error %.1f filtered, %.1f unfiltered\n", g.mode, g.snr, on, off);
        CHECK(done == 3, "%s %.0f dB: %d of 3 pictures complete", g.mode, g.snr, done);
        CHECK(on <= g.bound && on < off - 4, "%s %.0f dB: mean error %.1f filtered, %.1f unfiltered", g.mode, g.snr, on, off);
    }
    for (const char* mode : {"Martin M1", "Robot 36"}) {
        const int m = modeIndex(mode);
        if (m < 0) continue;
        const Result a = trial(m, 1, 30, 7, 0, 0, true), b = trial(m, 1, 30, 7, 0, 0, false);
        printf("%s textured 30 dB: PSNR %.2f filtered, %.2f unfiltered\n", mode, a.psnr, b.psnr);
        CHECK(a.complete && a.psnr >= b.psnr - 0.3, "%s textured 30 dB: PSNR %.2f filtered, %.2f unfiltered", mode, a.psnr, b.psnr);
    }
    {
        const int m = modeIndex("Robot 36");
        for (int t = 0; t < 3 && m >= 0; t++) (void)trial(m, 0, 4, 70 + (uint64_t)t, 50, 50, true);
        printf("Robot 36 at 4 dB: no crash\n");
    }
    const uint64_t pics = noiseOnlyPictures(60, 5);
    printf("noise only, 60 s: %llu pictures\n", (unsigned long long)pics);
    CHECK(pics == 0, "noise only: %llu pictures", (unsigned long long)pics);
    return testSummary("hfdig_sstv_sens40");
}
