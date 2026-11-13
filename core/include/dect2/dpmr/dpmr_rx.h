// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR receiver: ETSI TS 102 490 mode 1 (peer to peer), four-level FSK at 2400 symbols/s in 6.25 kHz, PMR446 and the licensed bands.
//
// The channel is mixed to 0 Hz, decimated (CIC, windowed-sinc) to 48 kHz and FM-demodulated, then symbol filtered. FS1 / FS3 / FS4 are
// searched by correlation on every sample, FS2 too (late entry, kept only when the CCH that follows passes its CRC); polarity from the
// sign, level and carrier offset from a least-squares fit. After a header every payload frame's sync is looked for within a few samples
// of where it should be (follows clock errors), two frames flywheeled; the end frame (FS3) closes the call.
// Decoded: header (called / own id, communication mode, call type, header type: call, status ...), colour code, CCH (frame number,
// ids in halves, slow data), data blocks (short messages, packet data), end frame.
// Voice: AMBE+2 through the user-installed mbelib (nxdn_ambe.h, the same library) when it is there; otherwise counted, no sound.
// Scan: with setScan the receiver steps through the 16 PMR446 channels inside the captured band until it finds frames, then stays.
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

struct DpmrTelemetry;   // dpmr_tel.h

class DpmrReceiver {
public:
    DpmrReceiver();
    ~DpmrReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                                        // after a retune (the logs stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(DpmrTelemetry& out, uint64_t lastSeq);
    // The call log and the decoded headers stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    // tunedMhz: the frequency the radio's channel (the signal offset) sits on; scanning hops to the other PMR446 channels from there
    void setChannel(double tunedMhz);
    void setScan(bool on);

    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setVoiceMonitor(voice::Monitor* m);   // the monitor that picks and records the call (voice_monitor.h); default voice::monitor("dpmr") while not silent
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);
    void setVoiceTap(std::function<void(const uint8_t bits[72])> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dpmrTuning();

} // namespace dect2
