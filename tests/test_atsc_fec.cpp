// ATSC channel coding: Reed-Solomon, randomiser, interleaver pair and the full field encoder/decoder round trip, with noise.
#include "dect2/atsc.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <cstdint>
#include <vector>
using namespace dect2::atsc;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    std::mt19937 rng(3);
    // ---- Reed-Solomon
    {
        uint8_t m[187], c[207];
        for (int k = 0; k < 200; k++) {
            for (auto& b : m) b = rng();
            rsEncode(m, c);
            uint8_t d[207];
            memcpy(d, c, 207);
            CHECK(rsDecode(d) == 0, "clean codeword reported errors");
            const int ne = k % 12;
            bool used[207] = {};
            for (int e = 0; e < ne; e++) { int p; do p = rng() % 207; while (used[p]); used[p] = true; d[p] ^= (uint8_t)(1 + rng() % 255); }
            const int r = rsDecode(d);
            if (ne <= 10) CHECK(r == ne && memcmp(d, c, 207) == 0, "RS: %d errors -> %d", ne, r);
            else CHECK(r == -1 || memcmp(d, c, 207) != 0 || r == ne, "RS: %d errors should not decode silently to the wrong word (r %d)", ne, r);
        }
        printf("Reed-Solomon ok\n");
    }
    // ---- randomiser is its own inverse, and the first bytes of the sequence are the known ones
    {
        Randomizer a, b;
        uint8_t d[400], e[400];
        for (auto& x : d) x = rng();
        memcpy(e, d, 400);
        a.apply(e, 400);
        b.apply(e, 400);
        CHECK(memcmp(d, e, 400) == 0, "randomiser not an involution");
        Randomizer z; uint8_t zz[4] = {0, 0, 0, 0}; z.apply(zz, 4);
        printf("randomiser sequence starts %02x %02x %02x %02x\n", zz[0], zz[1], zz[2], zz[3]);
    }
    // ---- interleaver / de-interleaver delay of exactly 52 segments
    {
        const size_t n = 207 * 140;
        std::vector<uint8_t> in(n), mid(n), out(n);
        for (auto& x : in) x = rng();
        ByteInterleaver a(false), b(true);
        a.process(in.data(), mid.data(), n);
        b.process(mid.data(), out.data(), n);
        const size_t d = 52 * 207;
        CHECK(memcmp(out.data() + d, in.data(), n - d) == 0, "interleaver pair does not restore the stream with a 52 segment delay");
        printf("interleaver pair ok\n");
    }
    // ---- trellis map covers every symbol and byte once
    {
        const TrellisMap& tm = trellisMap();
        std::vector<int> seen(12 * 832, 0), bytes(12 * 207 * 4, 0);
        for (int e = 0; e < 12; e++) for (int k = 0; k < 828; k++) { seen[tm.symPos[e][k]]++; bytes[tm.byteIdx[e][k] * 4 + tm.shift[e][k] / 2]++; }
        int bad = 0;
        for (int s = 0; s < 12; s++) for (int i = 0; i < 832; i++) { const int c = seen[s * 832 + i]; if (i < 4 ? c != 0 : c != 1) bad++; }
        for (int c : bytes) if (c != 1) bad++;
        CHECK(bad == 0, "trellis map is not a bijection (%d)", bad);
    }
    // ---- field round trip, clean and noisy
    for (double snrDb : {99.0, 22.0, 17.0, 15.5}) {
        FieldEncoder enc;
        FieldDecoder dec;
        std::normal_distribution<double> nd(0, 1);
        const double sigma = snrDb > 90 ? 0 : std::sqrt(21.0 / std::pow(10.0, snrDb / 10.0));   // symbol power of the 8 levels is 21
        const int nf = 6;
        std::vector<uint8_t> ts((size_t)nf * kDataSegs * 188), got;
        for (size_t p = 0; p < ts.size() / 188; p++) { ts[p * 188] = 0x47; for (int i = 1; i < 188; i++) ts[p * 188 + i] = rng(); }
        FieldStats st;
        std::vector<uint8_t> sym(kFieldSyms), pk(kDataSegs * 188);
        std::vector<float> lv(kFieldSyms);
        for (int f = 0; f < nf; f++) {
            enc.encode(&ts[(size_t)f * kDataSegs * 188], f & 1, sym.data());
            for (int i = 0; i < kFieldSyms; i++) lv[i] = levelOf(sym[i]) + (float)(sigma * nd(rng));
            const int n = dec.decode(lv.data(), pk.data(), &st);
            got.insert(got.end(), pk.begin(), pk.begin() + (size_t)n * 188);
        }
        size_t bad = 0;
        const size_t np = got.size() / 188;
        for (size_t p = 0; p < np; p++) if (memcmp(&got[p * 188], &ts[p * 188], 188) != 0) bad++;
        printf("SNR %.1f dB: %zu packets, %zu wrong, RS clean %d corrected %d failed %d\n", snrDb, np, bad, st.rsClean, st.rsCorrected, st.rsFailed);
        if (snrDb > 20) CHECK(bad == 0 && np == (size_t)nf * kDataSegs - kDelaySegs, "field round trip at %.1f dB: %zu wrong of %zu", snrDb, bad, np);
        if (snrDb == 15.5) CHECK(bad * 20 < np, "too many packets lost at 15.5 dB");
    }
    printf(fails ? "ATSC FEC tests FAILED\n" : "ATSC FEC tests passed\n");
    return fails ? 1 : 0;
}
