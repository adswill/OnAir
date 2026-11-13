// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// JS8Call test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Stations in white noise, each sending one message (a heartbeat, a CQ, a directed command, free text over several frames, a relay)
// in its submode's slots, again every few slots: 8-FSK, continuous phase, 79 symbols, with its own audio frequency, SNR (in 2500 Hz),
// time offset, drift. The slots start at sample 0 plus slotPhaseSec (makeJs8Synth: in step with the system clock, as the receiver).
//
// SynthConfig (makeJs8Synth):
//   snrDb       added to every station's SNR
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the test stations (js8TestStations), 1 only the first
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct Js8GenStation {
    enum Kind { Heartbeat, Cq, Directed, Text };
    Kind kind = Heartbeat;
    int submode = 0;
    std::string call = "LY2ABC", grid = "KO24", to;
    int cmd = 31, num = 0;       // Directed: the command code and its number (an SNR); Text: 31 free text or 5 relay
    std::string text;            // Text: the free text
    double hz = 1000;            // audio frequency of tone 0
    double snrDb = -5;           // in 2500 Hz
    double dt = 0;               // s, start against the nominal one
    double driftHzPerMin = 0;
    int firstSlot = 0;           // the slot of its first frame
    int everySlots = 4;          // its message starts again this many slots after the last frame (0: once)
};

struct Js8GenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;
    double cfoHz = 0;
    double noiseSigma = 0.02;
    uint32_t seed = 1;
    double slotPhaseSec = 0;     // the slot clock of sample 0 (seconds; slots start at multiples of the period)
    std::vector<Js8GenStation> stations;
};

class Js8Generator {
public:
    explicit Js8Generator(const Js8GenConfig& c);
    ~Js8Generator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<Js8GenStation> js8TestStations();
std::unique_ptr<ModeSynth> makeJs8Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
