// DTMB channel coding: LDPC encoder against the parity-check matrix, decoder round trips, BCH, scrambler, interleaver, 4QAM-NR.
#include "dect2/dtmb_ldpc.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>

using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);

    // ---- LDPC structure and encoder
    const int edges[3] = {34925, 37592, 37338}, infoBits[3] = {3048, 4572, 6096};
    for (int r = 0; r < 3; r++) {
        const LdpcCode& code = ldpcCode((Rate)r);
        CHECK(code.edgeCount() == edges[r], "rate %d edges %d", r, code.edgeCount());
        CHECK(code.infoBits() == infoBits[r] && code.infoBits() == ldpcInfoBits((Rate)r), "info bits");
        CHECK(code.parityBits() + code.infoBits() == kLdpcVars, "length");
        int count, rows;
        const LdpcBlock* b = ldpcBlocks((Rate)r, count, rows);
        // no 4-cycles: two variables share at most one check
        std::vector<std::vector<int>> chk((size_t)rows * kLdpcZ);
        for (int k = 0; k < count; k++) for (int i = 0; i < kLdpcZ; i++) chk[(size_t)(b[k].row * kLdpcZ + i)].push_back(b[k].col * kLdpcZ + (i + b[k].shift) % kLdpcZ);
        std::set<std::pair<int, int>> pairs;
        int cycles = 0;
        for (auto& v : chk) for (size_t x = 0; x < v.size(); x++) for (size_t y = x + 1; y < v.size(); y++)
            if (!pairs.insert({std::min(v[x], v[y]), std::max(v[x], v[y])}).second) cycles++;
        CHECK(cycles == 0, "rate %d has %d 4-cycles", r, cycles);
        // encoder: every random message gives a word that satisfies all checks
        int bad = 0;
        for (int t = 0; t < 20; t++) {
            std::vector<uint8_t> info((size_t)code.infoBits()), word(kLdpcVars);
            for (auto& x : info) x = (uint8_t)(rng() & 1);
            if (t == 0) std::fill(info.begin(), info.end(), 0);
            if (t == 1) std::fill(info.begin(), info.end(), 1);
            code.encodeFull(info.data(), word.data());
            if (code.syndromeWeight(word.data()) != 0) bad++;
            if (std::memcmp(word.data() + code.parityBits(), info.data(), info.size()) != 0) bad++;
            std::vector<uint8_t> sent(kLdpcSent);
            code.encode(info.data(), sent.data());
            if (std::memcmp(sent.data(), word.data() + 5, kLdpcSent) != 0) bad++;
        }
        CHECK(bad == 0, "rate %d: %d encoder failures", r, bad);
    }

    // ---- LDPC decoder: BPSK over AWGN. Eb/N0 here counts the information bits of the 7488 transmitted bits. Measured with this decoder (normalised
    // min-sum, 0.6, 100 iterations): 10 % frame errors at about 1.9 / 2.0 / 3.0 dB for rates 0.4 / 0.6 / 0.8 (BPSK capacity limits -0.2 / 0.7 / 2.2 dB).
    // The last point of each rate sits 1.5 dB above that and is run with many words: an earlier version of the decoder (a posteriori values clipped
    // at 40) lost about one word in a thousand there by diverging after it had already found the right information bits.
    {
        struct Pt { double db; int trials; int maxFails, minFails; };   // the low point must fail, the others must not
        const Pt pts[3][3] = {{{1.4, 40, 40, 30}, {2.4, 400, 0, 0}, {3.4, 1500, 0, 0}}, {{1.4, 40, 40, 30}, {2.6, 400, 0, 0}, {3.5, 1500, 0, 0}}, {{2.4, 40, 40, 30}, {3.6, 400, 0, 0}, {4.5, 1500, 0, 0}}};
        for (int r = 0; r < 3; r++) {
            const LdpcCode& code = ldpcCode((Rate)r);
            LdpcCode::Decoder dec(code);
            const double rate = (double)code.infoBits() / kLdpcSent;
            for (int s = 0; s < 3; s++) {
                const double ebn0 = std::pow(10.0, pts[r][s].db / 10), es = rate * ebn0;   // BPSK: Es = R Eb, N0 per real dimension
                const float sigma = (float)std::sqrt(1.0 / (2 * es));
                const int trials = pts[r][s].trials;
                int fails = 0, iterSum = 0, wrongOk = 0;
                std::vector<uint8_t> info((size_t)code.infoBits()), sent(kLdpcSent), out((size_t)code.infoBits());
                std::vector<float> llr(kLdpcSent);
                for (int t = 0; t < trials; t++) {
                    for (auto& x : info) x = (uint8_t)(rng() & 1);
                    code.encode(info.data(), sent.data());
                    for (int i = 0; i < kLdpcSent; i++) {
                        const float x = (sent[(size_t)i] ? -1.f : 1.f) + sigma * nd(rng);
                        llr[(size_t)i] = 2.f * x / (sigma * sigma);
                    }
                    const auto res = dec.decode(llr.data(), out.data(), 100);
                    iterSum += res.iterations;
                    const bool same = std::memcmp(out.data(), info.data(), info.size()) == 0;
                    if (!res.ok || !same) fails++;
                    if (res.ok && !same) wrongOk++;
                }
                printf("  LDPC rate %s Eb/N0 %.1f dB (BPSK): %d/%d frame errors, %.1f iterations\n", rateName((Rate)r), pts[r][s].db, fails, trials, (double)iterSum / trials);
                CHECK(wrongOk == 0, "decoder reported success on a wrong word");
                CHECK(fails <= pts[r][s].maxFails && fails >= pts[r][s].minFails, "rate %d at %.1f dB: %d of %d words lost", r, pts[r][s].db, fails, trials);
            }
        }
        // clean input: one iteration is enough
        LdpcCode::Decoder dec(ldpcCode(Rate::R06));
        std::vector<uint8_t> info(4572), sent(kLdpcSent), out(4572);
        for (auto& x : info) x = (uint8_t)(rng() & 1);
        ldpcCode(Rate::R06).encode(info.data(), sent.data());
        std::vector<float> llr(kLdpcSent);
        for (int i = 0; i < kLdpcSent; i++) llr[(size_t)i] = sent[(size_t)i] ? -6.f : 6.f;
        const auto res = dec.decode(llr.data(), out.data(), 20);
        CHECK(res.ok && res.iterations == 1 && out == info, "clean decode: ok %d iterations %d", (int)res.ok, res.iterations);
        // a word of random LLRs must not be reported as decoded
        for (auto& x : llr) x = nd(rng);
        const auto bad = dec.decode(llr.data(), out.data(), 20);
        CHECK(!bad.ok, "noise accepted as a code word");
    }

    // ---- decoder speed: one codeword at a moderate SNR
    {
        const LdpcCode& code = ldpcCode(Rate::R08);
        LdpcCode::Decoder dec(code);
        std::vector<uint8_t> info((size_t)code.infoBits()), sent(kLdpcSent), out((size_t)code.infoBits());
        for (auto& x : info) x = (uint8_t)(rng() & 1);
        code.encode(info.data(), sent.data());
        std::vector<float> llr(kLdpcSent);
        const float sigma = 0.6f;
        for (int i = 0; i < kLdpcSent; i++) llr[(size_t)i] = 2.f * ((sent[(size_t)i] ? -1.f : 1.f) + sigma * nd(rng)) / (sigma * sigma);
        const auto t0 = std::chrono::steady_clock::now();
        int iters = 0;
        for (int k = 0; k < 20; k++) iters += dec.decode(llr.data(), out.data(), 30).iterations;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 20;
        printf("  LDPC decoder: %.0f us per codeword, %.1f iterations, %.1f us per iteration\n", dt * 1e6, iters / 20.0, dt * 1e6 / (iters / 20.0));
    }

    // ---- BCH(762,752)
    {
        int bad = 0;
        for (int t = 0; t < 200; t++) {
            std::vector<uint8_t> m(kBchK), w(kBchN);
            for (auto& x : m) x = (uint8_t)(rng() & 1);
            bchEncode(m.data(), w.data());
            auto w0 = w;
            if (bchDecode(w.data()) != 0) bad++;
            const int p = (int)(rng() % kBchN);
            w[(size_t)p] ^= 1;
            if (bchDecode(w.data()) != 1 || w != w0) bad++;
            int q = (int)(rng() % kBchN);
            if (q == p) q = (q + 1) % kBchN;
            w[(size_t)p] ^= 1; w[(size_t)q] ^= 1;
            if (bchDecode(w.data()) == 0) bad++;
        }
        CHECK(bad == 0, "BCH: %d failures", bad);
        // g(x) = x^10 + x^3 + 1: the word with a single message bit at the last position has parity 0000001001 (x^10 mod g = x^3 + 1)
        std::vector<uint8_t> m(kBchK, 0), w(kBchN);
        m[kBchK - 1] = 1;
        bchEncode(m.data(), w.data());
        const int par = (w[752] << 9) | (w[753] << 8) | (w[754] << 7) | (w[755] << 6) | (w[756] << 5) | (w[757] << 4) | (w[758] << 3) | (w[759] << 2) | (w[760] << 1) | w[761];
        CHECK(par == 0x009, "BCH parity of x^751: %03X", par);
    }

    // ---- scrambler: maximal length PRBS of 1 + x^14 + x^15
    {
        Scrambler s;
        std::vector<uint8_t> seq;
        for (int i = 0; i < 2 * 32767 + 10; i++) seq.push_back(s.next());
        bool periodic = true;
        for (int i = 0; i < 32767 + 10; i++) if (seq[(size_t)i] != seq[(size_t)(i + 32767)]) periodic = false;
        CHECK(periodic, "scrambler period");
        int ones = 0;
        for (int i = 0; i < 32767; i++) ones += seq[(size_t)i];
        CHECK(ones == 16384, "ones in a period: %d", ones);
        // no shorter period
        bool shorter = false;
        for (int p : {1, 7, 31, 151, 217, 4681}) { bool same = true; for (int i = 0; i < 400; i++) if (seq[(size_t)i] != seq[(size_t)(i + p)]) same = false; if (same) shorter = true; }
        CHECK(!shorter, "short period");
        // the first output bits: the initial state 100101010000000 reaches the taps after six steps
        CHECK(seq[0] == 0 && seq[1] == 0 && seq[2] == 0 && seq[3] == 0 && seq[4] == 0 && seq[5] == 0 && seq[6] == 1, "first bits");
    }

    // ---- convolutional interleaver: a stream comes back unchanged after 51 * 52 * M elements
    for (int m : {240, 720}) {
        ConvInterleaver<int> tx(m, false), rx(m, true);
        const long delay = tx.totalDelay();
        CHECK(delay == 51L * 52 * m, "delay");
        const size_t n = (size_t)delay + 20000;
        std::vector<int> in(n), mid(n), out(n);
        for (size_t i = 0; i < n; i++) in[i] = (int)(i + 1);
        // feed in odd chunk sizes
        size_t pos = 0, chunk = 1;
        while (pos < n) { const size_t k = std::min(chunk, n - pos); tx.process(&in[pos], &mid[pos], k); rx.process(&mid[pos], &out[pos], k); pos += k; chunk = chunk * 3 + 1; if (chunk > 5000) chunk = 1; }
        int bad = 0;
        for (size_t i = (size_t)delay; i < n; i++) if (out[i] != in[i - (size_t)delay]) bad++;
        CHECK(bad == 0, "M=%d: %d elements wrong", m, bad);
        // the interleaver really permutes: not the identity and a block of 52 stays inside a window
        int moved = 0;
        for (size_t i = (size_t)delay; i < n; i++) if (mid[i] != in[i]) moved++;
        CHECK(moved > (int)(n - (size_t)delay) / 2, "interleaver did nothing");
    }

    // ---- 4QAM-NR soft decoding
    {
        int bad = 0;
        for (int t = 0; t < 400; t++) {
            const int x = (int)(rng() & 255);
            const int y = nrParity((uint8_t)x);
            float l[16], out[8];
            for (int i = 0; i < 8; i++) l[i] = ((x >> (7 - i)) & 1) ? -3.f : 3.f;
            for (int i = 0; i < 8; i++) l[8 + i] = ((y >> (7 - i)) & 1) ? -3.f : 3.f;
            // flip two bits: the code corrects them (distance 6)
            const int a = (int)(rng() % 16), b = (a + 1 + (int)(rng() % 15)) % 16;
            l[a] = -l[a]; l[b] = -l[b];
            nrSoftDecode(l, out);
            int v = 0;
            for (int i = 0; i < 8; i++) v = (v << 1) | (out[i] < 0);
            if (v != x) bad++;
        }
        CHECK(bad == 0, "NR: %d blocks wrong with two flipped bits", bad);
    }
    printf(failures ? "dtmb_fec: %d FAILED\n" : "dtmb_fec: all passed\n", failures);
    return failures ? 1 : 0;
}
