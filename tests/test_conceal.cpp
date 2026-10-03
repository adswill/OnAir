// Picture concealment: video interpolation puts a moving object where it belongs, much closer to the truth than a plain cross-fade.
#ifdef _WIN32
#include <stdlib.h>
#define setenv(k, v, o) _putenv_s(k, v)
#endif
#include "dect2/conceal.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    setenv("DECT2_SWINTERP", "1", 1);   // this test checks the motion search; the machine-learning backend (macOS) is judged on real footage
    // ---- video: textured background, a bright box moving 160 pixels to the right between A and B, the gap has 5 pictures
    {
        const int w = 640, h = 360;
        auto make = [&](int boxX) {
            VideoFrame f; f.w = w; f.h = h; f.y.assign((size_t)w * h, 0); f.uv.assign((size_t)w * h / 2, 128);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) f.y[(size_t)y * w + x] = (uint8_t)(90 + 40 * std::sin(x * 0.07) * std::cos(y * 0.05) + 20 * std::sin(x * 0.31 + y * 0.17));
            for (int y = 120; y < 240; y++) for (int x = boxX; x < boxX + 100; x++) if (x >= 0 && x < w) f.y[(size_t)y * w + x] = 230;
            return f;
        };
        const int xa = 100, xb = 260, count = 5;
        VideoFrame A = make(xa), B = make(xb);
        std::vector<std::shared_ptr<VideoFrame>> mid;
        CHECK(interpolateGap(A, B, count, mid), "interpolation refused");
        double errMC = 0, errBlend = 0; long n = 0;
        for (int k = 0; k < count; k++) {
            const double t = (k + 1.0) / (count + 1.0);
            VideoFrame truth = make((int)std::lround(xa + t * (xb - xa)));
            for (size_t i = 0; i < truth.y.size(); i++) {
                const double bl = A.y[i] * (1 - t) + B.y[i] * t;
                errMC += std::fabs((double)(*mid[(size_t)k]).y[i] - truth.y[i]);
                errBlend += std::fabs(bl - truth.y[i]);
                n++;
            }
        }
        printf("video: mean luma error vs the true picture: motion-compensated %.2f, plain cross-fade %.2f\n", errMC / n, errBlend / n);
        CHECK(errMC < 0.7 * errBlend, "interpolation is not clearly better than a cross-fade");
    }
    printf(fails ? "conceal tests FAILED\n" : "conceal tests passed\n");
    return fails ? 1 : 0;
}
