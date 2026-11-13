// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS physical layer wrapper: bursts, reception, the Watterson channel and the 12 kHz audio / WAV conversion, all through the Rust crate rust/onair_kaws.
#pragma once
#include "dect2/kaws/kaws_frame.h"
#include <complex>
#include <string>
#include <vector>

namespace dect2::kaws {

using cf = std::complex<float>;
constexpr int kBaseRate = 2400;      // baseband samples per second (one per chip)
constexpr int kAudioRate = 12000;    // WAV / sound card rate
constexpr int kNumProfiles = 8;

struct ProfileInfo {
    int id;
    const char* name;
    const char* modulation;
    int K, N;           // information / coded bits per block
    int slotSec;        // 0 = not slotted (push to talk)
    int units;          // lattice units in the preamble (front + midambles)
    double estAwgnDb;   // spec ESTIMATE of the AWGN threshold
};
inline constexpr ProfileInfo kProfiles[kNumProfiles] = {
    {0, "Abyss", "chirp SF10 x3", 128, 512, 120, 10, -29.6},
    {1, "Deep", "chirp SF10", 256, 1024, 60, 2, -25.2},
    {2, "Weak", "chirp SF9", 336, 1008, 60, 1, -21.4},
    {3, "Chat", "chirp SF8", 512, 1024, 30, 1, -17.1},
    {4, "Mid", "chirp SF6+guard", 510, 1020, 15, 1, -13.3},
    {5, "Fast data", "OFDM BPSK", 360, 1440, 15, 1, -7.4},
    {6, "Voice-700", "OFDM QPSK", 600, 1728, 0, 1, -2.3},
    {7, "Voice-1200", "OFDM QPSK", 900, 1728, 0, 1, 0.0},
};

// Samples (at 2400/s) of a burst of nblocks blocks; 0 for a bad profile. Seconds = samples / 2400.
int burstSamples(int profile, int nblocks);
// The burst: every block is K bits. Mean power 1. Empty on error.
std::vector<cf> buildBurst(int profile, const std::vector<Bits>& blocks);

struct RxBurst {
    bool sync = false;
    double stat = 0, tau = 0, deltaHz = 0, snrDb = -99;
    std::vector<Bits> blocks;    // K bits each (zeros where not decoded)
    std::vector<int> iters;      // LDPC iterations, -1 = not decoded
};
// The first lattice unit starts in [searchLo, searchHi] samples of `iq` (the range is clamped so that a lattice unit fits in the stream).
RxBurst receiveBurst(int profile, int nblocks, const std::vector<cf>& iq, int searchLo, int searchHi);

enum class Channel : int { Awgn = 0, Moderate = 1, Poor = 2 };
std::vector<cf> applyChannel(const std::vector<cf>& iq, Channel c, double snrDb, double freqOffsetHz, double driftHzPerS, double startDelaySamples, int tail, uint64_t seed);

std::vector<float> toAudio12k(const std::vector<cf>& iq, float peak = 0.9f);   // 5 x samples
std::vector<cf> fromAudio12k(const std::vector<float>& audio);                  // len / 5 samples

// 16-bit mono PCM WAV files.
bool writeWav(const std::string& path, const std::vector<float>& audio, int rate = kAudioRate);
bool readWav(const std::string& path, std::vector<float>& audio, int& rate);

}  // namespace dect2::kaws
