// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT receiver: the MSU-MR pictures Meteor-M N2-3 and N2-4 send on 137.1 / 137.9 MHz while they pass over.
//
// Demodulator: mixed to 0 Hz and decimated to 3 - 5 samples a symbol, a coarse carrier search (the raised-cosine shape fitted to the
// averaged spectrum) while not locked, root raised cosine matched filter (alpha 0.6), Gardner timing with cubic interpolation, a
// Costas loop, soft symbols. QPSK at 72 ksym/s (N2), OQPSK at 72 ksym/s with differential coding (N2-3, N2-4), or OQPSK at 80 ksym/s
// with differential coding, a sync marker every 80 bits and a 36 x 2048 convolutional interleaver (the N2-x 80k mode); automatic tries
// them in turn. The phase ambiguity (four rotations, mirrored or not) is resolved with the encoded sync word, then Viterbi, frames and
// the layers in meteor_proto.h. The picture grows strip by strip in the telemetry.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct MeteorTelemetry;   // meteor_tel.h

class MeteorReceiver {
public:
    MeteorReceiver();
    ~MeteorReceiver();

    void configure(double inputRateHz);   // also starts a new picture
    void setSignalOffset(double hz);
    void setModulation(int m);            // 0 automatic, 1 OQPSK 72k, 2 QPSK 72k, 3 OQPSK 80k interleaved
    int modulation() const;
    bool ready() const;
    void reset();                         // the demodulator only: the picture stays
    void clearImage();                    // start a new picture
    void feed(const cf32* x, size_t n);
    bool telemetry(MeteorTelemetry& out, uint64_t lastSeq);
    // The picture stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning meteorTuning();

struct MeteorPreset { const char* label; double mhz; };
const MeteorPreset* meteorPresets(int& count);   // 137.900 (default), 137.100

} // namespace dect2
