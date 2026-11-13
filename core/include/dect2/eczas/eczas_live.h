// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// e-Czas (Poland's national time service, 225 kHz long wave) receiver for the engine: IQ in, time packets out.
// Spec: GUM "e-CzasPL" frame description v1.0; the bit slicer and frame layout are in eczas_rx.h / eczas_proto.h (after sp5wwp/e-Czas, GPL-3.0).
//
// Signal path (all in the engine's analysis thread, nothing heavy in the interface): the samples are mixed so the carrier sits at 0 Hz and averaged
// down to 8 kHz; a narrow low-pass keeps +-150 Hz; the carrier phase is differentiated (the 50 bit/s phase keying is +-36 degrees ramps of 19 ms), the
// local mean of 100 ms is taken off the signal delayed by 50 ms (the same edge detector as the offline chain eczasAudioToDisc), low-passed and cut to
// 500 Hz for the sync search and bit slicer. A slow frequency loop keeps the carrier centred (sample clock and radio errors of tens of Hz).
//
// Input is IQ at any rate of 16 kHz and up, or (setAudioInput) a real BFO recording of the station: 8 kHz, 48 kHz ... with the carrier at toneHz
// (1 kHz when the radio's BFO was 224 kHz).
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

struct EczasTelemetry;   // eczas_tel.h

class EczasLiveReceiver {
public:
    EczasLiveReceiver();
    ~EczasLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // where the carrier sits in the samples (the radio is tuned tuneOffsetHz above 225 kHz: -tuneOffsetHz)
    // a recording of the demodulated audio (real samples; the I channel of a WAV): the carrier is the tone at toneHz (default 1 kHz)
    void setAudioInput(bool on, double toneHz = 1000);
    // 0: no clock to compare (a file); 1: the computer clock as the samples arrive (a radio, the test signal); 2: sample 0 was taken at UTC epoch originSec (tests)
    void setClockMode(int mode, double originSec = 0);
    void setReceiverLocation(double lat, double lon, bool userSet);
    bool ready() const;
    void reset();                               // the demodulator; the frame log stays
    void clearResults();                        // empties the frame log and the offset history
    void feed(const cf32* x, size_t n);
    bool telemetry(EczasTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning eczasTuning();

} // namespace dect2
