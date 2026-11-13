// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Orbcomm subscriber downlink receiver (137.2 - 137.8 MHz, orbcomm.h): decodes up to two downlinks in the passband at once.
//
// A spectrum of the input (Hann, about 60 Hz bins, averaged over about a second, smoothed over 5 kHz) finds the downlinks: peaks
// 4 dB over the median floor, narrower than 15 kHz (the APT weather satellites nearby are wider), inside 137.15 - 137.85 MHz when
// the tuned frequency is known. Each downlink gets a decoder: mixed to 0 Hz, filtered and decimated in two steps to about 50 kHz,
// the carrier from the 4th power (SDPSK turns the phase by quarter turns, so x^4 is a line at 4 x the offset), then a fine mixer
// steered by a frequency locked loop on the differential phase (tracks Doppler and its ramp, +/-4 kHz and more), AGC, the RRC
// matched filter, a Gardner symbol clock with interpolation, differential detection (the sign of the phase step is the bit), the
// sync word search in both polarities, 12-byte packets with their Fletcher checksum, 600-byte frames. A decoder without frames for
// some seconds lets its downlink go (and the spectrum is searched again).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "orbcomm_tel.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct SynthConfig;
class ModeSynth;

class OrbcommReceiver {
public:
    OrbcommReceiver();
    ~OrbcommReceiver();
    void configure(double inputRateHz);
    bool ready() const;                    // false when the input rate is too low
    void reset();                                        // after a retune (the satellites and the log stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(OrbcommTelemetry& out, uint64_t lastSeq);
    // The satellite table and the packet log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setCenterHz(double hz);           // the radio's centre (0: unknown, a recording): absolute frequencies and Doppler from it
    void setSignalOffset(double) {}        // the downlinks are found anywhere in the passband
    void setMaxChannels(int n);            // 1 or 2 downlinks at once (default 2)
    void setSilent(bool) {}                // no sound

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

ModeTuning orbcommTuning();
std::unique_ptr<ModeSynth> makeOrbcommSynth(const SynthConfig& cfg, double sampleRate);
// SynthConfig for the test signal: modeOpt[0] downlinks (0 = 2, 1 or 2), modeOpt[1] 1 = Doppler ramp +/-4 kHz (triangle, modeVal[0] s
// per leg, default 30), modeOpt[2] percent of packets with a broken checksum, modeOpt[3] seed; modeVal[1] clock offset ppm.
// snrDb is Es/N0 of each downlink, cfoHz a common offset. The downlinks sit 25 kHz below and above the radio centre.

// The usual downlink channels, where the presets tune
struct OrbcommPreset { const char* name; double mhz; };
const OrbcommPreset* orbcommPresets(int& count);

} // namespace dect2
