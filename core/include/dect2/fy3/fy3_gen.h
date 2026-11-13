// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FY-3 AHRPT / MPT test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A test picture coded as fy3_proto.h has it: VIRR frames (AHRPT of FY-3A / B / C, virtual channel 5) or MERSI scans (MPT, virtual channel 3; or AHRPT
// of a later satellite) in VCDUs, Reed-Solomon, randomised, sync word, the differential coder, each rail through the CCSDS convolutional code (G2
// inverted), QPSK with a root raised cosine. Over complex white noise.
//
// SynthConfig (makeFy3Synth): snrDb (Es/N0 in the symbol rate), cfoHz, modeOpt[0] the satellite (index into fy3::sats(), 2 = FY-3C), modeOpt[1] bit 0 the
// Q rail negated (a spectrum inverted by a down-converter), bit 1 the rails exchanged; modeVal[0] a symbol rate (0: the product's usual one), modeVal[1] a
// roll-off (0: the usual one).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct Fy3GenConfig {
    double rate = 8e6;
    int product = 0;             // 0 AHRPT, 1 MPT
    int sat = 2;                 // index into fy3::sats()
    double symbolRate = 0;       // 0: 2.6 Msym/s (AHRPT of FY-3C) or 18.7 Msym/s (MPT)
    double rolloff = 0;          // 0: 0.5
    double snrDb = 30;           // Es/N0 in the symbol rate; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;
    double driftHzPerS = 0;      // carrier offset change (Doppler rate)
    double phaseDeg = 0;
    bool conjugate = false;      // the Q rail negated
    bool swapIq = false;
    double amp = 0.25;
    double duty = 0.5;           // share of the frames that carry the instrument (the rest is fill, as on the real downlink)
    int inst = 0;                // 0: VIRR for AHRPT of FY-3A / B / C, else MERSI by the satellite; or a fy3::Inst value
    uint32_t seed = 1;
};

class Fy3Generator {
public:
    explicit Fy3Generator(const Fy3GenConfig& c);
    ~Fy3Generator();
    void generate(cf32* out, size_t n);
    double symbolRate() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeFy3Synth(const SynthConfig& cfg, double sampleRate, int product);

} // namespace dect2
