// Soft-decision vocoder frame protection (core/src/voice/vocoder_fec.cpp): code books, whitening, soft ML against the bounded-distance decoding a
// codec does, concealment of frames that stay unreliable, and the measured gain over a noise sweep.
#include "dect2/voice/vocoder_fec.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace dect2::voicefec;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t mask1(uint32_t d0) {   // the AMBE whitening of C1 (same recurrence as the codec)
    unsigned pr = (16u * d0) & 0xFFFF; uint32_t m = 0;
    for (int k = 1; k <= 23; k++) { pr = (173u * pr + 13849u) & 0xFFFF; if (pr >> 15) m |= 1u << (23 - k); }
    return m;
}

struct Tx { char fr[4][24]; uint32_t d0, d1; };

static Tx makeFrame(std::mt19937& g) {
    Tx t; memset(t.fr, 0, sizeof(t.fr));
    t.d0 = g() & 0xFFF; t.d1 = g() & 0xFFF;
    const uint32_t w0 = golay24Encode(t.d0), w1 = golay23Encode(t.d1) ^ mask1(t.d0);
    for (int j = 0; j < 24; j++) t.fr[0][j] = (w0 >> j) & 1;
    for (int j = 0; j < 23; j++) t.fr[1][j] = (w1 >> j) & 1;
    for (int j = 0; j < 11; j++) t.fr[2][j] = g() & 1;
    for (int j = 0; j < 14; j++) t.fr[3][j] = g() & 1;
    return t;
}

// BPSK over AWGN: hard bit and a reliability from the log-likelihood ratio
static void channel(const Tx& t, double sigma, std::mt19937& g, char rx[4][24], uint8_t rel[4][24]) {
    std::normal_distribution<double> n(0.0, sigma);
    memcpy(rx, t.fr, 96);
    for (int w = 0; w < 4; w++) for (int j = 0; j < 24; j++) {
        const int len = w == 0 ? 24 : w == 1 ? 23 : w == 2 ? 11 : 14;
        if (j >= len) { rel[w][j] = 128; continue; }
        const double y = (t.fr[w][j] ? 1.0 : -1.0) + n(g);
        rx[w][j] = y > 0;
        const double llr = 2.0 * std::fabs(y) / (sigma * sigma);
        rel[w][j] = (uint8_t)std::min(255.0, llr * 12.0);
    }
}

// what a codec does with hard bits: nearest code word within 3 errors per word (Golay is perfect: always found), C1 whitened by the decoded C0
static bool hardOk(const Tx& t, const char rx[4][24]) {
    uint32_t r0 = 0, r1 = 0;
    for (int j = 0; j < 24; j++) r0 |= (uint32_t)rx[0][j] << j;
    for (int j = 0; j < 23; j++) r1 |= (uint32_t)rx[1][j] << j;
    uint32_t bestD = 0; int bestDist = 99;
    for (uint32_t d = 0; d < 4096; d++) { const int dist = __builtin_popcount(golay24Encode(d) ^ r0); if (dist < bestDist) { bestDist = dist; bestD = d; } }
    if (bestDist > 3) return false;   // the codec flags it and repeats the frame: not a wrong frame, counted as a loss
    uint32_t bd1 = 0; int dist1 = 99; const uint32_t x = r1 ^ mask1(bestD);
    for (uint32_t d = 0; d < 4096; d++) { const int dist = __builtin_popcount(golay23Encode(d) ^ x); if (dist < dist1) { dist1 = dist; bd1 = d; } }
    return bestD == t.d0 && dist1 <= 3 && bd1 == t.d1;
}

int main() {
    // the code books: Golay is perfect with minimum distance 7 (extended 8)
    {
        int minD = 99;
        for (uint32_t d = 1; d < 4096; d++) minD = std::min(minD, __builtin_popcount(golay23Encode(d)));
        CHECK(minD == 7);
        minD = 99;
        for (uint32_t d = 1; d < 4096; d++) minD = std::min(minD, __builtin_popcount(golay24Encode(d)));
        CHECK(minD == 8);
    }
    // a clean frame passes untouched
    {
        std::mt19937 g(1);
        AmbeProtector p;
        for (int i = 0; i < 50; i++) {
            const Tx t = makeFrame(g);
            char out[4][24]; const FrameInfo fi = p.processHard(t.fr, out);
            CHECK(!fi.repaired && !fi.concealed && fi.gain == 1.f);
            CHECK(memcmp(out, t.fr, 96) == 0);
        }
    }
    // up to 3 flipped bits in each word are repaired exactly, also with hard input
    {
        std::mt19937 g(2);
        AmbeProtector p;
        for (int i = 0; i < 200; i++) {
            Tx t = makeFrame(g);
            char rx[4][24]; memcpy(rx, t.fr, 96);
            for (int e = 0; e < 3; e++) { rx[0][g() % 24] ^= 1; rx[1][g() % 23] ^= 1; }
            char out[4][24]; const FrameInfo fi = p.processHard(rx, out);
            if (fi.concealed) continue;   // two flips on the same bit cancel; 3 + 3 on small words can still look unsure
            CHECK(memcmp(out[0], t.fr[0], 24) == 0 && memcmp(out[1], t.fr[1], 23) == 0);
        }
    }
    // a frame of pure noise is concealed: the last good frame is repeated with a falling gain, then silence
    {
        std::mt19937 g(3);
        AmbeProtector p;
        const Tx good0 = makeFrame(g);
        char out[4][24]; p.processHard(good0.fr, out);
        char lastGood[4][24]; memcpy(lastGood, good0.fr, 96);
        float prev = 1.f; int muted = 0;
        for (int i = 0; i < 6; i++) {
            Tx bad = makeFrame(g);
            for (int w = 0; w < 2; w++) for (int j = 0; j < 24; j++) bad.fr[w][j] = g() & 1;
            const FrameInfo fi = p.processHard(bad.fr, out);
            if (!fi.concealed) { memcpy(lastGood, out, 96); prev = 1.f; continue; }   // a random word can be near a code word (12 percent): it counts as good
            CHECK(fi.gain <= prev);
            prev = fi.gain;
            if (fi.gain == 0.f) muted++;
            CHECK(memcmp(out, lastGood, 96) == 0);
        }
    }
    // measured: soft ML against bounded-distance decoding over an AWGN sweep
    printf("sigma   hard-wrong  hard-lost  soft-wrong  soft-concealed  (percent of frames; wrong = garbage reaches the codec)\n");
    {
        std::mt19937 g(4);
        const double sig[] = {0.45, 0.55, 0.65, 0.75, 0.85};
        for (double s : sig) {
            const int N = 1500;
            int hardWrong = 0, hardLost = 0, softWrong = 0, softConc = 0;
            AmbeProtector p;
            for (int i = 0; i < N; i++) {
                const Tx t = makeFrame(g);
                char rx[4][24]; uint8_t rel[4][24];
                channel(t, s, g, rx, rel);
                // hard path
                uint32_t r0 = 0;
                for (int j = 0; j < 24; j++) r0 |= (uint32_t)rx[0][j] << j;
                int bd = 99;
                for (uint32_t d = 0; d < 4096; d++) bd = std::min(bd, __builtin_popcount(golay24Encode(d) ^ r0));
                if (bd > 3) hardLost++;
                else if (!hardOk(t, rx)) hardWrong++;
                // soft path
                char out[4][24];
                const FrameInfo fi = p.process(rx, rel, out);
                if (fi.concealed) softConc++;
                else if (memcmp(out[0], t.fr[0], 24) || memcmp(out[1], t.fr[1], 23)) softWrong++;
            }
            printf("%.2f    %6.2f      %6.2f     %6.2f       %6.2f\n", s, 100.0 * hardWrong / N, 100.0 * hardLost / N, 100.0 * softWrong / N, 100.0 * softConc / N);
            // the soft path must never let more garbage through than the hard one, and at the fringe clearly less
            CHECK(softWrong <= hardWrong + N / 100);
            if (s >= 0.75) CHECK(softWrong * 2 <= hardWrong + hardLost + 1);
        }
    }
    printf("%s\n", fails ? "FAILED" : "voice_fec OK");
    return fails ? 1 : 0;
}
