// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VOR and ILS receiver (aircraft navigation beacons, ICAO Annex 10 Volume I, 3.1 and 3.3).
//
// One channel at the dial frequency: mixed to 0 Hz, decimated to about 50 kHz, the carrier found (FFT, +-8 kHz) and moved to 0 Hz,
// filtered +-12.5 kHz and envelope detected. From the envelope:
//  - VOR: the 30 Hz AM (the variable signal of a conventional VOR) and the 30 Hz FM of the 9960 Hz subcarrier (+-480 Hz, the reference)
//    are taken through the same filters and measured against the same 30 Hz phasor, so their delays cancel. The radial is
//    phase(FM 30 Hz) - phase(AM 30 Hz): Annex 10 3.3.5 has the variable signal lag the reference by the magnetic bearing of the receiver
//    from the station. A Doppler VOR swaps the roles in the RF (the reference is the AM, the variable the Doppler FM of the rotating
//    sidebands) and its sidebands turn the other way so that the same measurement gives the same bearing: the receiver needs nothing else
//    (vor_gen.h builds both). 0.5 s blocks (15 cycles, Hann window) every 0.25 s; the radial shown is the circular mean of about 2 s.
//  - ILS: the depths of the 90 Hz and 150 Hz AM, relative to the carrier. DDM = m90 - m150 (localizer: 90 Hz predominates left of the
//    course as seen from an approaching aircraft; glide slope: above the path). Marker beacons (75 MHz): 400, 1300 or 3000 Hz keyed AM.
//  - The 1020 Hz identification (Morse, 7 words per minute or so) decoded to letters; the voice (ATIS) played 300 - 3000 Hz.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "vor_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class VorReceiver {
public:
    VorReceiver();
    ~VorReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);           // where the dial frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);               // the radio's centre frequency (0: unknown, a file); the dial is centre - tuneOffsetHz
    // 0 automatic (by the dial frequency: 329 - 335.4 MHz glide slope, 74.5 - 75.5 marker, 108 - 112 odd tenths localizer, else VOR),
    // 1 VOR, 2 ILS (localizer, glide slope or marker by the dial frequency; localizer when unknown)
    void setFunction(int f);
    bool ready() const;                        // false when the input rate is too low
    void reset();                              // after a retune: forget the measurements
    void feed(const cf32* x, size_t n);
    bool telemetry(VorTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setVolume(float v);                   // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                    // decode but do not open the sound device (tests, command line)
    void setIdentFilter(bool on);              // take the 1020 Hz ident tone out of the voice
    void setAudioTap(std::function<void(const float* x, size_t n, double rate)> cb);   // tests: the voice audio

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning vorTuning();
NavFunction navFunctionFor(int setting, double dialHz);   // what setFunction(setting) measures at this dial frequency

} // namespace dect2
