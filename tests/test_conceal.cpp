// Picture concealment: video interpolation puts a moving object where it belongs, much closer to the truth than a plain cross-fade.
// The CPU motion search is checked everywhere; where a GPU backend exists (Direct3D 11 on Windows) it is checked against the true pictures
// and against the CPU result at full HD, with timings.
#include "dect2/conceal.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static double msSince(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

// peak signal to noise ratio of 8-bit data
static double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double se = 0;
    for (size_t i = 0; i < a.size(); i++) { const double d = (double)a[i] - b[i]; se += d * d; }
    const double mse = se / (double)a.size();
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// A textured background with a bright box moving right and a dark box moving down; chroma follows the boxes. position(t) is exact, so the
// true picture at any t in [0, 1] is known.
static VideoFrame scene(int w, int h, double t) {
    VideoFrame f; f.w = w; f.h = h; f.y.assign((size_t)w * h, 0); f.uv.assign((size_t)w * h / 2, 128);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) f.y[(size_t)y * w + x] = (uint8_t)(90 + 40 * std::sin(x * 0.07) * std::cos(y * 0.05) + 20 * std::sin(x * 0.31 + y * 0.17));
    const int bx = (int)std::lround(w * 0.10 + t * w * 0.05), by = h * 33 / 100;   // bright box, moves right by 5 % of the width (96 pixels at full HD)
    for (int y = by; y < by + h / 4; y++) for (int x = bx; x < bx + w / 24; x++) if (x >= 0 && x < w) { f.y[(size_t)y * w + x] = 230; f.uv[(size_t)(y / 2) * w + (x & ~1)] = 90; f.uv[(size_t)(y / 2) * w + (x & ~1) + 1] = 200; }
    const int dx = w * 60 / 100, dy = (int)std::lround(h * 0.10 + t * h * 0.08);   // dark box, moves down by 8 % of the height (86 pixels)
    for (int y = dy; y < dy + h / 16; y++) for (int x = dx; x < dx + w / 10; x++) if (y >= 0 && y < h) { f.y[(size_t)y * w + x] = 25; f.uv[(size_t)(y / 2) * w + (x & ~1)] = 170; f.uv[(size_t)(y / 2) * w + (x & ~1) + 1] = 100; }
    return f;
}

// mean absolute luma error over the pixels where the two real pictures differ (the moving objects and what they uncover): in a scene that is
// mostly static, an average over the whole picture would hide how well the motion is followed
static double movingAreaError(const VideoFrame& a, const VideoFrame& truth, const std::vector<uint8_t>& mask) {
    double e = 0; size_t n = 0;
    for (size_t i = 0; i < a.y.size(); i++) if (mask[i]) { e += std::fabs((double)a.y[i] - truth.y[i]); n++; }
    return n ? e / (double)n : 0.0;
}

int main() {
    // the repair has a time budget for live playback; the quality checks must not depend on how fast this computer is
#ifdef _WIN32
    _putenv_s("DECT2_CONCEAL_MS", "1000000000");
#else
    setenv("DECT2_CONCEAL_MS", "1000000000", 1);
#endif
    // ---- the CPU motion search at a small size: a bright box moving 160 pixels to the right, the gap has 5 pictures
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
        CHECK(interpolateGapCpu(A, B, count, mid), "interpolation refused");
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

    // ---- full HD, two objects moving differently, 8 pictures in the gap: CPU, GPU, cross-fade, all against the truth
    {
        const int w = 1920, h = 1080, count = 8;
        VideoFrame A = scene(w, h, 0.0), B = scene(w, h, 1.0);
        std::vector<uint8_t> mask(A.y.size());
        for (size_t i = 0; i < mask.size(); i++) mask[i] = std::abs((int)A.y[i] - (int)B.y[i]) > 8;
        std::vector<std::shared_ptr<VideoFrame>> cpu, gpu;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(interpolateGapCpu(A, B, count, cpu), "CPU repair refused");
        const double cpuMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        const bool haveGpu = interpolateGapGpu(A, B, count, gpu);
        const double gpuMs = msSince(t0);
        double errCpu = 0, errBlend = 0, errGpu = 0;
        for (int k = 0; k < count; k++) {
            const double t = (k + 1.0) / (count + 1.0);
            const VideoFrame truth = scene(w, h, t);
            VideoFrame blend = A;
            for (size_t i = 0; i < blend.y.size(); i++) blend.y[i] = (uint8_t)(A.y[i] * (1 - t) + B.y[i] * t + 0.5);
            errCpu += movingAreaError(*cpu[(size_t)k], truth, mask);
            errBlend += movingAreaError(blend, truth, mask);
            if (haveGpu) errGpu += movingAreaError(*gpu[(size_t)k], truth, mask);
        }
        printf("full HD, %d pictures, luma error vs truth where the pictures differ: CPU %.2f, cross-fade %.2f", count, errCpu / count, errBlend / count);
        if (haveGpu) printf(", GPU %.2f", errGpu / count);
        printf("; time CPU %.0f ms", cpuMs);
        if (haveGpu) printf(", GPU %.0f ms (first call, includes the one-off start of the device)", gpuMs);
        printf("\n");
        CHECK(errCpu < 0.7 * errBlend, "the CPU repair is not clearly better than a cross-fade at full HD");
        if (haveGpu) {
            CHECK(gpu.size() == cpu.size(), "the GPU made a different number of pictures");
            double worstPsnr = 99, chromaDiff = 0; long cn = 0;
            for (int k = 0; k < count && (size_t)k < gpu.size(); k++) {
                worstPsnr = std::min(worstPsnr, psnr(gpu[(size_t)k]->y, cpu[(size_t)k]->y));
                for (size_t i = 0; i < cpu[(size_t)k]->uv.size(); i++) { chromaDiff += std::fabs((double)gpu[(size_t)k]->uv[i] - cpu[(size_t)k]->uv[i]); cn++; }
            }
            // a second, warm call for the timing that matters (it includes the motion search, which runs on the CPU)
            std::vector<std::shared_ptr<VideoFrame>> again;
            t0 = std::chrono::steady_clock::now();
            interpolateGapGpu(A, B, count, again);
            const double warmMs = msSince(t0);
            printf("GPU vs CPU: luma PSNR (worst picture) %.1f dB, mean chroma difference %.3f; GPU warm call %.0f ms for %d pictures including the CPU motion search\n", worstPsnr, chromaDiff / (double)cn, warmMs, count);
            CHECK(errGpu < 0.7 * errBlend, "the GPU repair is not clearly better than a cross-fade at full HD");
            CHECK(errGpu < errCpu * 1.15 + 0.05, "the GPU repair is clearly worse than the CPU one");
            CHECK(worstPsnr > 30.0, "the GPU result differs too much from the CPU result");
        } else printf("no GPU repair backend here: the GPU checks are skipped\n");
    }
    printf(fails ? "conceal tests FAILED\n" : "conceal tests passed\n");
    return fails ? 1 : 0;
}
