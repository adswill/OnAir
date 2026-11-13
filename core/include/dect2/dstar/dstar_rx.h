// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR receiver: amateur digital voice (JARL D-STAR, DV mode), GMSK at 4800 bit/s (BT 0.5, +-1.2 kHz) in a 6.25 kHz channel, 2 m and
// 70 cm.
//
// The channel is mixed to 0 Hz, decimated (CIC, then a windowed-sinc resampler) to 48 kHz, FM-demodulated and averaged over one bit
// (10 samples). A call starts with the bit sync (1010...) and the 15-bit frame sync, found by correlation on every sample; the sign of the
// correlation gives the polarity (a mirrored spectrum or an inverting radio), the bit sync gives the tuning error (its mean) and the level.
// The 660 header bits are descrambled, deinterleaved and Viterbi decoded; the CRC checks the flags and the RPT2 / RPT1 / UR / MY
// callsigns. Then 96-bit voice frames follow (72 AMBE bits, 24 slow data bits), the bit clock kept by a zero-crossing (Gardner) tracker
// and the offset by decisions. Every 21st frame carries the data sync; two missed in a row, or the end pattern, end the call.
//
// A receiver that starts in the middle of a call finds it by two data syncs 420 ms apart, and takes the header from the slow data resend.
// Slow data: the 20-character message, GPS (NMEA and D-PRS) as positions on the map, the header resend and the code squelch.
// Voice: AMBE through the user-installed mbelib when it is there (dstar_ambe.h); otherwise the frames are counted, no sound.
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

struct DstarTelemetry;   // dstar_tel.h

class DstarReceiver {
public:
    DstarReceiver();
    ~DstarReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                                        // after a retune (the logs stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(DstarTelemetry& out, uint64_t lastSeq);
    // The call log, messages and positions stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);              // decode but never open the sound device
    void setVoiceMonitor(voice::Monitor* m);   // the monitor that picks and records the call (voice_monitor.h); default voice::monitor("dstar") while not silent
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz voice (mbelib only)
    void setVoiceTap(std::function<void(const uint8_t bits[72])> cb);      // tests: every voice frame's 72 bits as received

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dstarTuning();

} // namespace dect2
