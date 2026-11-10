// WWVB phase code on a simulated 60 kHz carrier: the amplitude keying on top, noise, a frequency offset, a bit error corrected, in both carrier polarities.
#include "dect2/time/wwvb_phase.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    std::mt19937 rng(5); dect2::normal_distribution<float> nd(0.f, 1.f);
    for (double cnr : {30.0, 12.0, 6.0}) for (double cfo : {0.0, 0.1}) for (int pol : {0, 1}) for (int startSample : {0, 3000}) {
        WwvbPhase d;
        int64_t minute = 1762700000 / 60 * 60;
        double ph = 0;
        std::array<uint8_t, 60> f = WwvbPhase::frameBits(minute);
        int sec = 0;
        for (long i = -(long)startSample; i < 8000 * 200; i++) {
            const int q = (int)(i % 8000 + 8000) % 8000;
            if (i >= 0 && q == 0) { sec++; if (sec % 60 == 0) { minute += 60; f = WwvbPhase::frameBits(minute); } }
            const int bit = f[(size_t)(sec % 60)];
            const double amp = q < 1600 ? 0.14 : 1.0;   // the AM keying (only its low part matters to the magnitude)
            ph += 2 * M_PI * cfo / 8000;
            std::complex<float> z = std::polar<float>((float)amp, (float)(ph + ((bit ^ pol) ? M_PI : 0)));
            const float sg = (float)std::pow(10.0, -cnr / 20) * 0.7f;
            d.feed(z + std::complex<float>(nd(rng), nd(rng)) * sg);
        }
        const auto& st = d.status();
        printf("CNR %.0f cfo %.2f pol %d start %d: locked %d, utc %lld, frames %llu ok, %llu bad, dst %d leap %d\n", cnr, cfo, pol, startSample, st.locked, (long long)st.utcMinute, (unsigned long long)st.framesOk, (unsigned long long)st.framesBad, st.dstOn, st.leap);
        if (cnr >= 12) { CHECK(st.locked, "no lock"); CHECK(st.utcMinute > 0 && (st.utcMinute - (1762700000 / 60 * 60)) % 60 == 0 && st.utcMinute >= (int64_t)1762700000 / 60 * 60 && st.utcMinute <= (int64_t)1762700000 / 60 * 60 + 300, "wrong minute %lld", (long long)st.utcMinute); CHECK(st.dstOn == 0 && st.leap == 0, "dst table"); }
    }
    { auto f = WwvbPhase::frameBits(1000000 * 60 + 946684800); for (int i : {0, 1, 2, 12}) CHECK(f[i] <= 1, "bits"); CHECK(f[2] == 1 && f[0] == 0, "sync word"); }
    printf(fails ? "wwvb phase FAILED\n" : "wwvb phase passed\n");
    return fails ? 1 : 0;
}
