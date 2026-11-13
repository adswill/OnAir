// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos test signal. Nothing is transmitted: the samples only exist in memory or in a file. Made-up platforms (the IDs are not registered
// ones) and made-up downlink frames. The 24 synchronisation bits of an A2 message are a stand-in (0x5A5A5A): the real pattern is in a CNES
// document that is not public; the length field holds k (the number of 32 bit words after the first 24 data bits), also a stand-in.
//
// makeArgosSynth(): the dial sits argosTuning().tuneOffsetHz below the radio's centre plus SynthConfig::cfoHz.
//   uplink (modeOpt[0] = 0): PTT-A2 bursts from three platforms in turn, each 160 ms of carrier then the message at 400 bit/s, Manchester,
//     +-1.1 rad, at their own frequency within the 401.618 - 401.680 MHz band; snrDb is the carrier power over the noise power in 2.4 kHz.
//   downlink (modeOpt[0] = 1): a continuous 400 bit/s Manchester signal with HDLC frames and flags in between, following a pass's Doppler
//     curve; snrDb is the carrier power over the noise power in 2.4 kHz.
//
// SynthConfig::modeOpt:  [0] 0 uplink, 1 downlink   [1] downlink: 1 = BPSK (+-90 degrees) instead of +-1.1 rad
//                        [2] uplink: 1 = also a burst of another modulation (carrier, then QPSK) each cycle; downlink: 1 = NRZI bits
//                        [3] noise seed, 0 = 1
// SynthConfig::modeVal:  [0] uplink: the carrier drifts this much during each burst (Hz/s); downlink: the largest Doppler of the pass (Hz, 0 = none)
//                        [1] uplink: seconds between bursts (0 = 2 s); downlink: the pass's time constant in seconds (0 = 60)
//                        [2] uplink: each platform's next burst is this many Hz higher (Doppler / drift between bursts)
//                        [3] downlink: seconds into the signal of the closest approach (0 = 40)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/argos/argos_proto.h"
#include <complex>
#include <memory>
#include <vector>

namespace dect2::argos {
struct TestPtt { uint32_t id; std::vector<uint8_t> data; int dataBits; double offsetHz; };
std::vector<TestPtt> testPtts();
constexpr uint32_t kTestSync = 0x5A5A5A;
// One A2 burst (160 ms carrier, then the bits) as unit-amplitude samples at `rate` with the carrier at carrierHz drifting driftHzPerSec
std::vector<std::complex<float>> a2BurstSamples(const std::vector<uint8_t>& bits, double rate, double carrierHz, double driftHzPerSec = 0);
std::vector<std::vector<uint8_t>> testDownlinkFrames();   // the payloads of the downlink test signal, in order
// The Doppler the downlink test signal has at time t (seconds into it)
double testDoppler(double t, double maxHz, double tauSec, double tcaSec);
}

namespace dect2 {
std::unique_ptr<ModeSynth> makeArgosSynth(const SynthConfig& cfg, double sampleRate);
}
