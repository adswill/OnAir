// ATSC 3.0 Preamble OFDM layer: pilot sequence, cell counts against A/322 Table 7.2, the frequency interleaver, and a round trip
// through modulation, noise and demodulation.
#include "dect2/atsc3_ofdm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

struct Row { int fft, gi, dx; int cells[5]; };
static const Row kTable72[] = {
    {8192, 192, 16, {6432, 6342, 6253, 6164, 6075}},
    {8192, 384, 8, {6000, 5916, 5833, 5750, 5667}},
    {8192, 512, 6, {5712, 5632, 5553, 5474, 5395}},
    {8192, 768, 4, {5136, 5064, 4993, 4922, 4851}},
    {8192, 1024, 3, {4560, 4496, 4433, 4370, 4307}},
    {8192, 1536, 4, {5136, 5064, 4993, 4922, 4851}},
    {8192, 2048, 3, {4560, 4496, 4433, 4370, 4307}},
    {16384, 192, 32, {13296, 13110, 12927, 12742, 12558}},
    {16384, 384, 16, {12864, 12684, 12507, 12328, 12150}},
    {16384, 512, 12, {12576, 12400, 12227, 12052, 11878}},
    {16384, 768, 8, {12000, 11832, 11667, 11500, 11334}},
    {16384, 1024, 6, {11424, 11264, 11107, 10948, 10790}},
    {16384, 1536, 4, {10272, 10128, 9987, 9844, 9702}},
    {16384, 2048, 3, {9120, 8992, 8867, 8740, 8614}},
    {16384, 2432, 3, {9120, 8992, 8867, 8740, 8614}},
    {16384, 3072, 4, {10272, 10128, 9987, 9844, 9702}},
    {16384, 3648, 4, {10272, 10128, 9987, 9844, 9702}},
    {16384, 4096, 3, {9120, 8992, 8867, 8740, 8614}},
    {32768, 192, 32, {26592, 26220, 25854, 25484, 25116}},
    {32768, 384, 32, {26592, 26220, 25854, 25484, 25116}},
    {32768, 512, 24, {26304, 25936, 25574, 25208, 24844}},
    {32768, 768, 16, {25728, 25368, 25014, 24656, 24300}},
    {32768, 1024, 12, {25152, 24800, 24454, 24104, 23756}},
    {32768, 1536, 8, {24000, 23664, 23334, 23000, 22668}},
    {32768, 2048, 6, {22848, 22528, 22214, 21896, 21580}},
    {32768, 2432, 6, {22848, 22528, 22214, 21896, 21580}},
    {32768, 3072, 8, {24000, 23664, 23334, 23000, 22668}},
    {32768, 3072, 3, {18240, 17984, 17734, 17480, 17228}},
    {32768, 3648, 8, {24000, 23664, 23334, 23000, 22668}},
    {32768, 3648, 3, {18240, 17984, 17734, 17480, 17228}},
    {32768, 4096, 3, {18240, 17984, 17734, 17480, 17228}},
    {32768, 4864, 3, {18240, 17984, 17734, 17480, 17228}}
};

int main() {
    // reference sequence: the first 24 values are given in A/322 8.1.2
    auto r = referenceSequence(24);
    const char* want = "110110000000000101000000";
    bool ok = true;
    for (int i = 0; i < 24; i++) ok &= (r[i] == want[i] - '0');
    CHECK(ok, "reference sequence");

    // preamble_structure values of Table H.1.1
    PreambleParams p;
    CHECK(preambleParams(0, p) && p.fftSize == 8192 && p.guard == 192 && p.dx == 16 && p.l1BasicMode == 1, "ps 0");
    CHECK(preambleParams(9, p) && p.fftSize == 8192 && p.guard == 384 && p.dx == 8 && p.l1BasicMode == 5, "ps 9");
    CHECK(preambleParams(34, p) && p.fftSize == 8192 && p.guard == 2048 && p.dx == 3 && p.l1BasicMode == 5, "ps 34");
    CHECK(preambleParams(35, p) && p.fftSize == 16384 && p.guard == 192 && p.dx == 32 && p.l1BasicMode == 1, "ps 35");
    CHECK(preambleParams(89, p) && p.fftSize == 16384 && p.guard == 4096 && p.dx == 3 && p.l1BasicMode == 5, "ps 89");
    CHECK(preambleParams(90, p) && p.fftSize == 32768 && p.guard == 192 && p.dx == 32 && p.l1BasicMode == 1, "ps 90");
    CHECK(preambleParams(135, p) && p.fftSize == 32768 && p.guard == 3072 && p.dx == 3, "ps 135");
    CHECK(preambleParams(159, p) && p.fftSize == 32768 && p.guard == 4864 && p.dx == 3 && p.l1BasicMode == 5, "ps 159");
    CHECK(!preambleParams(160, p), "ps 160 is reserved");

    // available data cells per Preamble symbol (Table 7.2) for every row and every carrier reduction
    int bad = 0;
    for (auto& row : kTable72) {
        PreambleParams q;
        q.fftSize = row.fft; q.guard = row.gi; q.dx = row.dx;
        for (int c = 0; c < 5; c++)
            if ((int)preambleDataCarriers(q, c).size() != row.cells[c]) {
                printf("  Table 7.2: FFT %d GI %d DX %d Cred %d: got %d, expected %d\n", row.fft, row.gi, row.dx, c, (int)preambleDataCarriers(q, c).size(), row.cells[c]);
                bad++;
            }
    }
    CHECK(bad == 0, "cell counts of Table 7.2");

    // the common continual pilots: 48/96/192 without carrier reduction, and the counts of Table 8.4
    CHECK((int)continualPilots(8192, 0).size() == 48 && (int)continualPilots(8192, 2).size() == 47 && (int)continualPilots(8192, 4).size() == 45, "CP counts 8K");
    CHECK((int)continualPilots(16384, 0).size() == 96 && (int)continualPilots(16384, 3).size() == 92, "CP counts 16K");
    CHECK((int)continualPilots(32768, 0).size() == 192 && (int)continualPilots(32768, 4).size() == 180, "CP counts 32K");

    // the interleaver is a permutation, and it changes from symbol to symbol
    for (int fft : {8192, 16384, 32768}) {
        PreambleParams q; q.fftSize = fft; q.guard = 192; q.dx = fft == 8192 ? 16 : 32;
        int n = (int)preambleDataCarriers(q, 4).size();
        for (int l = 0; l < 3; l++) {
            auto h = frequencyInterleaverSequence(fft, n, l);
            std::vector<char> seen(n, 0);
            bool perm = (int)h.size() == n;
            for (int v : h) { if (v < 0 || v >= n || seen[v]) perm = false; else seen[v] = 1; }
            char m[80]; snprintf(m, sizeof m, "interleaver is a permutation (FFT %d, symbol %d)", fft, l);
            CHECK(perm, m);
        }
        CHECK(frequencyInterleaverSequence(fft, n, 0) != frequencyInterleaverSequence(fft, n, 2), "offset changes every two symbols");
        CHECK(frequencyInterleaverSequence(fft, n, 2) == frequencyInterleaverSequence(fft, n, 2), "deterministic");
    }

    // round trip through the channel: random QPSK, noise, and an echo
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.f, 1.f);
    struct Case { int ps; int cred; int sym; double snr; bool echo; } cases[] = {
        {0, 4, 0, 30, false}, {0, 4, 1, 15, false}, {30, 4, 1, 20, true}, {35, 4, 0, 15, false}, {75, 3, 2, 12, true}, {95, 4, 0, 15, false}, {95, 4, 1, 15, false}, {130, 2, 3, 15, true}};
    for (auto& cs : cases) {
        PreambleParams q;
        preambleParams(cs.ps, q);
        int n = (int)preambleDataCarriers(q, cs.cred).size();
        std::vector<cf32> cells(n);
        std::vector<int> bits(2 * n);
        for (int i = 0; i < n; i++) {
            int b0 = rng() & 1, b1 = rng() & 1;
            bits[2 * i] = b0; bits[2 * i + 1] = b1;
            cells[i] = cf32(b0 ? -0.7071f : 0.7071f, b1 ? -0.7071f : 0.7071f);
        }
        auto tx = modulatePreambleSymbol(q, cs.cred, cs.sym, cells);
        double pw = 0;
        for (auto& v : tx) pw += std::norm(v);
        pw /= tx.size();
        char m[120];
        snprintf(m, sizeof m, "unit power (ps %d)", cs.ps);
        CHECK(std::fabs(pw - 1.0) < 0.05, m);
        std::vector<cf32> rx(tx.size() + 200, cf32(0, 0));
        for (size_t i = 0; i < tx.size(); i++) rx[i] += tx[i];
        if (cs.echo)   // an echo inside the guard interval, 10 dB down
            for (size_t i = 0; i + 40 < tx.size(); i++) rx[i + 40] += tx[i] * cf32(0.25f, 0.2f);
        double sig = 0;
        for (size_t i = 0; i < tx.size(); i++) sig += std::norm(rx[i]);
        sig /= tx.size();
        double sigma = std::sqrt(sig / std::pow(10.0, cs.snr / 10.0) / 2.0);
        for (auto& v : rx) v += cf32(g(rng), g(rng)) * (float)sigma;
        std::vector<cf32> out;
        float nv = 0;
        bool okd = demodulatePreambleSymbol(q, cs.cred, cs.sym, rx.data(), out, &nv);
        CHECK(okd && (int)out.size() == n, "demodulate");
        if (!okd) continue;
        int errs = 0;
        for (int i = 0; i < n; i++) {
            errs += ((out[i].real() < 0) != (bits[2 * i] == 1));
            errs += ((out[i].imag() < 0) != (bits[2 * i + 1] == 1));
        }
        double ber = (double)errs / (2.0 * n);
        printf("  ps %3d  FFT %5d  GI %4d  sym %d  SNR %2.0f dB%s: %d cells, bit errors %.4f, noise estimate %.3f\n", cs.ps, q.fftSize, q.guard, cs.sym, cs.snr, cs.echo ? " +echo" : "", n, ber, nv);
        double limit = cs.snr >= 25 ? 0.0 : cs.snr >= 15 ? 0.02 : 0.06;
        snprintf(m, sizeof m, "round trip bit errors (ps %d, symbol %d)", cs.ps, cs.sym);
        CHECK(ber <= limit, m);
    }
    printf(fails ? "atsc3 ofdm: FAILED\n" : "atsc3 ofdm: ok\n");
    return fails ? 1 : 0;
}
