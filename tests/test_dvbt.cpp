// DVB-T: Reed-Solomon, TPS, Viterbi and the whole coding chain (TS -> cells -> noise -> TS) for several modes.
#include "dect2/dvbt_fec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <cstdint>
#include <cstdlib>
#include <vector>
using namespace dect2::dvbt;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void testRs() {
    std::mt19937 rng(3);
    int bad = 0;
    for (int trial = 0; trial < 300; trial++) {
        uint8_t msg[188], cw[204], rx[204];
        for (auto& b : msg) b = rng();
        rsEncode(msg, cw);
        memcpy(rx, cw, 204);
        const int nerr = trial % 10; // 0..9 errors
        int used[9];
        for (int e = 0; e < nerr; e++) {
            int pos; bool dup;
            do { pos = rng() % 204; dup = false; for (int j = 0; j < e; j++) dup |= used[j] == pos; } while (dup);
            used[e] = pos;
            rx[pos] ^= (uint8_t)(1 + rng() % 255);
        }
        const int r = rsDecode(rx);
        if (nerr <= 8) { if (r != nerr || memcmp(rx, cw, 204) != 0) bad++; }
        else if (r >= 0 && memcmp(rx, cw, 204) == 0) bad++; // 9 errors must not "correct" to the right word
    }
    CHECK(bad == 0, "RS: %d trials failed", bad);
    printf("Reed-Solomon (204,188): up to 8 errors corrected, 9 detected\n");
}

static void testTps() {
    Params p; p.mode = k8K; p.guard = kGi8; p.mod = k16Qam; p.hier = 0; p.crHp = kR34; p.crLp = kR34; p.cellId = 0x1234; p.cellIdLength = true;
    for (int f = 0; f < 4; f++) {
        auto b = tpsBits(p, f);
        Params q; int fi = -1; bool odd = false;
        CHECK(tpsDecode(b.data(), q, fi, odd), "TPS frame %d does not decode", f);
        CHECK(fi == f && q.mode == k8K && q.guard == kGi8 && q.mod == k16Qam && q.crHp == kR34 && q.hier == 0, "TPS fields frame %d", f);
        b[40] ^= 1; // a corrupted BCH-protected bit must be rejected
        CHECK(!tpsDecode(b.data(), q, fi, odd), "TPS accepted a corrupted block");
        // ... unless correction is asked for: the BCH code fixes one or two errors anywhere in s1..s67, sync word included
        for (int e2 : {-1, 3, 66}) {
            auto c = tpsBits(p, f);
            c[40] ^= 1;
            if (e2 >= 0) c[e2] ^= 1;
            Params r; int fr = -1; bool o2 = false;
            CHECK(tpsDecode(c.data(), r, fr, o2, 2) && fr == f && r.mod == k16Qam && r.crHp == kR34 && r.guard == kGi8,
                  "TPS frame %d with errors at 40 and %d not corrected", f, e2);
        }
    }
    printf("TPS encode/decode, sync words and BCH checked\n");
}

// whole coding chain
static bool chain(const Params& p, double esn0Db, int symbols, double* decodeSecs = nullptr, FecStats* statsOut = nullptr, int phase = 0) {
    const int N = dataCarriers(p.mode), m = bitsPerCell(p.mod);
    const int k = std::vector<int>{1, 2, 3, 5, 7}[p.crHp], n = std::vector<int>{2, 3, 4, 6, 8}[p.crHp];
    const size_t codedBits = (size_t)symbols * N * m;
    const size_t infoBits = codedBits * k / n;
    const size_t packets = infoBits / 8 / 204 / 8 * 8; // whole scrambling groups
    std::mt19937 rng(11);
    std::vector<uint8_t> ts(packets * 188);
    for (size_t i = 0; i < packets; i++) { ts[i * 188] = 0x47; for (int j = 1; j < 188; j++) ts[i * 188 + j] = rng(); ts[i * 188 + 1] &= 0x7F; }
    std::vector<uint8_t> sc(ts.size()), rs(packets * 204), il(rs.size());
    scramble(ts.data(), packets, sc.data());
    for (size_t i = 0; i < packets; i++) rsEncode(&sc[i * 188], &rs[i * 204]);
    ConvInterleaver ci(false);
    ci.process(rs.data(), il.data(), il.size());
    std::vector<uint8_t> bits(il.size() * 8), coded;
    for (size_t i = 0; i < il.size(); i++) for (int j = 0; j < 8; j++) bits[i * 8 + j] = (il[i] >> (7 - j)) & 1;
    InnerEncoder enc(p.crHp);
    enc.setPhase(phase);
    enc.encode(bits, coded);
    coded.resize(codedBits, 0);
    std::vector<uint8_t> words;
    bitInterleave(coded, p.mod, words);
    FecDecoder dec;
    dec.configure(p);
    const double sigma2 = std::pow(10.0, -esn0Db / 10.0) / 2.0; // per real dimension, unit-power constellation
    std::normal_distribution<float> nd(0, 1);
    std::vector<cf32> cells, inter(N), noisy(N);
    std::vector<float> n0(N, (float)sigma2);
    const auto& H = symbolPermutation(p.mode);
    auto t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < symbols; s++) {
        std::vector<uint8_t> w(words.begin() + (size_t)s * N, words.begin() + (size_t)(s + 1) * N);
        mapSymbol(w, p.mod, p.hier, cells);
        const int si = s % 68;
        if (si % 2) for (int q = 0; q < N; q++) inter[q] = cells[H[q]]; else for (int q = 0; q < N; q++) inter[H[q]] = cells[q];
        for (int q = 0; q < N; q++) noisy[q] = inter[q] + cf32(nd(rng), nd(rng)) * (float)std::sqrt(sigma2);
        dec.pushSymbol(noisy.data(), n0.data(), si);
    }
    if (decodeSecs) *decodeSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<uint8_t> out;
    dec.takePackets(out);
    if (statsOut) *statsOut = dec.stats();
    // the output is a contiguous run of packets of the original stream: find where it starts and count mismatches
    const size_t got = out.size() / 188;
    if (got < packets / 2) return false;
    size_t start = 0;
    bool found = false;
    for (size_t s = 0; s + 4 < packets && !found; s++) if (memcmp(&out[0], &ts[s * 188], 188) == 0 && memcmp(&out[188 * 3], &ts[(s + 3) * 188], 188) == 0) { start = s; found = true; }
    if (!found) return false;
    size_t good = 0, tot = 0;
    for (size_t i = 0; i + 8 < got && start + i < packets; i++) { tot++; good += memcmp(&out[i * 188], &ts[(start + i) * 188], 188) == 0; }
    if (getenv("DVBT_DEBUG")) {
        printf("   output %zu packets, original run starts at %zu of %zu; mismatches at output index:", got, start, packets);
        int shown = 0;
        for (size_t i = 0; i + 8 < got && start + i < packets; i++) if (memcmp(&out[i * 188], &ts[(start + i) * 188], 188) != 0 && shown++ < 12) printf(" %zu", i);
        printf("\n");
    }
    return good == tot && tot > packets / 2;
}

int main() {
    testRs();
    testTps();
    struct C { int mode, mod, cr; double snr; const char* name; } cases[] = {
        {k2K, kQpsk, kR12, 6.0, "2K QPSK 1/2"},   {k2K, kQpsk, kR56, 12.0, "2K QPSK 5/6"},  {k2K, k16Qam, kR23, 16.0, "2K 16-QAM 2/3"},
        {k2K, k64Qam, kR34, 23.0, "2K 64-QAM 3/4"}, {k2K, k64Qam, kR78, 28.0, "2K 64-QAM 7/8"}, {k8K, k64Qam, kR23, 20.0, "8K 64-QAM 2/3"},
        {k8K, k16Qam, kR12, 11.0, "8K 16-QAM 1/2"}};
    for (auto& c : cases) {
        Params p; p.mode = c.mode; p.mod = c.mod; p.crHp = c.cr; p.crLp = c.cr;
        double secs = 0; FecStats st;
        const bool ok = chain(p, c.snr, c.mode == k8K ? 136 : 272, &secs, &st);
        const double symPerSec = (c.mode == k8K ? 136 : 272) / secs;
        printf("%-16s @ %4.1f dB Es/N0: %s  (RS clean %llu corrected %llu failed %llu, phase %d, %.0f symbols/s)\n", c.name, c.snr, ok ? "TS recovered" : "FAILED", (unsigned long long)st.rsClean, (unsigned long long)st.rsCorrected, (unsigned long long)st.rsFailed, st.punctPhase, symPerSec);
        CHECK(ok, "%s did not recover the transport stream", c.name);
    }
    // the puncturing pattern's phase relative to the symbol stream is not known to the receiver: it must find it
    for (int cr = kR23; cr <= kR78; cr++) {
        const int k = std::vector<int>{1, 2, 3, 5, 7}[cr];
        for (int ph = 0; ph < k; ph++) {
            Params p; p.mode = k2K; p.mod = k16Qam; p.crHp = cr; p.crLp = cr;
            FecStats st;
            const bool ok = chain(p, 22.0, 136, nullptr, &st, ph);
            CHECK(ok, "rate %s with puncturing phase %d not recovered (receiver chose %d)", rateName(cr), ph, st.punctPhase);
            CHECK(st.punctPhase == ph, "rate %s: phase %d detected as %d", rateName(cr), ph, st.punctPhase);
        }
    }
    printf("puncturing phase search checked for rates 2/3 .. 7/8\n");
    printf(fails ? "DVB-T tests FAILED\n" : "DVB-T tests passed\n");
    return fails ? 1 : 0;
}
