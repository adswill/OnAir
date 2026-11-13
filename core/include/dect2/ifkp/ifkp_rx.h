// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IFKP receiver (engine standard 118, active 117): 0.5, 1.0 and 2.0 speed, found automatically unless forced.
//
// The channel is mixed to 0 Hz and brought to 8 kHz complex (the audio passband of an upper sideband receiver, dial + 100 .. 3900 Hz); the
// modem (kbd33.h) finds the band of 33 tones, the speed and the symbol timing, and follows the tuning. Text, chat log and (FSQ) the sender /
// addressee / command of each line are in the telemetry.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/ifkp/ifkp_tel.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class IfkpReceiver {
public:
    IfkpReceiver();
    ~IfkpReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the dial frequency sits relative to 0 Hz in the input
    bool ready() const;
    void reset();                        // after a retune (the text stays: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(IfkpTelemetry& out, uint64_t lastSeq);
    void clearResults();                 // the text
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setSpeed(int speed);            // -1 automatic, else the index of the speed
    void setMyCall(const std::string& call);   // messages to this call (and to everybody) are marked
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

ModeTuning ifkpTuning();

// Usual dial frequencies (upper sideband): where the stations of the mode tend to be, not an official band plan
struct IfkpPreset { const char* name; double mhz; };
const IfkpPreset* ifkpPresets(int& count);

} // namespace dect2
