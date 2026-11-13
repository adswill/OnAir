// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE (2G, MIL-STD-188-141 appendix A / FED-STD-1045) receiver: the calls, soundings, AMD messages and LQA reports on one channel.
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front), then the upper sideband becomes 8 kHz audio
// (hfdig::UsbAudio). Every 2 audio samples (1/32 of a symbol) the eight tones are measured over the last 8 ms by direct DFTs (at
// the tone frequencies plus the tracked offset) and the strongest taken. A word is tried at every one of those instants from the 49
// tones that end there: majority vote, deinterleaving, Golay. Out of sync, a word with few vote and Golay errors, the best of the
// following symbol time, sets the word clock; in sync the next word is looked for one Tw (392 ms) later within +-4 instants, and two
// missed words end the transmission. The words of a transmission go to ale::Assembler; the call that comes out is put in the log,
// matched against the calls before it (a response to a call, the acknowledgement that links them, the responders of a net call) and
// its caller added to the stations. Each word also gives the tone offset (the DFT power around its tones), which the AFC follows.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/audio/hf_demod.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AleTelemetry;   // ale_tel.h

class AleReceiver {
public:
    AleReceiver();
    ~AleReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setDialHz(double hz);           // the dial frequency, logged with each call (0: unknown, a file)
    bool ready() const;
    void reset();                        // after a retune: keeps the log and the stations
    void feed(const cf32* x, size_t n);
    bool telemetry(AleTelemetry& out, uint64_t lastSeq);
    // The word log, events and stations stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setStartTime(double utc);       // the clock of the first sample (< 0: the system clock, the default)
    void setNetAddresses(const std::vector<std::string>& nets);   // addresses known to be nets: calls to them are net calls at once
    void clearLog();
    void flush();                        // tests: end the transmission in progress (the end of a recording)
    // "Listen" (off by default): the upper sideband, 200 .. 3800 Hz above the dial, on the sound card; volume and mute are the app's
    // shared sound controls. The decoding does not change with it.
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setListenTap(std::function<void(const float* x, size_t n)> cb);
    void setDemod(int d);                // the front end's demodulator (HfDemod, hf_demod.h): USB (default), LSB, NFM, AM; Auto counts as USB
    int demod() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning aleTuning();

struct AlePreset { const char* name; double mhz; };
const AlePreset* alePresets(int& count);   // the common ALE channels (upper sideband dial)

} // namespace dect2
