#include "dect2/voice/vocoder_fec.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace dect2::voicefec {

namespace {

std::atomic<bool> gOn{true};

constexpr uint32_t kGenPoly = 0xC75;   // x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1, the generator of the cyclic Golay (23,12) code

struct Book {
    uint32_t w23[4096];
    uint32_t w24[4096];
    Book() {
        for (uint32_t d = 0; d < 4096; d++) {
            uint32_t r = d << 11;
            for (int i = 22; i >= 11; i--) if (r & (1u << i)) r ^= kGenPoly << (i - 11);
            const uint32_t cw = (d << 11) | (r & 0x7FF);
            w23[d] = cw;
            w24[d] = (cw << 1) | (uint32_t)__builtin_parity(cw);
        }
    }
};
const Book& book() { static const Book b; return b; }

struct Cand { uint32_t data; int cost; int flips; };

// the best three code words by cost
void best3(uint32_t rx, const uint8_t* rel, int n, Cand out[3]) {
    const uint32_t* w = n == 24 ? book().w24 : book().w23;
    for (int i = 0; i < 3; i++) out[i] = {0, 1 << 30, 0};
    for (uint32_t d = 0; d < 4096; d++) {
        uint32_t diff = w[d] ^ rx;
        int c = 0;
        while (diff) { c += rel[__builtin_ctz(diff)]; diff &= diff - 1; }
        if (c >= out[2].cost) continue;
        Cand cur{d, c, __builtin_popcount(w[d] ^ rx)};
        if (c < out[0].cost) { out[2] = out[1]; out[1] = out[0]; out[0] = cur; }
        else if (c < out[1].cost) { out[2] = out[1]; out[1] = cur; }
        else out[2] = cur;
    }
}

// the whitening of C1 from the 12 data bits of C0 (the AMBE modulation sequence): bit j of the result is the mask of fr[1][j]
uint32_t c1Mask(uint32_t c0data) {
    unsigned pr = (16u * c0data) & 0xFFFF;
    uint32_t m = 0;
    for (int k = 1; k <= 23; k++) {
        pr = (173u * pr + 13849u) & 0xFFFF;
        if (pr >> 15) m |= 1u << (23 - k);
    }
    return m;
}

} // namespace

bool enabled() { return gOn.load(); }
void setEnabled(bool on) { gOn.store(on); }

uint32_t golay23Encode(uint32_t d) { return book().w23[d & 0xFFF]; }
uint32_t golay24Encode(uint32_t d) { return book().w24[d & 0xFFF]; }

Decoded golaySoft(uint32_t rx, const uint8_t* rel, int n) {
    Cand c[3];
    best3(rx, rel, n, c);
    Decoded d;
    d.data = c[0].data;
    d.word = n == 24 ? golay24Encode(c[0].data) : golay23Encode(c[0].data);
    d.cost = c[0].cost; d.flips = c[0].flips;
    d.margin = c[1].cost - c[0].cost;
    return d;
}

FrameInfo AmbeProtector::process(const char fr[4][24], const uint8_t rel[4][24], char out[4][24]) {
    FrameInfo fi;
    frames++;
    memcpy(out, fr, 96);
    uint32_t rx0 = 0, rx1 = 0;
    uint8_t r0[24], r1[23];
    for (int j = 0; j < 24; j++) { rx0 |= (uint32_t)(fr[0][j] & 1) << j; r0[j] = rel[0][j]; }
    for (int j = 0; j < 23; j++) { rx1 |= (uint32_t)(fr[1][j] & 1) << j; r1[j] = rel[1][j]; }

    const Book& bk = book();
    const uint32_t d0 = (rx0 >> 13) & 0xFFF;
    const uint32_t m0 = c1Mask(d0);
    const uint32_t x1 = rx1 ^ m0;
    bool bad = false;
    if (bk.w24[d0] == rx0 && bk.w23[(x1 >> 11) & 0xFFF] == x1) {
        // both words are code words as received: nothing to correct
    } else {
        Cand c0[3];
        best3(rx0, r0, 24, c0);
        int bestTotal = 1 << 30, secondTotal = 1 << 30, bi = 0;
        Cand bestC1{};
        for (int i = 0; i < 3; i++) {
            if (c0[i].cost >= (1 << 30)) continue;
            Cand c1[3];
            best3(rx1 ^ c1Mask(c0[i].data), r1, 23, c1);
            const int t = c0[i].cost + c1[0].cost;
            if (t < bestTotal) { secondTotal = bestTotal; bestTotal = t; bi = i; bestC1 = c1[0]; }
            else if (t < secondTotal) secondTotal = t;
        }
        const uint32_t w0 = bk.w24[c0[bi].data];
        const uint32_t w1 = bk.w23[bestC1.data] ^ c1Mask(c0[bi].data);
        for (int j = 0; j < 24; j++) out[0][j] = (char)((w0 >> j) & 1);
        for (int j = 0; j < 23; j++) out[1][j] = (char)((w1 >> j) & 1);
        fi.c0Flips = c0[bi].flips; fi.c1Flips = bestC1.flips; fi.cost = bestTotal;
        fi.repaired = fi.c0Flips + fi.c1Flips > 0;
        // an unreliable decision: too many bits changed (the Golay code is perfect: beyond 3 errors a word is just as near a wrong code word),
        // or a runner-up nearly as good
        const int margin = secondTotal - bestTotal;
        bad = fi.c0Flips >= 4 || fi.c1Flips >= 5 || fi.c0Flips + fi.c1Flips >= 7 || margin < 40;
    }

    if (fi.repaired) repairedFrames++;
    if (!bad) {
        memcpy(last_, out, 96);
        have_ = true; repeats_ = 0;
        return fi;
    }
    // conceal: repeat the last good frame with a falling gain, then silence
    fi.concealed = true; fi.repaired = false;
    concealedFrames++; repairedFrames -= fi.c0Flips + fi.c1Flips > 0 ? 1 : 0;
    repeats_++;
    static const float kGain[kMaxRepeats + 1] = {1.f, 0.7f, 0.4f, 0.15f};
    fi.gain = repeats_ <= kMaxRepeats ? kGain[repeats_] : 0.f;
    if (have_) memcpy(out, last_, 96);
    else fi.gain = 0.f;
    return fi;
}

FrameInfo AmbeProtector::processHard(const char fr[4][24], char out[4][24]) {
    uint8_t rel[4][24];
    memset(rel, 128, sizeof(rel));
    return process(fr, rel, out);
}

void permuteReliability(const int perm[96], const uint8_t rel72[72], uint8_t rel[4][24]) {
    for (int i = 0; i < 96; i++) rel[i / 24][i % 24] = perm[i] >= 0 && perm[i] < 72 ? rel72[perm[i]] : 128;
}

} // namespace dect2::voicefec
