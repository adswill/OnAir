// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR receiver: DMR (Digital Mobile Radio) two-slot TDMA, 12.5 kHz channel, 4FSK at 4800 symbols/s (ETSI TS 102 361-1).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
//
//   input (centred on the channel) -> decimation to 48 kHz -> DC removal -> carrier offset correction -> channel filter -> FM discriminator
//   -> root raised cosine matched filter -> frame sync search (24 symbol patterns) -> time slot tracking -> burst decoding -> calls, messages
//
// Voice: voice bursts are decoded with the AMBE+2 vocoder (dmr_ambe.h, mbelib) and played; they are also handed on as raw bits (setVoiceCallback); the sound
// controls act on that sound.
#pragma once
#include "dmr_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
namespace voice { class Monitor; }

class DmrReceiver {
public:
    DmrReceiver();
    ~DmrReceiver();

    void configure(double inputRateHz);
    // A channel already cut out of a wide capture at 0 Hz (single-radio trunking, trunkpool/capture_voice.h): any rate from 48 kHz, without the radio minimum of configure()
    void configureChannel(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal, drop queued output (the call log stays: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(DmrTelemetry& out, uint64_t lastSeq);
    // The call log and the messages stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // Sound (volume, mute) as in FmReceiver; the app pushes its shared controls here. The AMBE+2 voice of one time slot is played (mbelib, as in NXDN).
    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setListenSlot(int slot);                        // 0 the first slot heard (default), 1 or 2: only that time slot is decoded and played
    uint64_t encryptedBurstsSkipped() const;             // voice bursts of calls marked encrypted that were not decoded
    void setSilent(bool s);                             // decode but do not open the sound device (tests, command line)
    // The voice monitor that picks the call to play and records it (voice_monitor.h). Default: voice::monitor("dmr") while not silent, none while silent.
    void setVoiceMonitor(voice::Monitor* m);
    void setAudioTap(std::function<void(const float* left, const float* right, size_t n)> cb);   // tests: the 48 kHz sound, called from feed() (8 kHz AMBE+2 speech interpolated to 48 kHz; left == right)
    // The voice bursts as they arrive: slot (1 or 2), position in the superframe (0 = A ... 5 = F), the 216 vocoder bits VS(215)..VS(0) packed into 27 octets.
    void setVoiceCallback(std::function<void(int slot, int pos, const uint8_t* bits27)> cb);
    // Burst timing (dmr_slot_sync.h), called from feed(): the input time (seconds from the first input sample, the receiver's filter delay
    // removed) of a burst centre, its family (0 base station, 1 mobile, 2 direct), slot (1 / 2 from a clean CACH, else 0) and CACH access type
    // (1 busy, 0 idle, -1 none). Every base-station burst also goes to DmrSlotClock::global() on the steady clock, for the transmitter.
    void setBurstTimingCallback(std::function<void(double inputSec, int family, int slot, int at)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dmrTuning();

} // namespace dect2
