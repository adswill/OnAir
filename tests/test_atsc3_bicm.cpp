// ATSC 3.0 BICM (FEC frame to cells and back): block interleaver against the examples of A/322, constellation power, and decoding through noise.
#include "dect2/atsc3_bicm.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    // the examples of A/322 6.2.3.1: 256QAM, 64800 bits, Type A
    auto a = blockInterleaverMap(64800, 8, 0);
    CHECK(a[0] == 0 && a[1] == 7920 && a[2] == 15840 && a[7] == 55440 && a[8] == 1 && a[9] == 7921, "type A, first output bits");
    CHECK(a[63357] == 47519 && a[63358] == 55439 && a[63359] == 63359 && a[63360] == 63360 && a[63361] == 63540, "type A, the end of part 1 and start of part 2");
    CHECK(a[64799] == 64799, "type A, last bit");
    for (int eta : {2, 4, 6, 8, 10, 12}) for (int t = 0; t < 2; t++) {
        auto m = blockInterleaverMap(64800, eta, t);
        std::vector<char> seen(64800, 0);
        bool perm = true;
        for (int v : m) { if (v < 0 || v >= 64800 || seen[v]) perm = false; else seen[v] = 1; }
        char msg[80]; snprintf(msg, sizeof msg, "block interleaver is a permutation (64800, %d, type %d)", eta, t);
        CHECK(perm, msg);
    }
    for (int eta : {2, 4, 6, 8}) for (int t = 0; t < 2; t++) {
        auto m = blockInterleaverMap(16200, eta, t);
        std::vector<char> seen(16200, 0);
        bool perm = true;
        for (int v : m) { if (v < 0 || v >= 16200 || seen[v]) perm = false; else seen[v] = 1; }
        char msg[80]; snprintf(msg, sizeof msg, "block interleaver is a permutation (16200, %d, type %d)", eta, t);
        CHECK(perm, msg);
    }
    auto b = blockInterleaverMap(64800, 8, 1);
    CHECK(b[0] == 0 && b[1] == 360 && b[7] == 2520 && b[8] == 1 && b[63360] == 63360, "type B 256QAM");

    std::mt19937 rng(17);
    std::normal_distribution<float> g(0.f, 1.f);
    struct T { int n, rate, eta, outer; double snr; } tests[] = {
        {64800, 8, 2, 0, 3}, {64800, 8, 4, 0, 8}, {64800, 8, 6, 1, 12}, {64800, 10, 8, 0, 20}, {64800, 9, 10, 2, 22}, {64800, 8, 12, 0, 26},
        {16200, 6, 2, 0, 3}, {16200, 5, 4, 1, 9}, {16200, 8, 6, 0, 14}, {16200, 10, 8, 0, 22}, {64800, 3, 2, 0, -2}, {64800, 13, 8, 0, 29}};
    for (auto& t : tests) {
        BicmConfig c; c.nInner = t.n; c.rate15 = t.rate; c.bitsPerCell = t.eta; c.outer = t.outer;
        Bicm bicm(c);
        char m[120];
        snprintf(m, sizeof m, "BICM %d/%d/%d exists", t.n, t.rate, t.eta);
        CHECK(bicm.ok(), m);
        if (!bicm.ok()) continue;
        double pw = 0;
        for (auto& p : bicm.constellation()) pw += std::norm(p);
        pw /= bicm.constellation().size();
        snprintf(m, sizeof m, "unit power constellation (%d/%d/%d: %.4f)", t.n, t.rate, t.eta, pw);
        CHECK(std::fabs(pw - 1.0) < 0.01, m);
        std::vector<uint8_t> in(bicm.kPayload());
        for (auto& v : in) v = rng() & 1;
        auto cells = bicm.encode(in);
        CHECK((int)cells.size() == t.n / t.eta, "cell count");
        float sigma = (float)std::sqrt(std::pow(10.0, -t.snr / 10.0) / 2.0);
        for (auto& v : cells) v += cf32(g(rng), g(rng)) * sigma;
        std::vector<uint8_t> out;
        int it = 0;
        bool ok = bicm.decode(cells.data(), 2 * sigma * sigma, out, &it);
        printf("  n %5d  rate %2d/15  %4d-point  outer %d  SNR %+3.0f dB: %s (%d iterations)\n", t.n, t.rate, 1 << t.eta, t.outer, t.snr, ok && out == in ? "ok" : "FAILED", it);
        snprintf(m, sizeof m, "decode %d/%d/%d", t.n, t.rate, t.eta);
        CHECK(ok && out == in, m);
    }
    printf(fails ? "atsc3 bicm: FAILED\n" : "atsc3 bicm: ok\n");
    return fails ? 1 : 0;
}
