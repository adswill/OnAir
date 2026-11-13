// SSTV picture quality against SNR (docs/SENSITIVITY-4.0-X2.md). Not a ctest: a bench.
//   sstv_sens40_bench <mode name> <snrFrom> <snrTo> <step> <trials> [picture 0 test / 1 textured] [offHz] [ppm]
// Per SNR point: pictures complete, mean absolute error (levels) and PSNR against the sent picture, and the share of trials that pass the
// sens39d criterion (complete, MAE <= 20). SNR in 3 kHz (test_hfdig_sens39d.cpp). The offset is applied by a Hilbert frequency shifter, the
// clock error by resampling, so both work with any picture. SSTV_NF=0: the picture filter off (3.9). `noise <secs>` instead of a mode name: noise alone, pictures reported.
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "sstv_sens40_util.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace sstvbench;

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "noise") {
        const double secs = atof(argv[2]);
        uint64_t total = 0;
        for (int s = 0; s < 4; s++) total += noiseOnlyPictures(secs, 900 + (uint64_t)s);
        printf("noise only: %llu pictures in %.0f s\n", (unsigned long long)total, 4 * secs);
        return 0;
    }
    if (argc < 6) { fprintf(stderr, "usage: %s <mode> <from> <to> <step> <trials> [pic] [offHz] [ppm]\n", argv[0]); return 2; }
    const int m = modeIndex(argv[1]);
    if (m < 0) { fprintf(stderr, "no mode %s\n", argv[1]); return 2; }
    const double a = atof(argv[2]), b = atof(argv[3]), st = atof(argv[4]);
    const int trials = atoi(argv[5]);
    const int pic = argc > 6 ? atoi(argv[6]) : 0;
    const double off = argc > 7 ? atof(argv[7]) : 0, ppm = argc > 8 ? atof(argv[8]) : 0;
    printf("%s pic %d off %+g Hz clock %+g ppm\n", argv[1], pic, off, ppm);
    for (double snr = a; snr >= b - 1e-9; snr -= st) {
        int comp = 0, pass = 0;
        double mae = 0, psnr = 0;
        for (int t = 0; t < trials; t++) {
            const Result r = trial(m, pic, snr, 1000 + (uint64_t)(snr * 10 + 500) * 31 + (uint64_t)t, (t & 1) ? off : -off, (t & 2) ? ppm : -ppm, !getenv("SSTV_NF") || atoi(getenv("SSTV_NF")) != 0);
            comp += r.complete; pass += r.complete && r.mae <= 20;
            mae += r.mae; psnr += r.psnr;
        }
        printf("  %5.1f dB: complete %d/%d pass %d/%d MAE %.1f PSNR %.1f dB\n", snr, comp, trials, pass, trials, mae / trials, psnr / trials);
        fflush(stdout);
    }
    return 0;
}
