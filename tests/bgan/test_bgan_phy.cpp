// BGAN physical layer: all published tables load, encode / decode round trips at a BER the code must reach, frame layout counts.
#include "dect2/bgan/bgan_phy.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2; using namespace dect2::bgan;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)
int main() {
    CHECK(tableCount() == 118, "tables %d", tableCount());
    CHECK(bearerCount() == 13, "bearers");
    for (int i = 0; i < bearerCount(); i++) {
        const Bearer& b = bearer(i);
        CHECK(b.mos == b.uw + b.ps + b.teo * b.blocks, "%s frame count", b.name);
        FrameLayout f = frameLayout(b);
        CHECK(int(f.pilotPos.size()) == b.ps && int(f.dataPos.size()) == b.teo * b.blocks, "%s layout", b.name);
        CHECK(std::abs(double(b.mos) / b.symRate - 0.08) < 1e-9, "%s 80 ms", b.name);
    }
    for (int m = 0; m < 4; m++) { int n; const cf32* c = constellation((Mod)m, n); double p = 0; for (int i = 0; i < n; i++) p += std::norm(c[i]); CHECK(std::abs(p / n - 1) < 0.01, "power mod %d %.3f", m, p / n); }
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0, 1);
    TurboDecoder dec;
    int tested = 0;
    for (int fam = 0; fam < 12; fam++) for (int st = 0; st < kSubtypes; st++) {
        const Table* t = table(fam, st);
        if (!t) continue;
        tested++;
        Mod m = t->bps == 2 ? Mod::Q4 : t->bps == 4 ? Mod::Q16 : t->bps == 5 ? Mod::Q32 : Mod::Q64;
        std::vector<uint8_t> in(t->dataBytes());
        for (auto& v : in) v = uint8_t(rng());
        std::vector<cf32> s(t->symbols);
        encodeBlock(*t, m, in.data(), s.data());
        // noise-free decode: must be perfect in one iteration or two
        std::vector<float> llr(t->symbols * t->bps);
        for (int i = 0; i < t->symbols; i++) softDemap(m, s[i], 0.1f, &llr[i * t->bps]);
        std::vector<uint8_t> out;
        dec.decode(*t, llr.data(), out, 2);
        CHECK(out == in, "fam %d %s clean decode", fam, subtypeName(st));
        // moderate noise: Es/N0 of the rate plus a margin, 8 iterations
        const double rate = double(t->dataBits()) / (t->symbols * t->bps);
        const double esn0 = 10 * std::log10((std::pow(2.0, 2 * rate * t->bps / 2) - 1)) + 3.0;
        const float nv = float(std::pow(10.0, -esn0 / 10));
        for (int i = 0; i < t->symbols; i++) { cf32 y = s[i] + cf32(g(rng), g(rng)) * std::sqrt(nv / 2); softDemap(m, y, nv, &llr[i * t->bps]); }
        dec.decode(*t, llr.data(), out, 8);
        int err = 0;
        for (size_t i = 0; i < in.size(); i++) err += __builtin_popcount(in[i] ^ out[i]);
        if (err) { std::printf("fam %d %s esn0 %.1f rate %.2f: %d bit errors\n", fam, subtypeName(st), esn0, rate, err); fails++; }
    }
    CHECK(tested == 118, "tested %d", tested);
    std::printf(fails ? "bgan phy: %d FAILED\n" : "bgan phy: all passed\n", fails);
    return fails ? 1 : 0;
}
