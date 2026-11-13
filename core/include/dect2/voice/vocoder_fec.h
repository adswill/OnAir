// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Soft-decision protection of the AMBE / AMBE+2 3600 bit/s vocoder frames (DMR, D-STAR, NXDN, dPMR, P25 phase 2) with concealment.
//
// The vocoder frame is four code words C0..C3 of 24 / 23 / 11 / 14 bits (the mbelib layout): C0 is an extended Golay (24,12), C1 a Golay (23,12)
// whitened by a sequence seeded from the C0 data, C2 and C3 carry no protection. The speech codec (user-installed, docs/CODECS.md) only sees hard
// bits and corrects at most 3 errors per word. This stage does the work before it, from the symbol reliabilities of the demodulator:
//   - maximum-likelihood decoding of C0 over its 4096 code words with the reliability of every bit as the cost of disagreeing with it (instead of
//     bounded-distance decoding), jointly with C1 (the best few C0 candidates are each tried with their own whitening, the pair with the lowest
//     total cost wins: a wrong C0 shows up as a hopeless C1);
//   - the corrected frame is re-encoded, so the codec sees clean code words;
//   - a frame that is still unreliable is concealed instead of letting garbage through: the last good frame is repeated with a falling gain and the
//     audio fades to silence after a few frames.
// Only the bit path changes; no codec is bundled. Settings > Advanced can turn it off (voicefec::setEnabled).
#pragma once
#include <cstdint>

namespace dect2::voicefec {

bool enabled();
void setEnabled(bool on);

// Golay (23,12): the code word for 12 data bits (data in bits 22..11, parity in 10..0), and the extended (24,12) word (parity bit in bit 23).
uint32_t golay23Encode(uint32_t data12);
uint32_t golay24Encode(uint32_t data12);

struct Decoded {
    uint32_t data = 0;      // 12 data bits
    uint32_t word = 0;      // the chosen code word (23 or 24 bits)
    int cost = 0;           // sum of the reliabilities of the received bits that disagree with it
    int flips = 0;          // number of received bits that disagree with it
    int margin = 0;         // cost of the runner-up code word minus cost (how unambiguous the decision is)
};

// Maximum likelihood over all 4096 code words. rx: received hard bits (bit i = position i), rel: reliability per bit position 0..255.
// n = 23 or 24 (extended).
Decoded golaySoft(uint32_t rx, const uint8_t* rel, int n);

struct FrameInfo {
    int c0Flips = 0, c1Flips = 0;   // bits changed by the correction
    int cost = 0;                   // total reliability cost of the correction
    bool repaired = false;          // the correction changed something
    bool concealed = false;         // the frame was replaced (repeat) or muted
    float gain = 1.f;               // gain for the 20 ms of audio of this frame (1 normal, falling on repeats, 0 muted)
};

// One per speaker. fr: the received frame, rel: the reliability of every bit (0..255), same layout; out: the frame to give to the codec.
class AmbeProtector {
public:
    FrameInfo process(const char fr[4][24], const uint8_t rel[4][24], char out[4][24]);
    // hard-bit input: every bit is as reliable as any other (nothing to gain over the codec's own correction, but the concealment still works)
    FrameInfo processHard(const char fr[4][24], char out[4][24]);
    void reset() { have_ = false; repeats_ = 0; }
    int repairedFrames = 0, concealedFrames = 0, frames = 0;   // counters since the start of the call (the receiver shows them in the stats)
    static constexpr int kMaxRepeats = 3;   // after this many concealed frames in a row the audio is muted
private:
    char last_[4][24] = {};
    bool have_ = false;
    int repeats_ = 0;
};

// The 72 voice bits of a frame (the over-the-air order) to the code word layout, for reliabilities that are carried in the over-the-air order.
// perm[i] = index into the 72 over-the-air bits of fr[i / 24][i % 24] (from the frame table of the mode: nxdn / dstar).
template <class FrameFn>
void permutationOf(FrameFn frame, int perm[96]) {   // frame(const uint8_t bits[72], char fr[4][24]): the mode's de-interleaver
    for (int i = 0; i < 96; i++) perm[i] = -1;
    for (int b = 0; b < 72; b++) {
        uint8_t bits[72] = {}; char fr[4][24] = {};
        bits[b] = 1;
        frame(bits, fr);
        for (int i = 0; i < 96; i++) if (fr[i / 24][i % 24]) perm[i] = b;
    }
}
void permuteReliability(const int perm[96], const uint8_t rel72[72], uint8_t rel[4][24]);

} // namespace dect2::voicefec
