#include "dect2/gpu_ldpc.h"
#include "dect2/t2fec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>
using namespace dect2;
int main(int argc, char** argv) {
    // usage: bench_fec [snr_dB] [mod 0..3] [rate 0..5] [blocks per second the mux needs]
    float snr = argc > 1 ? atof(argv[1]) : 16.f;
    PlpFec f; f.rate = argc > 3 ? atoi(argv[3]) : 2; f.mod = argc > 2 ? atoi(argv[2]) : 2; f.rotation = true;
    const double needBlocksPerSec = argc > 4 ? atof(argv[4]) : 624;
    FecDims d = fecDims(f);
    const LdpcCode& ldpc = ldpcFor(f); const BchCode& bch = bchFor(f);
    const auto& map = bitInterleaverMap(f);
    std::mt19937 rng(9); std::normal_distribution<float> nd(0.f, 1.f);
    const int blocks = 64, cells = d.cellsPerBlock, bps = d.bitsPerCell;
    std::vector<std::vector<float>> llrs(blocks);
    std::vector<std::vector<uint8_t>> msgs(blocks);
    double tDem = 0;
    for (int b = 0; b < blocks; b++) {
        msgs[b].resize(d.kBch); for (auto& v : msgs[b]) v = rng() & 1;
        auto bits = msgs[b]; bch.encode(bits, d.kBch); ldpc.encode(bits);
        std::vector<uint16_t> lab(cells);
        for (int c = 0; c < cells; c++) { unsigned l = 0; for (int k = 0; k < bps; k++) l = (l << 1) | bits[map[c * bps + k]]; lab[c] = l; }
        std::vector<cf32> tx; qamMapBlock(f, lab, tx);
        float n0 = std::pow(10.f, -snr / 10.f) / 2.f;
        std::vector<float> nn(cells, n0);
        for (auto& v : tx) v += cf32(nd(rng), nd(rng)) * std::sqrt(n0);
        std::vector<float> ll((size_t)cells * bps); llrs[b].resize(d.nLdpc);
        auto t0 = std::chrono::steady_clock::now();
        qamDemapBlock(f, tx.data(), nn.data(), cells, ll.data());
        tDem += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (int p = 0; p < d.nLdpc; p++) llrs[b][map[p]] = ll[p];
    }
    printf("(fast demapper)\n");
    for (int mode = 0; mode < 2; mode++) {
        double t = 0; long its = 0; int ok = 0;
        for (int b = 0; b < blocks; b++) {
            std::vector<uint8_t> hard; int it = 0;
            auto t0 = std::chrono::steady_clock::now();
            bool good = mode ? ldpc.decodeFast(llrs[b], 50, hard, &it) : ldpc.decode(llrs[b], 50, hard, &it);
            t += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            its += it; ok += good;
        }
        printf("%s: %.2f ms/block, avg iterations %.1f, converged %d/%d\n", mode ? "layered QC" : "flooding   ", t / blocks, (double)its / blocks, ok, blocks);
    }
    printf("demapper: %.2f ms/block\n", tDem / blocks);
    // GPU batch
    double perBlockGpu = 0;
    {
        auto& g = GpuLdpc::instance();
        if (g.available()) {
            std::vector<float> all((size_t)blocks * d.nLdpc);
            for (int b = 0; b < blocks; b++) std::copy(llrs[b].begin(), llrs[b].end(), all.begin() + (size_t)b * d.nLdpc);
            std::vector<uint8_t> hard((size_t)blocks * d.nLdpc), ok(blocks);
            std::vector<int> it(blocks);
            g.decode(ldpc, all.data(), blocks, 50, hard.data(), ok.data(), it.data()); // warm-up (tables, buffers)
            auto t0 = std::chrono::steady_clock::now();
            g.decode(ldpc, all.data(), blocks, 50, hard.data(), ok.data(), it.data());
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            long its = 0; int good = 0;
            for (int b = 0; b < blocks; b++) { its += it[b]; good += ok[b]; }
            perBlockGpu = ms / blocks;
            printf("GPU batch of %d: %.3f ms/block, avg iterations %.1f, converged %d/%d\n", blocks, perBlockGpu, (double)its / blocks, good, blocks);
        } else printf("GPU: not available\n");
    }
    printf("\nmux needs %.0f blocks/s. With the demapper and BCH included (BCH ~0.3 ms/block):\n", needBlocksPerSec);
    const double cpuPer = 0.0; (void)cpuPer;
}
