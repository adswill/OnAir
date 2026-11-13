// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS and AX.25 packet radio on one 25 kHz FM channel (144.800 MHz in Europe): 1200 baud AFSK and 9600 baud G3RUH at the same time.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "packet_tel.h"
#include "packet_ax25.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class PacketReceiver {
public:
    PacketReceiver();
    ~PacketReceiver();

    void configure(double inputRateHz);
    void setWideInput(bool on);                          // the input is one carrier at 192 ksps from the whole-span bank (accepted below the radio's minimum rate)
    void setTableScale(int scale);                       // frame (200) and station (300) lists times 1 (default) .. 20; the oldest leave first
    void setNarrowChannel(bool on);                      // channel filter width: false 25 kHz (default), true 12.5 kHz (rejects a neighbouring channel better; the signal must be centred)
    bool narrowChannel() const;
    // "Fix bits": a frame that fails the FCS is tried again with one bit (1), or also two neighbouring bits (2), flipped; 0 = off (default).
    // More frames get through on a weak signal, at a small risk of a wrong frame passing the check (docs/PACKET.md)
    void setFixBits(int level);
    int fixBits() const;
    // "Vote" (default on, automatic): a frame that no demodulator got right is rebuilt from the copies of all of them (ax25::voteRepair): majority
    // vote bit by bit, then the few positions where they disagree are tried until the check sequence fits. Gains a little at the threshold.
    void setVote(bool on);
    bool vote() const;
    // 1200 baud audio emphasis: 0 auto (default, no filter: the slicers cover a tone balance of +-6 dB; telemetry twistDb tells which it
    // is), 1 de-emphasis (for a transmitter's pre-emphasised microphone audio), 2 pre-emphasis (for a radio's de-emphasised speaker audio)
    void setEmphasis(int mode);
    int emphasis() const;
    // the input is FM-demodulated audio (a sound card on a radio's discriminator or speaker output) instead of radio IQ: the real part of
    // the samples is the audio, at any rate the mode accepts
    void setAudioInput(bool on);
    void setMyCall(const std::string& call);             // the user's callsign: messages to it are marked "to me" (aprs.message event field to_me)
    bool audioInput() const;
    void setSignalOffset(double hz);                     // where the user's frequency sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the frames and stations stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(PacketTelemetry& out, uint64_t lastSeq);
    // The frame list and the station table stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // every frame with a good FCS, as parsed (the Winlink AX.25 link reads its connected-mode frames here); called from the receiver thread
    void setFrameTap(std::function<void(const ax25::Frame& f, int baud)> cb);
    // "Listen": the FM-demodulated audio (the AFSK tones, 1200 / 2200 Hz) at 8 kHz on the sound card; off by default
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

ModeTuning packetTuning();

} // namespace dect2
