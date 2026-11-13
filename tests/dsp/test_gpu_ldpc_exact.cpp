// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The GPU LDPC decoder for the DVB-T2 / DVB-S2 codes (GpuLdpc::decode: Metal, Direct3D 11 or Vulkan) against a scalar CPU version of the same
// layered normalised min-sum, on identical soft inputs: the hard decisions, the ok flag and the iteration count must agree to the bit for every
// block (clean, near the threshold, failing). Skipped when no GPU backend is usable (on Linux, DECT2_VULKAN=1 also accepts lavapipe).
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/common/stable_random.h"
#include "dect2/t2/t2fec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;

namespace {

constexpr int Z = 360;

// the kernel's schedule on one block: every message of a layer from the posteriors at the start of the layer, then the increments, edges
// that meet the same variable group one rank after the other
struct Reference {
    const LdpcCode& c;
    std::vector<int> ls, cg, cs, rk, lr;
    std::vector<uint32_t> mb;
    uint32_t msgPer = 0, postPer = 0;
    explicit Reference(const LdpcCode& code) : c(code) {
        ls.push_back(0);
        for (auto& l : c.layers()) {
            int maxRank = 0;
            for (size_t i = 0; i < l.size(); i++) {
                int rank = 0;
                for (size_t k = 0; k < i; k++) if (l[k].group == l[i].group) rank++;
                maxRank = std::max(maxRank, rank);
                cg.push_back(l[i].group); cs.push_back(l[i].shift); rk.push_back(rank);
            }
            lr.push_back(maxRank);
            ls.push_back((int)cg.size());
            mb.push_back(msgPer);
            msgPer += (uint32_t)(l.size() + 2) * Z;
        }
        postPer = (uint32_t)(c.groups() * Z + c.q() * (Z + 1));
    }
    int vidx(int j, int nc0, int c0, int r, int m) const {
        if (j < nc0) { int nn = m - cs[(size_t)(c0 + j)]; if (nn < 0) nn += Z; return cg[(size_t)(c0 + j)] * Z + nn; }
        const int pb = c.groups() * Z;
        if (j == nc0) return pb + r * (Z + 1) + 1 + m;
        return pb + (r > 0 ? (r - 1) * (Z + 1) + 1 + m : (c.q() - 1) * (Z + 1) + m);
    }
    static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
    static float flt(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

    void decode(const float* llr, int maxIter, float alpha, uint8_t* hard, bool& ok, int& iters) const {
        const int q = c.q(), G = c.groups(), pb = G * Z, k = c.k();
        std::vector<float> post(postPer), msg(msgPer, 0.f), d(64);
        std::vector<float> dl((size_t)Z * 64);
        for (int i = 0; i < G * Z; i++) post[(size_t)i] = llr[i];
        for (int r = 0; r < q; r++) {
            for (int m = 0; m < Z; m++) post[(size_t)(pb + r * (Z + 1) + 1 + m)] = llr[k + r + q * m];
            post[(size_t)(pb + r * (Z + 1))] = 1e4f;
        }
        int it = 0;
        ok = false;
        while (it < maxIter) {
            for (int r = 0; r < q; r++) {
                const int c0 = ls[(size_t)r], nc0 = ls[(size_t)r + 1] - c0, nc = nc0 + 2;
                const uint32_t mr = mb[(size_t)r];
                for (int m = 0; m < Z; m++) {
                    float min1 = 1e30f, min2 = 1e30f;
                    int idx1 = -1;
                    uint32_t sg = 0;
                    for (int j = 0; j < nc; j++) {
                        const float x = post[(size_t)vidx(j, nc0, c0, r, m)] - msg[mr + (size_t)(j * Z + m)];
                        const float a = std::fabs(x);
                        sg ^= bits(x) & 0x80000000u;
                        if (a < min1) { min2 = min1; min1 = a; idx1 = j; }
                        else if (a < min2) min2 = a;
                    }
                    for (int j = 0; j < nc; j++) {
                        const float x = post[(size_t)vidx(j, nc0, c0, r, m)] - msg[mr + (size_t)(j * Z + m)];
                        const float mag = (idx1 == j ? min2 : min1) * alpha;
                        const float nm = flt(bits(mag) | (sg ^ (bits(x) & 0x80000000u)));
                        dl[(size_t)m * 64 + (size_t)j] = nm - msg[mr + (size_t)(j * Z + m)];
                        msg[mr + (size_t)(j * Z + m)] = nm;
                    }
                }
                for (int rank = 0; rank <= lr[(size_t)r]; rank++)
                    for (int m = 0; m < Z; m++)
                        for (int j = 0; j < nc; j++) {
                            if ((j < nc0 ? rk[(size_t)(c0 + j)] : 0) != rank) continue;
                            if (r == 0 && j == nc0 + 1 && m == 0) continue;   // the pad variable stays known
                            const size_t vi = (size_t)vidx(j, nc0, c0, r, m);
                            post[vi] = post[vi] + dl[(size_t)m * 64 + (size_t)j];
                        }
            }
            it++;
            bool bad = false;
            for (int r = 0; r < q && !bad; r++) {
                const int c0 = ls[(size_t)r], nc0 = ls[(size_t)r + 1] - c0, nc = nc0 + 2;
                for (int m = 0; m < Z && !bad; m++) {
                    int par = 0;
                    for (int j = 0; j < nc; j++) par ^= post[(size_t)vidx(j, nc0, c0, r, m)] < 0.f;
                    bad = par != 0;
                }
            }
            ok = !bad;
            if (ok) break;
        }
        iters = it;
        for (int i = 0; i < G * Z; i++) hard[i] = post[(size_t)i] < 0.f;
        for (int r = 0; r < q; r++)
            for (int m = 0; m < Z; m++) hard[k + r + q * m] = post[(size_t)(pb + r * (Z + 1) + 1 + m)] < 0.f;
    }
};

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

} // namespace

int main() {
    GpuLdpc& g = GpuLdpc::instance();
    if (!g.available()) { printf("gpu_ldpc_exact: SKIPPED (no usable GPU backend)\n"); return 0; }
    printf("GPU: %s\n", g.deviceName());
    int failures = 0;
    std::mt19937 rng(5);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    struct Case { bool shortF; int rate; double snrDb; };
    // around each rate's threshold (some blocks decode, some need many iterations, some fail) and one clean point
    const Case cases[] = {{true, 0, 0.9}, {true, 2, 2.7}, {true, 4, 4.4}, {true, 5, 20.0}, {false, 1, 2.3}, {false, 3, 4.1}, {false, 5, 9.0}};
    for (const Case& cs : cases) {
        PlpFec f; f.shortFrame = cs.shortF; f.rate = cs.rate; f.mod = 2; f.rotation = false;
        const LdpcCode& c = ldpcFor(f);
        const int n = c.n(), k = c.k(), nb = cs.shortF ? 24 : 8;
        const double sigma = std::pow(10.0, -cs.snrDb / 20.0);
        std::vector<float> llr((size_t)nb * n);
        for (int b = 0; b < nb; b++) {
            std::vector<uint8_t> cw((size_t)n);
            for (int i = 0; i < k; i++) cw[(size_t)i] = rng() & 1;
            c.encode(cw);
            for (int i = 0; i < n; i++) { const float x = (cw[(size_t)i] ? -1.f : 1.f) + (float)sigma * nd(rng); llr[(size_t)b * n + i] = 2.f * x / (float)(sigma * sigma); }
        }
        std::vector<uint8_t> hard((size_t)nb * n), ok((size_t)nb);
        std::vector<int> it((size_t)nb);
        double t0 = now();
        const bool called = g.decode(c, llr.data(), nb, 30, hard.data(), ok.data(), it.data());
        const double tg = now() - t0;
        if (!called) { printf("FAIL: %s rate %d: decode returned false\n", cs.shortF ? "short" : "normal", cs.rate); failures++; continue; }
        Reference ref(c);
        std::vector<uint8_t> rh((size_t)n);
        int mism = 0, okC = 0, okG = 0;
        long itSum = 0;
        t0 = now();
        for (int b = 0; b < nb; b++) {
            bool rok; int rit;
            ref.decode(&llr[(size_t)b * n], 30, 0.78f, rh.data(), rok, rit);
            okC += rok; okG += ok[(size_t)b]; itSum += rit;
            if (rok != (bool)ok[(size_t)b] || rit != it[(size_t)b] || std::memcmp(rh.data(), &hard[(size_t)b * n], (size_t)n) != 0) {
                if (mism < 3) printf("  block %d: ok %d/%d, iterations %d/%d (reference/GPU)\n", b, (int)rok, (int)ok[(size_t)b], rit, it[(size_t)b]);
                mism++;
            }
        }
        const double tc = now() - t0;
        printf("  %s rate %d at %.1f dB: %d blocks, ok reference %d GPU %d, mean iterations %.1f, mismatching blocks %d | GPU %.0f ms, reference %.0f ms\n",
               cs.shortF ? "short" : "normal", cs.rate, cs.snrDb, nb, okC, okG, (double)itSum / nb, mism, tg * 1e3, tc * 1e3);
        if (mism) failures++;
    }
    printf(failures ? "gpu_ldpc_exact: %d FAILED\n" : "gpu_ldpc_exact: all passed\n", failures);
    return failures ? 1 : 0;
}
