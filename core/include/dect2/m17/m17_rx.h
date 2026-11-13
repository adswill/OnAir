// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 receiver: the open digital voice and data protocol for amateur radio (M17 specification 2.0), 4FSK at 4800 symbols/s in a 9 kHz
// channel.
//
// The channel is mixed to 0 Hz, decimated (CIC, then a windowed-sinc resampler) to 48 kHz, FM-demodulated and passed through the receive
// root-raised-cosine filter (10 samples per symbol). Frames are found by their sync bursts (LSF, stream, packet, BERT; the end-of-
// transmission marker ends a transmission). Each candidate is scaled from its sync symbols and then from its own decided symbols (a least-
// squares fit of level and offset), which takes out the tuning error and a deviation that is off, and is kept only when it checks: the LSF
// by its CRC, a stream frame by the Golay words of its LICH, a packet frame by the convolutional decoder's error count. A mirrored spectrum
// (I and Q swapped) turns every symbol upside down, which makes an LSF sync look like a stream sync: both readings are tried until one
// checks, then the polarity is kept. Once a frame has checked, the next is looked for 40 ms later within +-2 samples (the clock may be off).
//
// A receiver that starts in the middle of a stream rebuilds the LSF (callsigns, TYPE, META) from the LICH of six stream frames; the TYPE
// alone, enough to start the voice, from the third. Voice: Codec 2 3200 or 1600 through the user-installed libcodec2; without it the
// frames and callsigns are still shown. An encrypted stream is reported as encrypted and never decrypted (no sound).
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
namespace voice { class Monitor; }

struct M17Telemetry;   // m17_tel.h

class M17Receiver {
public:
    M17Receiver();
    ~M17Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the channel sits relative to 0 Hz in the input
    bool ready() const;                  // false when the input rate is too low
    void reset();                                        // after a retune (the logs stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(M17Telemetry& out, uint64_t lastSeq);
    // The transmission log, messages and positions stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setCanFilter(int can);          // play only this channel access number (-1: every one)
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);              // decode but do not open the sound device
    void setVoiceMonitor(voice::Monitor* m);   // the monitor that picks and records the call (voice_monitor.h); default voice::monitor("m17") while not silent
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz voice

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning m17Tuning();

} // namespace dect2
