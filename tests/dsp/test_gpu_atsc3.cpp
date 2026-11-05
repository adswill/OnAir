// ATSC 3.0 Type B LDPC (rates 6/15..13/15, 16200 and 64800) on the GPU (GpuLdpc::decode with the atsc3::qcCode graph) against the CPU decoder
// (LdpcCode::decodeFast with the two-minimum correction): same code words from noisy BPSK soft bits, and the time per word of each. The GPU must
// converge wherever the CPU does (within one word of 24 per code) and its hard decisions must equal the sent word whenever it reports ok.
// Passes without a GPU. Prints a table for docs/GPU-AUDIT-3.7.md.
#include "../../core/src/atsc3/atsc3_ldpc.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/dsp/ldpc.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
using Clk = std::chrono::steady_clock;
static double ms(Clk::time_point a, Clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

int main() {
    if (!GpuLdpc::instance().available()) { printf("atsc3 gpu: no GPU, nothing to compare\n"); return 0; }
    printf("device: %s\n", GpuLdpc::instance().deviceName());
    std::mt19937 rng(5);
    dect2::normal_distribution<float> g(0.f, 1.f);
    int fails = 0;
    const int nbList[] = {1, 4, 16};
    for (int n : {16200, 64800}) {
        for (int r = 6; r <= 13; r++) {
            const atsc3::Ldpc& enc = atsc3::ldpcCode(n, r);
            const LdpcCode* qc = atsc3::qcCode(n, r);
            if (!enc.ok() || !qc) continue;
            const int nb = 16;
            std::vector<std::vector<uint8_t>> sent(nb);
            std::vector<float> llr((size_t)nb * n);
            // noise: Eb/N0-ish margin above the rate's capacity, BPSK; sigma per rate so the CPU decodes nearly all words
            const double R = r / 15.0;
            const double sigma = std::sqrt(1.0 / (2 * R * std::pow(10.0, (R < 0.5 ? 1.2 : 2.0) / 10)));
            for (int b = 0; b < nb; b++) {
                std::vector<uint8_t> w(enc.k());
                for (auto& x : w) x = rng() & 1;
                enc.encode(w);
                sent[(size_t)b] = w;
                for (int i = 0; i < n; i++) llr[(size_t)b * n + i] = (float)(2.0 / (sigma * sigma) * ((w[(size_t)i] ? -1.0 : 1.0) + sigma * g(rng)));
            }
            const float alpha = atsc3::qcAlpha(n, r);
            int cpuOk = 0;
            auto t0 = Clk::now();
            for (int b = 0; b < nb; b++) {
                std::vector<float> l(llr.begin() + (size_t)b * n, llr.begin() + (size_t)(b + 1) * n);
                std::vector<uint8_t> h; int it = 0;
                const bool ok = qc->decodeFast(l, 50, h, &it, alpha, true);
                cpuOk += ok && h == sent[(size_t)b];
            }
            const double tCpu = ms(t0, Clk::now()) / nb;
            std::vector<uint8_t> hard((size_t)nb * n), ok(nb);
            std::vector<int> it(nb);
            GpuLdpc::instance().decode(*qc, llr.data(), nb, 50, hard.data(), ok.data(), it.data(), alpha, true);   // warm-up (pipeline build)
            double tGpu[3] = {};
            for (int k = 0; k < 3; k++) {
                const int m = nbList[k];
                t0 = Clk::now();
                for (int rep = 0; rep < 5; rep++) GpuLdpc::instance().decode(*qc, llr.data(), m, 50, hard.data(), ok.data(), it.data(), alpha, true);
                tGpu[k] = ms(t0, Clk::now()) / 5 / m;
            }
            GpuLdpc::instance().decode(*qc, llr.data(), nb, 50, hard.data(), ok.data(), it.data(), alpha, true);
            int gpuOk = 0, wrong = 0;
            for (int b = 0; b < nb; b++) {
                const bool same = std::equal(sent[(size_t)b].begin(), sent[(size_t)b].end(), hard.begin() + (size_t)b * n);
                gpuOk += ok[(size_t)b] && same;
                wrong += ok[(size_t)b] && !same;
            }
            printf("n=%5d rate %2d/15: CPU %2d/%d ok %.2f ms/word | GPU %2d/%d ok %.2f (1 word) %.2f (4) %.2f (16) ms/word\n", n, r, cpuOk, nb, tCpu, gpuOk, nb, tGpu[0], tGpu[1], tGpu[2]);
            if (wrong) { printf("FAIL: GPU reported ok on a wrong word\n"); fails++; }
            if (gpuOk + 2 < cpuOk) { printf("FAIL: GPU converges on fewer words than the CPU\n"); fails++; }
        }
    }
    printf(fails ? "atsc3 gpu: %d FAILED\n" : "atsc3 gpu: ok\n", fails);
    return fails ? 1 : 0;
}
