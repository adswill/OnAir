// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FY-3 AHRPT and MPT receivers (Fengyun-3 polar orbit weather satellites, FY-3A .. FY-3H).
//
// FY-3 AHRPT: QPSK, 1704.5 MHz at 2.8 Msym/s (FY-3A / B), 1701.4 MHz at 2.6 Msym/s (FY-3C); FY-3D onwards use X-band (7.8 GHz, 30 - 38.4 Msym/s, FY-3G
// 6 Msym/s: a down-converter and a very fast radio). FY-3 MPT (the medium rate downlink of FY-3A / B / C): X-band 7750 / 7780 MHz, 18.7 Msym/s.
// The L-band downlink needs a dish (a metre or more, tracking) with an LNA at the feed and a radio of 4 Msps or more (6 - 10 Msps better); the X-band
// ones a 1.2 m or larger dish, a down-converter and 30 Msps and more (so far only offered here as the same chain; see docs/fy3.md).
//
// Chain: DC block, blind IQ correction, mixed to 0 Hz, decimated, symbol rate measured when it is not set (the line at the symbol rate in the spectrum of
// the signal's power), carrier found from the spectrum, root raised cosine, Gardner timing, a Costas loop with narrowing bandwidth, soft symbols scaled
// by a slow level estimate (so a fade lowers the confidence); two independent K = 7 Viterbi decoders, the pair alignment and polarity found from the
// path metrics; the differential decoder; sync word; Reed-Solomon; virtual channels, instruments, pictures. Written to the engine's contract:
// configure() once the sample rate is known, feed() from the analysis thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/fy3/fy3_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class Fy3Receiver {
public:
    explicit Fy3Receiver(int product);   // 0 AHRPT, 1 MPT
    ~Fy3Receiver();

    void configure(double inputRateHz);   // the pictures stay
    void setSignalOffset(double hz);
    void setPreset(int idx);              // 0 automatic, else 1 + the index in fy3Presets(): its symbol rate and roll-off
    int preset() const;
    void setSymbolRate(double hz);        // 0: the preset's / measured; else this rate (no clamps; the roll-off is the preset's or 0.5 / 0.35)
    void setRolloff(double a);            // 0: automatic
    bool ready() const;
    void reset();                         // the demodulator only: the pictures stay
    void clearImage();                    // start new pictures
    void feed(const cf32* x, size_t n);
    bool telemetry(Fy3Telemetry& out, uint64_t lastSeq);
    uint64_t idleInputSamples() const;   // input samples let go by unprocessed while there was no signal (the no-signal pre-check; tests)
    void clearResults();
    std::string saveResults() const { return {}; }        // pictures are saved as PNG from the screen, not kept in the results file
    bool loadResults(const std::string&) { return false; }
    void setLogCallback(std::function<void(const std::string&)> cb);
    int product() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// One class a product, for the places that make a receiver with a default constructor (the sensitivity harness, the tests)
struct Fy3AhrptReceiver : Fy3Receiver { Fy3AhrptReceiver() : Fy3Receiver(0) {} };
struct Fy3MptReceiver : Fy3Receiver { Fy3MptReceiver() : Fy3Receiver(1) {} };

ModeTuning fy3AhrptTuning();   // engine standard 237 (family 235)
ModeTuning fy3MptTuning();     // engine standard 238 (family 236)

struct Fy3Preset { const char* label; double mhz; double symbolRate; double rolloff; int product; const char* sats; };
const Fy3Preset* fy3Presets(int product, int& count);
// Symbol rates the automatic search may pick (those the sample rate fits), for the product
int fy3Candidates(int product, double sampleRate, double* rs, double* alpha, int max);
// The lowest sample rate that holds a symbol rate with a roll-off (the signal's width with a little room)
double fy3MinSampleRate(double symbolRate, double rolloff);

} // namespace dect2
