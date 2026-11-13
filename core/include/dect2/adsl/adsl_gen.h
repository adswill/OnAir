// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-L test signal. Nothing is transmitted: the samples only exist in memory or in a file.
// adslBurst(): one frame at baseband. adslScene(): frames on their channels with chosen levels, carrier offsets and clock errors, on a capture tuned to centreHz.
// makeAdslSynth(): made-up aircraft around the reference position, Traffic and Status on the M-band, Traffic on O-LDR, a Traffic Uplink with FEC on O-HDR.
#pragma once
#include "adsl_proto.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

struct AdslTx {
    double timeSec = 0;
    adsl::Band band = adsl::BandM0;
    std::vector<uint8_t> net;           // network packet (makeNetwork)
    double gainDb = 0, cfoHz = 0;
    int preamble = -1;                  // preamble repeats, -1 the default
    bool garbleLength = false;          // M-band / HDR: the unprotected length byte hit by errors
};

std::vector<uint8_t> adslTrafficNet(const adsl::Traffic& t, int version = 1, int keyIndex = 0, bool fec = false);
std::vector<cf32> adslBurst(const AdslTx& tx, double rate, double clockPpm = 0);
std::vector<cf32> adslScene(double rate, double centreHz, double durationSec, const std::vector<AdslTx>& txs, double snrDb, double clockPpm = 0, unsigned seed = 3);
std::unique_ptr<ModeSynth> makeAdslSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
