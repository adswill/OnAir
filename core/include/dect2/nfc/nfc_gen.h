// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Synthetic NFC / RFID test signal for the tests and the "synthetic" source. NOTHING IS TRANSMITTED: the samples only exist in
// memory or in a file. It renders a scripted reader <-> tag exchange on 13.56 MHz baseband (carrier at carrierHz), so the receiver
// and its loopback tests have a signal without any real tag or reader present.
//
// Sessions (SynthConfig::modeOpt[0]): 0 = ISO/IEC 14443 Type A (REQA, ATQA, anticollision, SELECT, SAK, READ with an NDEF URI),
// 1 = ISO/IEC 15693 inventory + system information, 2 = ISO/IEC 14443 Type B (REQB, ATQB). The matched demodulator in nfc_rx.h
// recovers these frames; the exchange is a fabricated example, not a captured or cloned tag.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::nfc {

struct GenConfig {
    double fs = 4e6;
    double carrierHz = 0;      // where the 13.56 MHz carrier sits in the baseband (the receiver's signal offset)
    double amp = 0.3;          // carrier amplitude
    double loadDepth = 0.35;   // tag load-modulation depth (synthetic; real load modulation is far weaker). Kept below the reader
                               // pause threshold so a tag answer is never mistaken for a 100% ASK reader frame.
    double snrDb = 1e9;        // carrier power over noise power; 1e9 = clean
    double cfoHz = 0;
    int session = 0;           // 0 Type A, 1 ISO 15693, 2 Type B
    uint32_t seed = 1;
};

// One pass of the scripted exchange, clean (no noise, no carrier offset unless carrierHz is set).
std::vector<cf32> renderSession(const GenConfig& g);

// The generator the engine's synthetic source plays (loops the session, adds noise and carrier offset).
std::unique_ptr<ModeSynth> makeNfcSynth(const SynthConfig& cfg, double sampleRate);

}  // namespace dect2::nfc
