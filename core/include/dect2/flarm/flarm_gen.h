// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeFlarmSynth(): made-up gliders and light aircraft circling around a reference position send V6 packets about once a second on 868.2 and
// 868.4 MHz (radio at 868.3 MHz); one made-up aircraft has the no-track flag, one the stealth flag, one sends the newer protocol (random payload).
// Packets are encrypted for the computer's clock (the receiver's key time). SynthConfig: snrDb = signal to noise in 200 kHz; cfoHz = carrier offset;
// modeVal[0], modeVal[1] = the reference latitude, longitude (0, 0: 54.70, 25.30).
//
// flarmScene(): the building block of the tests, packets at given times on given channels with chosen levels, carrier offsets and clock error.
#pragma once
#include "flarm_proto.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

struct FlarmTx {
    double timeSec = 0.01;              // start of the packet
    int channel = 0;                    // 0 868.2 MHz, 1 868.4 MHz
    flarm::V6 pkt;                      // used when raw is empty
    uint32_t keyTime = 0;               // unix seconds the payload is encrypted for
    std::vector<uint8_t> raw;           // 24 bytes sent as they are (the newer protocol, garbage), else the encrypted pkt
    double gainDb = 0;                  // level relative to 0 dB
    double cfoHz = 0;                   // carrier offset of this transmitter
    bool badCrc = false;                // flip a CRC bit
};

// One burst at baseband (the channel's own centre): GFSK +-50 kHz, BT 0.5, Manchester chips, a short preamble. clockPpm: this transmitter's chip clock error.
std::vector<cf32> flarmBurst(const FlarmTx& tx, double rate, double clockPpm = 0);
// Radio samples centred on 868.3 MHz: every transmission mixed to its channel, plus white noise. snrDb: a 0 dB packet's power over the noise in 200 kHz
// (< -100: no noise). clockPpm: the transmitters' clock error. The noise has a fixed seed per `seed`.
std::vector<cf32> flarmScene(double rate, double durationSec, const std::vector<FlarmTx>& txs, double snrDb, double clockPpm = 0, unsigned seed = 1);

std::unique_ptr<ModeSynth> makeFlarmSynth(const SynthConfig& cfg, double sampleRate);

// The made-up aircraft of the test signal
struct FlarmDemoAircraft { uint32_t id; int addrType; int acType; bool noTrack, stealth, newProtocol; };
std::vector<FlarmDemoAircraft> flarmDemoAircraft();

} // namespace dect2
