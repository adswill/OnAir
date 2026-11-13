// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// e-Czas test signal: the 225 kHz carrier with the phase keying of the GUM description (50 bit/s, a bit that changes the keying state is a 36 degree
// phase ramp of 19 ms, the 20 slots of the minute frame; some slots carry the time packet, the others other packets). Nothing is
// transmitted: the samples only exist in memory or in a file.
//
// SynthConfig (makeEczasSynth): snrDb (carrier to noise in 100 Hz), cfoHz. The test signal carries the computer's clock, so the app shows the
// computer clock as right (the offset then shows the path delay only).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "eczas_proto.h"
#include <cstdint>
#include <memory>

namespace dect2 {

struct EczasGenConfig {
    double rate = 256000;
    double dialOffsetHz = 0;      // where the carrier sits in the samples
    double snrDb = 40;            // carrier to noise in 100 Hz; >= 99: no noise
    double cfoHz = 0;
    double amp = 0.3;
    double startUtc = 0;          // UTC epoch seconds of the first sample (0: the computer's clock now)
    double delayMs = 0;           // the path delay: the frames arrive this much after UTC
    double clockPpm = 0;          // the receiving radio's sample clock error
    int tzHours = -1;             // 0..3; -1: Poland's local time (+1 in winter, +2 from the last Sunday of March to the last Sunday of October)
    uint8_t flags = 0;            // LS (4), LSS (2), TZC (1) of the time packet
    uint8_t txState = 0;
    int timeSlotStride = 4;       // every this many slots (0, 4, 8 ... of the 20 of a minute) carry the time packet (the recordings show time packets in many slots)
    bool otherSlots = true;       // the other slots carry other packets (header 0x60 + slot, not the time packet)
    bool carrierOnly = false;     // no keying at all
    double amDepth = 0;           // slow amplitude fading, depth 0..1 (tests)
    uint32_t seed = 1;
};

class EczasGenerator {
public:
    explicit EczasGenerator(const EczasGenConfig& c);
    ~EczasGenerator();
    void generate(cf32* out, size_t n);
    // the 96 bits of the frame in slot `slot` of the minute that starts at UTC epoch second minuteStart
    static void frameFor(int64_t minuteStart, int slot, const EczasGenConfig& c, uint8_t out[eczas::kFrameBytes]);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Poland's offset from UTC in hours at this UTC epoch second (1 in winter, 2 in summer time)
int polandOffsetHours(int64_t utcSec);

std::unique_ptr<ModeSynth> makeEczasSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
