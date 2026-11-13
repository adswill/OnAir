// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// JS8Call receiver: decodes every JS8 signal in the audio passband of an upper sideband receiver (dial + 200 Hz .. dial + 3.2 kHz),
// normal (15 s slots), fast (10 s) and turbo (6 s) at once, and puts the frames together into messages.
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front), then to 4 kHz complex centred on dial + 1750 Hz. The
// slots come from the system clock (UTC), as JS8Call's, unless a start time is given (a recording, the tests). At the end of each slot
// (and a second more for late starts) its samples go to a thread of its own: a spectrogram of symbol-long windows a quarter symbol apart,
// the Costas sync searched over the whole band and -1.5 .. +2.5 s of time offset (fast and turbo less), the best candidates refined in
// time and frequency by direct DFTs, soft decisions of the 3 bits of each 8-FSK symbol, belief propagation on the (174,87) code, CRC-12;
// a decoded signal is subtracted and the slot searched once more. js8_proto.h has the message layer and the sources.
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

struct Js8Telemetry;   // js8_tel.h

class Js8Receiver {
public:
    Js8Receiver();
    ~Js8Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune: keeps the decodes, messages and stations
    void feed(const cf32* x, size_t n);
    bool telemetry(Js8Telemetry& out, uint64_t lastSeq);
    // The decodes, messages and stations stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setEnabled(int submode, bool on);   // normal, fast, turbo (all on by default)
    void setStartTime(double utc);       // the slot clock of the first sample (< 0: the system clock, the default)
    void clearDecodes();
    void waitIdle();                     // tests: until every slot handed to the decoder is done
    void flush();                        // tests: decode the slot in progress now (the end of a recording)
    // "Listen" (off by default): the upper sideband, 200 .. 3800 Hz above the dial, as audio on the sound card; volume and mute are the
    // app's shared sound controls. The decoding does not change with it.
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);              // listen without opening the sound device (tests, command line)
    void setListenTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz audio Listen plays, called from feed()

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning js8Tuning();

struct Js8Preset { const char* name; double mhz; };
const Js8Preset* js8Presets(int& count);   // the JS8Call dial frequencies (upper sideband)

} // namespace dect2
