// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C test signal: the simulated airspace of the ADS-B generator (Mode S / ADS-B messages from aircraft around a reference point) with the
// old secondary-radar replies mixed in. Every aircraft answers interrogations with Mode A (its squawk) and Mode C (its altitude, 100 ft steps,
// Gillham code) replies at random times, at a level that falls with distance, with a carrier error of its own; replies and Mode S messages overlap
// now and then, as they do on the air. Nothing is transmitted: the samples only exist in memory or in a file.
//
// SynthConfig (makeModeAcSynth): snrDb (as for ADS-B: pulse over the noise of an aircraft 50 NM away, noise in 2 MHz, default 30), cfoHz;
// modeOpt[0] aircraft (0 = 12, at most 300, -1 none); modeOpt[1] 0 with the Mode S traffic, 1 replies only; modeOpt[2] random seed;
// modeOpt[3] 0 nothing special, 1 one aircraft squawks 7700, 2 one aircraft has its ident (SPI) pulse on for 18 s of every 60;
// modeVal[0] reply rate multiplier (0 = 1; about 2 replies per second per aircraft at 1); modeVal[1] radio filter width in MHz (0 = automatic);
// modeVal[2], modeVal[3] reference latitude and longitude (both 0: 25.25 N 55.36 E).
#pragma once
#include "dect2/adsb/adsb_gen.h"
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct ModeAcTrue {
    double t = 0;            // start of F1, seconds of stream time
    uint16_t bits = 0;       // information positions
    bool spi = false;
    bool isC = false;
    uint32_t icao = 0;
    float amp = 0;
};

struct ModeAcGenConfig {
    double rate = 4e6;
    double snrDb = 30;
    double cfoHz = 0;
    double filterMHz = 0;
    int aircraft = 12;       // 0 = none
    bool modeS = true;       // also the Mode S traffic
    uint32_t seed = 1;
    double replyRate = 1.0;
    int special = 0;         // modeOpt[3]
    double refLat = 25.25, refLon = 55.36;
};

class ModeAcGenerator {
public:
    explicit ModeAcGenerator(const ModeAcGenConfig& c);
    ~ModeAcGenerator();
    void generate(cf32* out, size_t n);
    const std::vector<ModeAcTrue>& sent() const;         // every reply placed so far
    struct Plane { uint32_t icao; std::string callsign; int squawk; double altFt; bool ground; };
    std::vector<Plane> planes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeModeAcSynth(const SynthConfig& cfg, double sampleRate);   // nullptr below 2 Msps

} // namespace dect2
