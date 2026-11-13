// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APCO Project 25 Phase 1 receiver (TIA-102): C4FM, 4800 symbols/s, 12.5 kHz. Receive only.
//
// The channel is mixed to 0 Hz, decimated to 48 kHz and FM-demodulated; a half-symbol filter gives the levels. The 24-symbol frame sync
// is found by correlation on every sample (its sign gives the polarity, its fit the deviation and the carrier offset), then the NID
// (NAC and DUID, BCH(63,16)) says what follows; the status symbols are taken out. Decoded: HDU (Golay + RS(36,20): algorithm, key,
// talkgroup), LDU1 link control (Hamming + RS(24,12): talkgroup, unit IDs, emergency, encrypted), LDU2 encryption sync (RS(24,16)),
// TDU / TDULC, TSBKs (rate 1/2 trellis, CRC: network, RFSS and adjacent site status, identifier updates, voice grants) and PDU headers.
// Encrypted calls are reported as encrypted with their algorithm and key ID; nothing is decrypted.
// Voice: IMBE through the user-installed mbelib when it is there (p25_imbe.h); otherwise the voice frames are counted, no sound.
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

struct P25Telemetry;   // p25_tel.h

class P25Receiver {
public:
    P25Receiver();
    ~P25Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                                        // after a retune (the logs and the site stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(P25Telemetry& out, uint64_t lastSeq);
    // The call log, talkgroups, control messages and the site with its neighbours stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);              // decode but never open the sound device
    void setVoiceMonitor(voice::Monitor* m);   // the monitor that picks and records the call (voice_monitor.h); default voice::monitor("p25") while not silent
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz voice (mbelib only)
    void setVoiceTap(std::function<void(const uint8_t bits[144], bool clear)> cb);   // tests: every IMBE frame (c0..c7) as received

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning p25Tuning();

} // namespace dect2
