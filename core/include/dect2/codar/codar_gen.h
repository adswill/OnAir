// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Complex white noise plus linear FM sweeps (sawtooth, 10% blanking at the end of each sweep) of several radars at once; one radar
// can send a Morse call sign (the whole sweep width keyed on and off, fast repeated sweeps while the key is down) in a slot of its cycle.
//
// SynthConfig (makeCodarSynth): snrDb (of the strongest radar, in a 1 kHz bin); modeOpt[0]: 0 the schedule (3 radars), 1 noise only,
// 2 the schedule plus the ID of radar 1 (14 s cycle: 7 s of sweeps, then the ID), 3 a continuous carrier and a 200 kHz/s wide ionosonde chirp
// only (nothing the detector may count).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct CodarGenRadar {
    double offsetHz = 0;        // sweep centre from the radio's centre
    double bwHz = 50e3;
    double periodSec = 0.5;
    int dir = 1;                // +1 up, -1 down
    double relDb = 0;           // level against the config's snrDb
    double startSec = 0;        // where in the repetition the sweep starts
    std::string callsign;       // keyed during the ID slot ("" none)
    double wpm = 15;
    double idCycleSec = 14;     // the slot is the second half of the cycle
};

struct CodarGenConfig {
    double rate = 2e6;
    double snrDb = 20;          // strongest radar, per 1 kHz bin
    double blank = 0.1;
    uint32_t seed = 1;
    bool noiseOnly = false;
    bool junk = false;          // the carrier and the wide chirp
    std::vector<CodarGenRadar> radars;
};

class CodarGenerator {
public:
    explicit CodarGenerator(const CodarGenConfig& c);
    ~CodarGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<CodarGenRadar> codarTestRadars(bool withId);   // three radars: 50 kHz up / 0.5 s, 100 kHz down / 1 s, 25 kHz up / 0.5 s

std::unique_ptr<ModeSynth> makeCodarSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
