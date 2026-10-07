// DVB-S2 channel coding and framing building blocks: properties of the code tables, known-answer checks against the specification
// (EN 302 307-1), and a round trip through every MODCOD (BB scrambling, BCH, LDPC, bit interleaver, mapping, noise, soft demapping, decoding).
#include "dect2/dvbs_s2.h"
#include "dect2/dvbt.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::dvbs;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// the LDPC encoder written once more, literally from clause 5.3.2.1, working on the address table (not on the LdpcCode object)
static void specLdpcEncode(const std::vector<std::vector<int>>& rows, int k, int n, int q, std::vector<uint8_t>& bits) {
    const int m = n - k;
    bits.resize(n);
    for (int i = k; i < n; i++) bits[i] = 0;
    for (size_t g = 0; g < rows.size(); g++)
        for (int j = 0; j < 360; j++) {
            const int info = (int)g * 360 + j;
            for (int x : rows[g]) bits[k + (x + j * q) % m] ^= bits[info];
        }
    for (int i = 1; i < m; i++) bits[k + i] ^= bits[k + i - 1];
}

int main() {
    std::mt19937 rng(12345);

    // ---- MODCOD table (table 12) and code dimensions (tables 5a, 5b, 7a, 7b, 11)
    {
        int mod, rate;
        CHECK(s2ModcodSplit(1, mod, rate) && mod == kQpsk && rate == 0, "MODCOD 1 is QPSK 1/4");
        CHECK(s2ModcodSplit(12, mod, rate) && mod == k8psk && std::string(s2RateName(rate)) == "3/5", "MODCOD 12 is 8PSK 3/5");
        CHECK(s2ModcodSplit(18, mod, rate) && mod == k16apsk && std::string(s2RateName(rate)) == "2/3", "MODCOD 18 is 16APSK 2/3");
        CHECK(s2ModcodSplit(28, mod, rate) && mod == k32apsk && std::string(s2RateName(rate)) == "9/10", "MODCOD 28 is 32APSK 9/10");
        CHECK(!s2ModcodSplit(29, mod, rate) && !s2ModcodSplit(0, mod, rate), "MODCOD 0 (dummy) and 29..31 (reserved) carry no data");
        int count = 0;
        for (int mo = 0; mo < 4; mo++) for (int r = 0; r < kS2Rates; r++) if (s2Modcod(mo, r) >= 0) count++;
        CHECK(count == 28, "28 modulation and code rate pairs, got %d", count);
        CHECK(s2Dims(kQpsk, 3, false).kbch == 32208 && s2Dims(kQpsk, 3, false).t == 12, "normal 1/2: Kbch 32208, t 12");
        CHECK(s2Dims(k8psk, 5, false).kbch == 43040 && s2Dims(k8psk, 5, false).t == 10, "normal 2/3: Kbch 43040, t 10");
        CHECK(s2Dims(k8psk, 9, false).kbch == 57472 && s2Dims(k8psk, 9, false).t == 8, "normal 8/9: Kbch 57472, t 8");
        CHECK(s2Dims(kQpsk, 3, true).kbch == 7032 && s2Dims(kQpsk, 3, true).kldpc == 7200, "short 1/2: Kbch 7032, kldpc 7200");
        CHECK(!s2Dims(kQpsk, 10, true).ok, "no short 9/10");
        // table 11: slots per frame
        CHECK(s2Dims(kQpsk, 3, false).slots == 360 && s2Dims(k8psk, 5, false).slots == 240 && s2Dims(k16apsk, 5, false).slots == 180 && s2Dims(k32apsk, 6, false).slots == 144, "slots, normal frames");
        CHECK(s2Dims(kQpsk, 3, true).slots == 90 && s2Dims(k8psk, 5, true).slots == 60 && s2Dims(k16apsk, 5, true).slots == 45 && s2Dims(k32apsk, 6, true).slots == 36, "slots, short frames");
        // figure 13: PLFRAME length 90(S+1) + 36 int((S-1)/16)
        CHECK(s2FrameSymbols(kQpsk, false, false) == 32490 && s2FrameSymbols(kQpsk, false, true) == 33282, "QPSK normal frame length");
        CHECK(s2FrameSymbols(k32apsk, true, true) == 90 * 37 + 36 * 2, "32APSK short frame length with pilots");
    }

    // ---- LDPC tables: encoder against a transcription of the spec procedure, and the parity check structure
    {
        const char* names[kS2Rates] = {"1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"};
        int checked = 0;
        for (int sh = 0; sh < 2; sh++)
            for (int r = 0; r < kS2Rates; r++) {
                int mo = -1;
                for (int m = 0; m < 4; m++) if (s2Dims(m, r, sh).ok) { mo = m; break; }
                if (mo < 0) continue;
                const S2Dims d = s2Dims(mo, r, sh);
                const LdpcCode& code = s2Ldpc(r, sh);
                CHECK(code.k() == d.kldpc && code.n() == d.nldpc, "%s %s: code size", sh ? "short" : "normal", names[r]);
                // rebuild the rows from the code's layers: layer l, (group g, shift s) means the address a = s * q + l in row g
                const int groups = d.kldpc / 360, q = d.q;
                CHECK(code.groups() == groups, "%s %s: %d groups, expected %d", sh ? "short" : "normal", names[r], code.groups(), groups);
                std::vector<std::vector<int>> rows(groups);
                for (int l = 0; l < code.q(); l++) for (const auto& c : code.layers()[l]) rows[c.group].push_back(c.shift * q + l);
                std::vector<uint8_t> a(d.kldpc), b;
                for (auto& v : a) v = rng() & 1;
                b = a;
                code.encode(b);
                std::vector<uint8_t> c = a;
                specLdpcEncode(rows, d.kldpc, d.nldpc, q, c);
                CHECK(b == c, "%s %s: encoder differs from the specification procedure", sh ? "short" : "normal", names[r]);
                // every address is a valid parity bit index and a row has no repeated address
                bool rowsOk = true;
                for (auto& row : rows) { std::set<int> s(row.begin(), row.end()); rowsOk &= s.size() == row.size(); for (int x : row) rowsOk &= x >= 0 && x < d.nldpc - d.kldpc; }
                CHECK(rowsOk, "%s %s: repeated or out of range address", sh ? "short" : "normal", names[r]);
                // the normal frame codes have a constant check node degree (number of information bits in each check)
                std::vector<int> deg(d.nldpc - d.kldpc, 0);
                for (int l = 0; l < code.q(); l++) for (int j = 0; j < 360; j++) deg[(size_t)l * 360 + j] = 0;
                for (int g = 0; g < groups; g++) for (int x : rows[g]) for (int j = 0; j < 360; j++) deg[(x + j * q) % (d.nldpc - d.kldpc)]++;
                std::set<int> dset(deg.begin(), deg.end());
                if (!sh) CHECK(dset.size() == 1, "normal %s: check degrees are not constant (%zu different values)", names[r], dset.size());
                checked++;
            }
        CHECK(checked == 21, "21 LDPC codes, got %d", checked);
        printf("LDPC: %d codes: encoder = spec procedure, normal frame check degrees constant\n", checked);
    }

    // ---- BCH: polynomials give the right number of parity bits; corrects t errors, detects t+1 (a few tries)
    {
        for (int sh = 0; sh < 2; sh++)
            for (int r : {0, 5, 9, 4}) {
                if (sh && r == 10) continue;
                int mo = -1;
                for (int m = 0; m < 4; m++) if (s2Dims(m, r, sh).ok) { mo = m; break; }
                const S2Dims d = s2Dims(mo, r, sh);
                const BchCode& bch = s2Bch(r, sh);
                CHECK(bch.parityBits() == d.kldpc - d.kbch, "BCH parity bits %d, expected %d", bch.parityBits(), d.kldpc - d.kbch);
                std::vector<uint8_t> msg(d.kbch);
                for (auto& v : msg) v = rng() & 1;
                std::vector<uint8_t> cw = msg;
                bch.encode(cw, d.kbch);
                auto bad = cw;
                std::set<int> pos;
                while ((int)pos.size() < d.t) pos.insert(rng() % cw.size());
                for (int p : pos) bad[p] ^= 1;
                const int fixed = bch.decode(bad);
                CHECK(fixed == d.t && bad == cw, "BCH t=%d: %d errors not corrected (%d)", d.t, d.t, fixed);
            }
        printf("BCH: parity lengths and error correction ok\n");
    }

    // ---- known answers from the specification text
    {
        // SOF 18D2E82 (clause 5.5.2.1): "01-1000-....-0010" in binary notation
        CHECK(kSof == 0x18D2E82 && (kSof >> 24) == 1 && (kSof & 0xF) == 2, "SOF constant");
        // PLS scrambling sequence (clause 5.5.2.4), as printed in the specification
        const char* plsSeq = "0111000110011101100000111100100101010011010000100010110111111010";
        const uint64_t zero = s2PlsCode(0, false, false);                 // all information bits zero: the codeword is zero, what is left is the scrambling sequence
        std::string got;
        for (int i = 0; i < 64; i++) got += (char)('0' + ((zero >> (63 - i)) & 1));
        CHECK(got == plsSeq, "PLS scrambling sequence: %s", got.c_str());
        // the (64,7) code has 128 different words and a minimum distance of 32
        std::set<uint64_t> words;
        int dmin = 64;
        std::vector<uint64_t> all;
        for (int p = 0; p < 128; p++) { all.push_back(s2PlsCode(p >> 2, (p >> 1) & 1, p & 1) ^ zero); words.insert(all.back()); }
        for (int i = 0; i < 128; i++) for (int j = i + 1; j < 128; j++) dmin = std::min(dmin, __builtin_popcountll(all[i] ^ all[j]));
        CHECK(words.size() == 128 && dmin == 32, "PLS code: %zu words, minimum distance %d (expected 128 and 32)", words.size(), dmin);
        {   // the generator matrix of the (32,6) code as printed in TR 102 376-1 annex B.1 (the first order Reed-Muller code in natural order); EN 302 307-1
            // figure 13b in the pdf text layer gives other rows, which would not decode real signals (see docs/modes/dvbs.md)
            static const char* rows[6] = {"01010101010101010101010101010101", "00110011001100110011001100110011", "00001111000011110000111100001111",
                                          "00000000111111110000000011111111", "00000000000000001111111111111111", "11111111111111111111111111111111"};
            for (int i = 0; i < 6; i++) {
                const uint64_t w = s2PlsCode(i < 5 ? 1 << (4 - i) : 0, i == 5, false) ^ zero;     // message bit i alone: row i, every bit sent twice
                std::string y;
                for (int k = 0; k < 32; k++) y += (char)('0' + ((w >> (63 - 2 * k)) & 1));
                CHECK(y == rows[i], "PLS code row %d: %s (TR 102 376-1: %s)", i + 1, y.c_str(), rows[i]);
            }
            // linear: the code word of a sum of messages is the sum of the code words
            for (int t = 0; t < 40; t++) {
                const int a = (int)(rng() & 127), b = (int)(rng() & 127);
                const uint64_t wa = s2PlsCode(a >> 2, (a >> 1) & 1, a & 1) ^ zero, wb = s2PlsCode(b >> 2, (b >> 1) & 1, b & 1) ^ zero;
                const int c = a ^ b;
                CHECK((wa ^ wb) == (s2PlsCode(c >> 2, (c >> 1) & 1, c & 1) ^ zero), "PLS code is not linear");
            }
            // every non-zero code word of the 32 bit code has weight 16 (or 32 for the all ones row)
            int weights[3] = {};
            for (int m = 0; m < 64; m++) {
                const uint64_t w = s2PlsCode(m >> 1, m & 1, false) ^ zero;
                int wt = 0;
                for (int k = 0; k < 32; k++) wt += (int)((w >> (63 - 2 * k)) & 1);
                weights[wt == 0 ? 0 : wt == 16 ? 1 : wt == 32 ? 2 : 0]++;
            }
            CHECK(weights[1] == 62 && weights[2] == 1, "PLS code weights: %d words of weight 16, %d of 32", weights[1], weights[2]);
            // the extra row of the DVB-S2X code (EN 302 307-2 figure 20, as in gr-dtv) is 12 away from the code, the most any row on 32 bits can be
            const uint32_t extra = 0x90AC2DDDu;
            int best = 32;
            for (int m = 0; m < 64; m++) {
                uint32_t cw = 0;
                static const uint32_t g[6] = {0x55555555u, 0x33333333u, 0x0F0F0F0Fu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu};
                for (int i = 0; i < 6; i++) if ((m >> i) & 1) cw ^= g[i];
                best = std::min(best, __builtin_popcount(cw ^ extra));
            }
            CHECK(best == 12, "S2X PLS extra row: distance %d to the Reed-Muller code (12 expected)", best);
        }
        // odd bits are equal to the previous one or always the opposite, depending on b7 (the pilot flag)
        for (int p = 0; p < 128; p++) {
            const uint64_t w = all[p];
            const int b7 = p & 1;
            bool ok = true;
            for (int k = 0; k < 32; k++) ok &= (((w >> (63 - 2 * k)) ^ (w >> (62 - 2 * k))) & 1) == (uint64_t)b7;
            CHECK(ok, "PLS code %d: the second bit of each pair does not follow b7", p);
        }
        // CRC-8 of the DVB-S2 BBHEADER / user packets: x^8+x^7+x^6+x^4+x^2+1, check value of the catalogue entry CRC-8/DVB-S2
        CHECK(s2Crc8((const uint8_t*)"123456789", 9) == 0xBC, "CRC-8 check value 0x%02X (expected 0xBC)", s2Crc8((const uint8_t*)"123456789", 9));
        // BBHEADER round trip and CRC detection
        S2BbHeader h; h.dfl = 43040 - 80; h.syncd = 1234; h.ro = 1; h.ccm = false;
        uint8_t hb[80];
        s2BuildBbHeader(h, hb);
        S2BbHeader g;
        CHECK(s2ParseBbHeader(hb, g) && g.dfl == h.dfl && g.syncd == 1234 && g.ro == 1 && !g.ccm && g.upl == 1504 && g.sync == 0x47 && g.tsGs == 3 && g.sis, "BBHEADER round trip");
        hb[20] ^= 1;
        CHECK(!s2ParseBbHeader(hb, g), "BBHEADER with a flipped bit passes the CRC");
        // BB scrambling: PRBS 1 + X^14 + X^15 loaded with 100101010000000, against the DVB-T energy dispersal generator (same polynomial and seed)
        std::vector<uint8_t> z(8 * 187, 0);
        s2BbScramble(z.data(), (int)z.size());
        std::vector<uint8_t> ts(188, 0), sc(188);
        dvbt::scramble(ts.data(), 1, sc.data());
        bool same = true;
        for (int i = 0; i < 8 * 187; i++) same &= z[i] == ((sc[1 + i / 8] >> (7 - i % 8)) & 1);
        CHECK(same, "BB scrambler sequence differs from the DVB-T energy dispersal sequence");
        const uint8_t* bbr = bbRandomiser();
        bool same2 = true;
        for (int i = 0; i < 8 * 187; i++) same2 &= z[i] == bbr[i];
        CHECK(same2, "BB scrambler sequence differs from the DVB-T2 one");
        // PL scrambling: both m-sequences have the full period 2^18 - 1 (the polynomials are primitive), and R in 0..3
        const auto& rn = s2ScramblingRn(0);
        int cnt[4] = {};
        for (uint8_t v : rn) cnt[v & 3]++;
        CHECK(rn.size() == 66420 && cnt[0] + cnt[1] + cnt[2] + cnt[3] == 66420, "scrambling sequence length");
        for (int c : cnt) CHECK(c > 15000 && c < 18000, "scrambling sequence is not balanced: %d %d %d %d", cnt[0], cnt[1], cnt[2], cnt[3]);
        {   // the x and y recursions: the state must come back after exactly 2^18 - 1 steps and not after N / p
            auto period = [](bool isX) {
                const long N = (1 << 18) - 1;
                std::vector<uint8_t> s(N + 18);
                for (int i = 0; i < 18; i++) s[i] = isX ? (i == 0) : 1;
                for (long i = 0; i + 18 < N + 18; i++) s[i + 18] = isX ? (s[i + 7] ^ s[i]) : (s[i + 10] ^ s[i + 7] ^ s[i + 5] ^ s[i]);
                auto same = [&](long shift) { for (int i = 0; i < 18; i++) if (s[shift + i] != s[i]) return false; return true; };
                bool ok = same(N);
                for (long p : {3L, 7L, 19L, 73L}) ok &= !same(N / p);
                return ok;
            };
            CHECK(period(true) && period(false), "scrambling code generators are not maximal length sequences");
        }
        // rotation table of clause 5.5.4
        const cf32 s(0.3f, 0.7f);
        CHECK(s2RotateByR(s, 1) == cf32(-0.7f, 0.3f) && s2RotateByR(s, 2) == cf32(-0.3f, -0.7f) && s2RotateByR(s, 3) == cf32(0.7f, -0.3f), "R = 1, 2, 3 rotations");
        // PLHEADER: SOF symbols follow the pi/2 BPSK rule (first symbol I = Q, second I = -Q)
        cf32 hdr[90];
        s2PlHeader(4, false, true, hdr);
        CHECK(hdr[0] == s2Bpsk(0, 0) && std::fabs(hdr[0].real() - hdr[0].imag()) < 1e-6f && std::fabs(hdr[1].real() + hdr[1].imag()) < 1e-6f, "PLHEADER first symbols");
        // the PLS decoder finds every code word in noise
        std::normal_distribution<float> nd(0.f, 1.f);
        int wrong = 0;
        for (int p = 0; p < 128; p++) {
            cf32 hh[90];
            s2PlHeader(p >> 2, (p >> 1) & 1, p & 1, hh);
            cf32 sy[64];
            for (int k = 0; k < 64; k++) sy[k] = hh[26 + k] + cf32(nd(rng), nd(rng)) * 0.5f;   // Es/N0 = 3 dB
            const PlsResult r = s2PlsDecode(sy);
            if (r.modcod != (p >> 2) || r.shortFrame != (bool)((p >> 1) & 1) || r.pilots != (bool)(p & 1)) wrong++;
        }
        CHECK(wrong == 0, "PLS decoder: %d of 128 words decoded wrongly at 3 dB", wrong);
    }

    // ---- Es/N0 of table 13 (the receiver measures its margin against it)
    {
        CHECK(std::fabs(s2QefEsN0(kQpsk, 0, false) - (-2.35)) < 1e-9 && std::fabs(s2QefEsN0(k32apsk, 10, false) - 16.05) < 1e-9 && std::fabs(s2QefEsN0(k8psk, 4, false) - 5.50) < 1e-9, "Es/N0 of table 13");
        CHECK(s2QefEsN0(kQpsk, 10, true) > 90 && s2QefEsN0(k8psk, 0, false) > 90, "combinations that do not exist");
    }

    // ---- constellations: unit energy, distinct points, ring structure
    {
        for (int mo = 0; mo < 4; mo++)
            for (int r = 0; r < kS2Rates; r++) {
                if (s2Modcod(mo, r) < 0) continue;
                const cf32* c = s2Constellation(mo, r);
                const int P = s2ConstellationSize(mo);
                double e = 0, dmin = 1e9;
                for (int i = 0; i < P; i++) { e += std::norm(c[i]); for (int j = i + 1; j < P; j++) dmin = std::min(dmin, (double)std::abs(c[i] - c[j])); }
                CHECK(std::fabs(e / P - 1.0) < 1e-5, "%s %s: average energy %.5f", s2ModName(mo), s2RateName(r), e / P);
                CHECK(dmin > 0.05, "%s %s: two points coincide", s2ModName(mo), s2RateName(r));
            }
        // 16APSK 2/3: ring ratio 3.15 (table 9), 4 points on the inner ring and 12 on the outer one
        const cf32* c = s2Constellation(k16apsk, 5);
        std::set<int> radii;
        double rmin = 9, rmax = 0;
        for (int i = 0; i < 16; i++) { rmin = std::min(rmin, (double)std::abs(c[i])); rmax = std::max(rmax, (double)std::abs(c[i])); }
        int inner = 0;
        for (int i = 0; i < 16; i++) if (std::abs(std::abs(c[i]) - rmin) < 1e-4) inner++;
        CHECK(std::fabs(rmax / rmin - 3.15) < 1e-4 && inner == 4, "16APSK 2/3 rings: ratio %.4f, %d inner points", rmax / rmin, inner);
        // 32APSK 3/4: 4 + 12 + 16 points, ratios 2.84 and 5.27 (table 10)
        const cf32* d = s2Constellation(k32apsk, 6);
        std::vector<double> rad;
        for (int i = 0; i < 32; i++) rad.push_back(std::abs(d[i]));
        std::sort(rad.begin(), rad.end());
        CHECK(std::fabs(rad[4] / rad[0] - 2.84) < 1e-4 && std::fabs(rad[16] / rad[0] - 5.27) < 1e-4 && std::fabs(rad[3] - rad[0]) < 1e-5 && std::fabs(rad[15] - rad[4]) < 1e-5, "32APSK 3/4 rings");
        // 8PSK: Gray mapping, neighbours differ in one bit
        const cf32* p8 = s2Constellation(k8psk, 5);
        for (int a = 0; a < 8; a++) {
            int near = 0;
            for (int b = 0; b < 8; b++) if (a != b && std::abs(p8[a] - p8[b]) < 0.78 + 1e-3) { near++; CHECK(__builtin_popcount(a ^ b) == 1, "8PSK labels %d and %d are neighbours but not Gray", a, b); }
            CHECK(near == 2, "8PSK point %d has %d neighbours", a, near);
        }
        printf("constellations: unit energy, rings and ratios of tables 9 and 10, 8PSK Gray\n");
    }

    // ---- the whole data path in noise, every MODCOD, normal and short frames, a little above the QEF Es/N0 of table 13
    {
        static const double qef[4][kS2Rates] = {   // table 13, normal FECFRAME
            {-2.35, -1.24, -0.30, 1.00, 2.23, 3.10, 4.03, 4.68, 5.18, 6.20, 6.42},
            {0, 0, 0, 0, 5.50, 6.62, 7.91, 0, 9.35, 10.69, 10.98},
            {0, 0, 0, 0, 0, 8.97, 10.21, 11.03, 11.61, 12.89, 13.13},
            {0, 0, 0, 0, 0, 0, 12.73, 13.64, 14.28, 15.69, 16.05}};
        std::normal_distribution<float> nd(0.f, 1.f);
        int frames = 0, bad = 0;
        double totalMs = 0, itSum = 0;
        for (int sh = 0; sh < 2; sh++)
            for (int mo = 0; mo < 4; mo++)
                for (int r = 0; r < kS2Rates; r++) {
                    const S2Dims d = s2Dims(mo, r, sh);
                    if (!d.ok) continue;
                    // a BBFRAME: header + data, scrambled
                    std::vector<uint8_t> bb(d.kbch);
                    for (auto& v : bb) v = rng() & 1;
                    S2BbHeader h; h.dfl = d.kbch - 80;
                    s2BuildBbHeader(h, bb.data());
                    std::vector<uint8_t> scr = bb;
                    s2BbScramble(scr.data(), d.kbch);
                    std::vector<uint8_t> fec, il;
                    s2EncodeFec(scr, r, sh, fec);
                    s2BitInterleave(fec, mo, r, il);
                    std::vector<cf32> sym(d.xfecSymbols);
                    s2MapBits(il.data(), d.nldpc, mo, r, sym.data());
                    const double es = qef[mo][r] + (sh ? 1.3 : 0.9);
                    const double sigma2 = std::pow(10.0, -es / 10.0) / 2.0;
                    for (auto& s : sym) s += cf32(nd(rng), nd(rng)) * (float)std::sqrt(sigma2);
                    auto t0 = std::chrono::steady_clock::now();
                    std::vector<float> llr((size_t)d.nldpc), dl((size_t)d.nldpc);
                    s2Demap(sym.data(), d.xfecSymbols, mo, r, (float)sigma2, llr.data());
                    s2BitDeinterleaveLlr(llr.data(), d.nldpc, mo, r, dl.data());
                    std::vector<uint8_t> hard;
                    int iters = 0;
                    const bool ok = s2Ldpc(r, sh).decodeFast(dl, 50, hard, &iters, s2LdpcAlpha(r));
                    std::vector<uint8_t> out;
                    const int fixed = ok ? s2BchDecode(hard.data(), r, sh, out) : -1;
                    totalMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    itSum += iters;
                    frames++;
                    if ((int)out.size() == d.kbch) s2BbScramble(out.data(), d.kbch);
                    const bool good = fixed >= 0 && out == bb;
                    if (!good) bad++;
                    CHECK(good, "%s %s %s frame: Es/N0 %.2f dB (QEF %.2f): LDPC %s, BCH %d", sh ? "short" : "normal", s2ModName(mo), s2RateName(r), es, qef[mo][r], ok ? "ok" : "failed", fixed);
                }
        printf("round trip: %d frames, %d failed, mean LDPC iterations %.1f, %.1f ms per frame (demap + decode)\n", frames, bad, itSum / frames, totalMs / frames);
    }
    printf(fails ? "dvbs s2fec: FAILED\n" : "dvbs s2fec: ok\n");
    return fails ? 1 : 0;
}
