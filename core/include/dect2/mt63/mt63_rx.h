// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 receiver (500 / 1000 / 2000 Hz, short and long interleave, found automatically).
//
// The channel is mixed to 0 Hz and brought to 8 kHz complex (the audio passband of an upper sideband receiver, dial + 100 .. 3900 Hz).
// A 2048-point spectrum finds a flat block of the width of one of the three modes (power in the box against the bands beside it).
// The synchroniser then takes the last 16 symbols through the modem's own front end (the box mixed to 0 Hz and brought to 2 x the
// bandwidth), cuts 512-sample frames with the modem's pulse as window, and looks for the symbol timing and the comb of 64 carriers
// (4 FFT bins apart) for which the phase steps of consecutive symbols are real (+1 or -1: differential BPSK). The frequency error comes
// from the residual rotation (squared steps), the carrier comb position and the interleave are checked by the Walsh-Hadamard decoder:
// three comb positions run side by side until one gives clean codewords. Both interleaves are decoded all the time; the one whose
// codewords are cleaner is shown (unless it is forced). An AFC follows the carriers (tuning tolerance about +-100 Hz).
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

struct Mt63Telemetry;   // mt63_tel.h

class Mt63Receiver {
public:
    Mt63Receiver();
    ~Mt63Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the dial frequency sits relative to 0 Hz in the input
    bool ready() const;
    void reset();                        // after a retune (the text stays: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(Mt63Telemetry& out, uint64_t lastSeq);
    void clearResults();                 // the text
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setBandwidth(int hz);           // 0 automatic, or 500 / 1000 / 2000
    void setInterleave(int i);           // 0 automatic, 1 short, 2 long
    // "Listen" (off by default): the upper sideband, 200 .. 3800 Hz above the dial, as audio on the sound card
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);              // listen without opening the sound device (tests, command line)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning mt63Tuning();

// The usual MT63 dial frequencies (upper sideband)
struct Mt63Preset { const char* name; double mhz; };
const Mt63Preset* mt63Presets(int& count);

} // namespace dect2
