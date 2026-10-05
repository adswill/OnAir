// Where does the receiver's error correction fall off a cliff? Frame error rate against SNR for one DVB-T2 mode, comparing the production
// decoder (normalised min-sum, 50 iterations, then a retry pass) with a floating-point sum-product reference with many iterations.
//   bench_threshold [mod 0..3] [rate 0..] [rotation 0|1] [blocks per point] [snr_from snr_to step] [short 0|1]
#include "dect2/t2fec.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
using namespace dect2;

// exact sum-product decoder on the same Tanner graph (flooding schedule), reference only
static bool decodeBp(const LdpcCode& L, const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard) {
    const auto& cs = L.chkStart();
    const auto& cv = L.chkVar();
    const int m = (int)cs.size() - 1, n = (int)llr.size(), E = cs[m];
    std::vector<float> c2v(E, 0.f), v2c(E), total(n);
    hard.assign(n, 0);
    for (int it = 0; it < maxIter; it++) {
        total = llr;
        for (int e = 0; e < E; e++) total[cv[e]] += c2v[e];
        for (int e = 0; e < E; e++) v2c[e] = total[cv[e]] - c2v[e];
        for (int j = 0; j < m; j++) {
            double prod = 1.0;
            const int s = cs[j], t = cs[j + 1];
            std::vector<double> th(t - s);
            for (int e = s; e < t; e++) { th[e - s] = std::tanh(0.5 * std::max(-30.f, std::min(30.f, v2c[e]))); prod *= th[e - s]; }
            for (int e = s; e < t; e++) {
                double x = std::fabs(th[e - s]) > 1e-12 ? prod / th[e - s] : 0.0;
                x = std::max(-0.999999999, std::min(0.999999999, x));
                c2v[e] = (float)(2.0 * std::atanh(x));
            }
        }
        total = llr;
        for (int e = 0; e < E; e++) total[cv[e]] += c2v[e];
        bool ok = true;
        for (int i = 0; i < n; i++) hard[i] = total[i] < 0;
        for (int j = 0; j < m && ok; j++) { int p = 0; for (int e = cs[j]; e < cs[j + 1]; e++) p ^= hard[cv[e]]; if (p) ok = false; }
        if (ok) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    PlpFec f;
    f.shortFrame = argc > 8 && atoi(argv[8]) != 0;
    f.mod = argc > 1 ? atoi(argv[1]) : 2;
    f.rate = argc > 2 ? atoi(argv[2]) : 2;
    f.rotation = argc > 3 ? atoi(argv[3]) != 0 : true;
    const int blocks = argc > 4 ? atoi(argv[4]) : 40;
    const float s0 = argc > 5 ? (float)atof(argv[5]) : 11.5f, s1 = argc > 6 ? (float)atof(argv[6]) : 14.f, ds = argc > 7 ? (float)atof(argv[7]) : 0.25f;
    const FecDims d = fecDims(f);
    const LdpcCode& ldpc = ldpcFor(f);
    const BchCode& bch = bchFor(f);
    const auto& map = bitInterleaverMap(f);
    const int bps = d.bitsPerCell, cells = d.cellsPerBlock;
    printf("%s mod %d rate %d rot %d: %d blocks per point\n   SNR   production   sum-product(100)\n", f.shortFrame ? "short" : "normal", f.mod, f.rate, (int)f.rotation, blocks);
    for (float snr = s0; snr <= s1 + 1e-4f; snr += ds) {
        std::atomic<int> fails{0}, failsBp{0}, next{0};
        auto worker = [&](unsigned seed) {
            std::mt19937 rng(seed);
            std::normal_distribution<float> nd(0.f, 1.f);
            for (;;) {
                const int b = next++;
                if (b >= blocks) return;
                std::vector<uint8_t> msg(d.kBch);
                for (auto& v : msg) v = rng() & 1;
                std::vector<uint8_t> bits = msg;
                bch.encode(bits, d.kBch);
                ldpc.encode(bits);
                std::vector<uint16_t> lab(cells);
                for (int c = 0; c < cells; c++) { unsigned l = 0; for (int k = 0; k < bps; k++) l = (l << 1) | bits[map[c * bps + k]]; lab[c] = (uint16_t)l; }
                std::vector<cf32> tx;
                qamMapBlock(f, lab, tx);
                const float n0 = std::pow(10.f, -snr / 10.f) / 2.f;
                std::vector<cf32> rx(tx.size());
                std::vector<float> nv(tx.size(), n0);
                for (size_t i = 0; i < tx.size(); i++) rx[i] = tx[i] + cf32(nd(rng), nd(rng)) * std::sqrt(n0);
                std::vector<float> llrLab((size_t)cells * bps), llr(d.nLdpc);
                qamDemapBlock(f, rx.data(), nv.data(), cells, llrLab.data());
                for (int p = 0; p < d.nLdpc; p++) llr[map[p]] = llrLab[p];
                auto good = [&](const std::vector<uint8_t>& h) {
                    std::vector<uint8_t> bb(h.begin(), h.begin() + d.kLdpc);
                    if (bch.decode(bb) < 0) return false;
                    for (int i = 0; i < d.kBch; i++) if (bb[i] != msg[i]) return false;
                    return true;
                };
                std::vector<uint8_t> h;
                // production: first pass, then the retry passes of t2plp.cpp
                ldpc.decodeFast(llr, 50, h);
                bool ok = good(h);
                static const float kAlpha[3] = {0.78f, 0.66f, 0.9f};
                for (int a = 0; a < 3 && !ok; a++) { ldpc.decodeFast(llr, 150, h, nullptr, kAlpha[a]); ok = good(h); }
                if (!ok) fails++;
                if (!decodeBp(ldpc, llr, 100, h) || !good(h)) failsBp++;
            }
        };
        std::vector<std::thread> th;
        const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned t = 0; t < nt; t++) th.emplace_back(worker, 100 + t);
        for (auto& t : th) t.join();
        printf("%6.2f   %3d/%d      %3d/%d\n", snr, fails.load(), blocks, failsBp.load(), blocks);
        fflush(stdout);
    }
}
