// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone decoders on FM audio or sound-card audio: CTCSS (50 tones), DCS (23 bit Golay word, both polarities), DTMF, five-tone sequential
// calling (ZVEI1 / ZVEI2, CCIR, EEA, EIA, DZVEI), SELCAL, and a generic two-tone sequential paging detector (measured tone pair).
//
// Input: IQ of an FM channel (the APRS / packet FM front end: mix out the signal offset, resample to 96 kHz, AFC, channel filter,
// discriminator) or, with setAudioInput(), real audio samples (the real part of the samples: a sound card or a recording) at any rate of
// 8 kHz and up. Everything after that runs on 8 kHz audio. CTCSS and DCS use a 300 Hz low-pass at 1 kHz; the voice band decoders work
// on Goertzel filters over short windows. A squelch-style gate (setGate) opens the listening audio only while a chosen tone or code is
// present, and takes the sub-audible tones out of it.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/tones/tones_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

// How the radio's channel is demodulated: FM (VHF / UHF), USB / LSB (HF voice: SELCAL on the aeronautical HF channels is sent in USB)
// or AM (VHF airband). SSB and AM: the channel at 8 kHz, 300 - 3000 Hz beside the dial (AM: +-3.4 kHz and the envelope), with an AGC.
enum TonesDemod { TonesFm = 0, TonesUsb, TonesLsb, TonesAm, TonesDemodCount };
const char* tonesDemodName(int d);                        // "FM", "USB", "LSB", "AM"
int tonesAutoDemod(double dialHz);                        // USB below 30 MHz, FM above (the auto choice of the interface)

class TonesReceiver {
public:
    TonesReceiver();
    ~TonesReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setAudioInput(bool on);                          // real audio, not FM IQ (call before or after configure())
    void setSelcalOnly(bool on);                          // only the SELCAL detector runs (the SELCAL mode)
    void setDemod(int d);                                 // TonesDemod for a radio's IQ (default FM); a change restarts the chain
    bool ready() const;
    void reset();                                         // the signal only: the detections stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(TonesTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // The gate: mode 0 always open, 1 any CTCSS tone or DCS code, 2 only the chosen one (kind 0 CTCSS: hz; kind 1 DCS: code, inverted)
    void setGate(int mode, int kind, double ctcssHz, int dcsCode, bool dcsInverted);
    // "Listen": the (gated) audio at 8 kHz on the sound card; off by default
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                               // decode but do not open the sound device
    void setListenTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz audio after the gate (also when silent)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning tonesTuning();

} // namespace dect2
