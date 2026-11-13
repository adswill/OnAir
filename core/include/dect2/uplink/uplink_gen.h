// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink test signal (1030 MHz interrogations). Nothing is transmitted: the samples only exist in memory or in a file.
//
// UplinkGenerator renders the interrogations it is given: Mode A/C (P1, P2 at its own level, P3 8 or 21 us, optional P4 for the intermode
// all-calls) and Mode S (P1, P2, P6 with the sync and data phase reversals, optional P5 over the sync reversal from a control antenna).
// Pulses have 50 ns edges; each transmission has its own carrier phase and offset; the sum goes through a [1/4 1/2 1/4] filter (the phase
// reversals dip the envelope as a band-limited receiver sees them), then white noise.
//
// The scene (makeUplinkSynth, uplinkScene): a Mode S radar II 3 (4.8 s rotation: UF11 all-calls, intermode A/C/S all-calls, Mode A/C,
// roll-calls UF4/5/20/21 with Comm-B requests BDS 2,0 / 4,0 / 5,0 / 6,0), a Mode S radar SI 17 (10 s, DI 3 roll-calls), a Mode A/C SSR
// (6 s, sidelobe interrogations with P2 6 dB above P1 between its main-beam bursts), and an ACAS aircraft 4CA2B1 sending UF0 to 3C6586
// every second and a UF16 RA coordination every 5 s.
// SynthConfig (makeUplinkSynth): snrDb main-beam level over the noise, cfoHz the radio's tuning error.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/common/stable_random.h"
#include "uplink_msg.h"
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace dect2 {

struct UplinkTx {
    double sec = 0;          // P1 leading edge
    int kind = 0;            // 0 Mode A/C, 1 Mode S
    char acMode = 'A';
    int p4 = 0;              // 0 none, 1 0.8 us (A/C-only all-call), 2 1.6 us (A/C/S all-call)
    float levelDb = -12;     // dBFS of P1 (P3, P4, P6 the same)
    float p2Db = -99;        // P2 against P1 (Mode S: always 0); -99 none
    bool p5 = false;         // Mode S: P5 over the sync reversal
    float p5Db = 3;
    uplink::Frame frame;
    double cfoHz = 0;        // on top of the generator's
    float phase = 0;
};

class UplinkGenerator {
public:
    UplinkGenerator(double rate, double noiseRms, double cfoHz = 0, uint32_t seed = 1);
    void add(const UplinkTx& t);          // in time order is not needed
    void generate(cf32* out, size_t n);
    double timeSec() const { return (double)pos_ / rate_; }
    double rate() const { return rate_; }
private:
    struct Ev { UplinkTx tx; double t0 = 0, t1 = 0; float a = 0, a2 = 0, a5 = 0; std::vector<double> flips; };
    double rate_, noise_, cfo_;
    uint64_t pos_ = 0;
    std::vector<Ev> ev_;
    cf32 h1_, h2_;
    std::mt19937 rng_;
    dect2::normal_distribution<float> nd_{0.f, 1.f};
};

// The interrogations of the test scene that start in [t0, t1) seconds
void uplinkScene(double t0, double t1, std::vector<UplinkTx>& out, float mainDb);

std::unique_ptr<ModeSynth> makeUplinkSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
