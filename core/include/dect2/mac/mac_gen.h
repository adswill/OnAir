// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC television test signal (D2-MAC/packet or D-MAC/packet over FM, as a satellite sends it). Nothing is transmitted: the samples only
// exist in memory or in a file.
//
// A loop of frames (two by default, 80 ms) is built once: the analog TV test card as time-compressed components (3:2 luminance, 3:1 colour
// difference on alternate lines), the duobinary data bursts with the packet multiplex (companded 32 kHz mono sound with a tone, the service
// identification with the channel name, dummy packets, ECM packets when scrambled), line 624 references and line 625 (frame sync, SDF with
// the aspect and scrambling flags, RDF). Pre-emphasis, 25 Hz energy dispersal, FM at 13.5 MHz/V on a 40.5 Msps grid, then the radio's
// band (0.96 of the output rate) and the output rate. The loop plays with fresh noise, the carrier offset and the FM phase carried on.
// Scrambled: the vision by single-cut line rotation (random cut points), the sound block by a pseudo-random sequence, and the service
// marked as under EuroCrypt conditional access (VSAM, ACCM): a receiver can only detect it.
//
// SynthConfig (makeMacSynth): snrDb (carrier to noise in 27 MHz), cfoHz; modeOpt[0] 0 D2-MAC / 1 D-MAC, [1] scrambled, [2] 16:9,
// [3] picture (0 test card, 1 colour bars, 2 grey ramp); modeVal[0] tone Hz (0: 1000).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct MacGenConfig {
    int variant = 0;              // mac::kD2, mac::kD
    double rate = 20e6;
    double snrDb = 30;            // carrier to noise in 27 MHz; >= 99: no noise
    double cfoHz = 0;
    double amp = 0.3;
    bool scrambled = false;       // EuroCrypt: video line rotation, sound scrambled, ACCM / VSAM set
    bool wide = false;            // 16:9 flag (the picture is the same, squeezed)
    int pattern = 0;              // test card, colour bars, grey ramp
    std::string name = "ONAIR TV", network = "ONAIR SATELLITE", country = "LITHUANIA";
    double toneHz = 1000, toneDbfs = -10;
    bool preEmphasis = true, dispersal = true;
    bool invert = false;          // spectrum inversion (the radio's I and Q swapped)
    double radioBwHz = 0;         // the radio's filter after FM (0: 0.96 x rate, up to 36 MHz)
    int loopFrames = 2;
    uint16_t chid = 0x1A2B;
    uint32_t seed = 1;
};

class MacGenerator {
public:
    explicit MacGenerator(const MacGenConfig& c);
    ~MacGenerator();
    void generate(cf32* out, size_t n);
    // the 20.25 MHz baseband of the loop (0..1 V, before pre-emphasis), and the frame the test card made for loop frame k (RGBA, 768 x 576)
    const std::vector<float>& baseband() const;
    void referenceFrame(int k, std::vector<uint8_t>& rgba) const;
    int loopFrames() const;
    static double noiseSigma(const MacGenConfig& c);   // complex rms of the noise at the output rate
    static constexpr int kSoundAddress = 224, kEcmAddress = 346;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeMacSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
