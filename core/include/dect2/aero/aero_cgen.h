// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat Aero C channel test signal: a ground station's voice channel (8400 bit/s OQPSK, framing and coding of aero_cchan.h) carrying
// pseudo-random coded voice frames (no real vocoder: the content is random bytes, the frames are numbered so a test can compare), fill-in
// signal units (type 0x01, GES id in octet 4, as seen on a real channel) and a call-progress unit (type 0x30, AES id, GES id) now and then.
// Only for tests and the synthetic source.
#pragma once
#include "aero_cchan.h"
#include "aero_demod.h"
#include <array>
#include <memory>
#include <vector>

namespace dect2 {

struct AeroCGenConfig {
    double fs = 48000;            // output sample rate
    double carrierHz = 0;         // carrier offset against 0 Hz
    double clockPpm = 0;          // bit clock error
    double amplitude = 1.0;       // rms of the signal (the modulator makes unit power)
    double ebn0Db = 0;            // > 0: add white noise for this Eb/N0 per channel bit (relative to `amplitude`)
    uint32_t seed = 1;
    uint32_t aesId = 0x7812E7;
    uint8_t gesId = 0x90;
    int callEvery = 6;            // a call-progress unit in every n-th frame (0 = never)
    bool voiceIdle = false;       // true: every voice frame the same (a channel with no talker)
};

class AeroCGenerator {
public:
    explicit AeroCGenerator(const AeroCGenConfig& cfg);
    ~AeroCGenerator();
    void generate(cf32* out, size_t n);
    // every voice frame sent so far (12 bytes each), oldest first; frame k went out in the frame number k / 25
    const std::vector<std::array<uint8_t, 12>>& sentVoice() const;
    uint64_t framesSent() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
