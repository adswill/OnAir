// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA Weather Radio SAME (Specific Area Message Encoding, the EAS alerts of the US and Canada) on the seven NFM weather channels
// 162.400 - 162.550 MHz: receive only. The text of an alert is a public broadcast.
//
// Front end: the APRS / packet FM front end (mix out the signal offset, resample to 96 kHz, AFC, channel filter, discriminator), then 48 kHz audio.
// AFSK at 520.83 baud, mark 2083.3 Hz, space 1562.5 Hz (one bit is exactly 4 and 3 cycles): two tone correlators over one bit, a clock loop that
// resynchronises on every tone change. Bytes are sent least significant bit first after 16 x 0xAB; a burst is found by two aligned preamble bytes
// and read until the tones stop. The message is sent three times: the bursts of one message are voted bit by bit (the lengths must agree), the
// result is parsed and every field checked (same_proto.h). A Goertzel detector on the audio finds the 1050 Hz attention tone.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "same_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class SameReceiver {
public:
    SameReceiver();
    ~SameReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                                        // the signal only: the alerts stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(SameTelemetry& out, uint64_t lastSeq);
    // The alerts stay through reset(), configure() and retunes: only clearResults() empties them
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setCountry(int country);                        // same::Country: how the state part of a location code is named (the alerts already decoded keep their names)
    // "Listen": the demodulated audio (voice, the tones) at 8 kHz on the sound card; off by default
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device
    void setListenTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz audio (also when silent)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sameTuning();

namespace same {

struct Channel { const char* name; double mhz; };
const Channel* channels(int& count);                     // WX1 .. WX7: 162.400, .425, .450, .475, .500, .525, .550 MHz
// Vote the bursts of one message bit by bit (the bursts of the most common length only; at least two). Returns "" when fewer than two agree on a
// length; `fixed` counts the bits changed against some burst and `agree` the bursts equal to the result.
std::string vote(const std::vector<std::string>& bursts, int* fixed = nullptr, int* agree = nullptr);

} // namespace same
} // namespace dect2
