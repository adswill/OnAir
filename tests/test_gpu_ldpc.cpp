// Validates the Metal LDPC decoder against the CPU decoder on noisy codewords (all normal-frame rates), and times both.
#include "dect2/gpu_ldpc.h"
#include "dect2/t2fec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <cstdint>
#include <vector>
using namespace dect2;
int main() {
    auto& g = GpuLdpc::instance();
    if (!g.available()) { printf("no GPU\n"); return 0; }
    printf("GPU: %s\n", g.deviceName());
    std::mt19937 rng(1);
    int fails = 0;
    for (int shortF = 0; shortF < 2; shortF++)
    for (int rate = 0; rate < 6; rate++) {
        PlpFec f; f.shortFrame = shortF; f.rate = rate; f.mod = 2; f.rotation = false;
        const LdpcCode& c = ldpcFor(f);
        const int n = c.n(), k = c.k(), nb = 64;
        static const double snrDb[6] = {1.5, 3.5, 4.5, 5.5, 6.5, 7.5}; // a little above each rate's threshold
        double sigma = std::pow(10.0, -snrDb[rate] / 20.0) * 0.9; // BPSK sigma
        std::normal_distribution<float> nd(0, 1);
        std::vector<float> llr((size_t)nb * n);
        std::vector<std::vector<uint8_t>> cw(nb);
        for (int b = 0; b < nb; b++) {
            cw[b].resize(n);
            for (int i = 0; i < k; i++) cw[b][i] = rng() & 1;
            c.encode(cw[b]);
            for (int i = 0; i < n; i++) { float x = (cw[b][i] ? -1.f : 1.f) + (float)sigma * nd(rng); llr[(size_t)b * n + i] = 2 * x / (float)(sigma * sigma); }
        }
        std::vector<uint8_t> hard((size_t)nb * n), ok(nb);
        std::vector<int> it(nb);
        auto t0 = std::chrono::steady_clock::now();
        g.decode(c, llr.data(), nb, 50, hard.data(), ok.data(), it.data());
        double tg = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        int agree = 0, okc = 0, okg = 0, bitErr = 0;
        long itc = 0, itg = 0;
        for (int b = 0; b < nb; b++) {
            std::vector<float> l(llr.begin() + (size_t)b * n, llr.begin() + (size_t)(b + 1) * n);
            std::vector<uint8_t> h; int ic = 0;
            bool oc = c.decodeFast(l, 50, h, &ic);
            okc += oc; okg += ok[b]; itc += ic; itg += it[b];
            agree += (oc == (bool)ok[b]);
            if (ok[b]) for (int i = 0; i < n; i++) bitErr += hard[(size_t)b * n + i] != cw[b][i];
        }
        double tc = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool pass = bitErr == 0 && okg >= okc - 2 && okg > 0;
        printf("%s rate %d: gpu ok %d/%d cpu ok %d/%d, wrong bits in ok blocks %d, iters gpu %.1f cpu %.1f | gpu %.1f ms, cpu(1 thread) %.1f ms  %s\n", shortF ? "short" : "normal", rate, okg, nb, okc, nb, bitErr, (double)itg / nb, (double)itc / nb, tg, tc, pass ? "PASS" : "FAIL");
        fails += !pass;
    }
    return fails ? 1 : 0;
}
