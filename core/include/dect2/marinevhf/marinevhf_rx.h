// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF receiver: narrow-band FM voice on the marine channels (156.025 - 162.025 MHz), all channels of the captured span at once.
//
// The carrier finder (dsp/carrierfind.h) looks for carriers on the 25 kHz raster of the marine channels (every marine channel frequency is a
// multiple of 25 kHz, the raster is placed from the radio's centre frequency); each carrier gets a channelizer (mixed to 0 Hz, 8 kHz low-pass,
// 48 kHz) and its own narrow-FM channel receiver (engine/wide_bank.h). A channel receiver is the radio's NFM path: discriminator (+-5 kHz
// deviation = full scale), 300 - 3000 Hz voice band, optional 6 dB/octave de-emphasis (750 us), and a quieting squelch: the discriminator's noise in
// 5.5 kHz and up against the voice band, which is high in noise and low in a real signal (no carrier level is needed, so a weak and a strong
// signal open at the same quality). The audio runs 30 ms late so the first syllable is not cut.
//
// Every channel keeps a call log (start, duration, peak and mean level) and can be recorded: one WAV per transmission,
// <dir>/marinevhf_<date>-<time>_ch<label>_<MHz>MHz.wav. One channel plays at a time: the one the user picked (listen()), or with scan on the
// channel the scan stops on (marinevhf_scan.h: stop on the first that opens, hang, channel 16 priority), or the strongest open channel when
// nothing is picked. The priority watch on 16 takes over from any of them.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/marinevhf/marinevhf_channels.h"
#include "dect2/marinevhf/marinevhf_scan.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct MarineVhfTelemetry;   // marinevhf_tel.h

constexpr double kMarineVhfStepHz = 25000.0;      // the channel raster
constexpr double kMarineVhfAudioRate = 48000.0;

class MarineVhfReceiver {
public:
    MarineVhfReceiver();
    ~MarineVhfReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                  // where the dial frequency sits relative to the middle of the samples
    void setCenterHz(double hz);                      // the radio's centre frequency (0: not known, files)
    void setDialHz(double hz);                        // the dial frequency (places the channels when the centre is not known)
    bool ready() const;                               // false when the input rate is too low for this mode
    void reset();                                     // after a retune: the carriers are new, the call log stays
    void feed(const cf32* x, size_t n);
    bool telemetry(MarineVhfTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void clearResults();                              // the call log and the activity counts

    // ---- the channel plan and what plays
    void setRegion(marinevhf::Region r);
    marinevhf::Region region() const;
    void listen(int number, char suffix = 0);         // play this channel (0: automatic: the scan, else the strongest open channel)
    void setScan(bool on);
    void setScanChannels(const std::vector<std::pair<int, char>>& ch);   // the channels to scan (empty: every voice channel of the region)
    void setScanTiming(double dwellSec, double hangSec);
    void setPriority16(bool on);                      // channel 16 takes over from whatever plays (default on)
    // Recording: one file per transmission in dir (created); minSec: shorter ones are not kept
    void setRecording(bool on, const std::string& dir, double minSec = 0.5);

    // ---- the receiver
    void setSquelch(int percent);                     // 0 (open at the weakest) .. 100 (needs a clean signal); default 50
    void setDeemphasis(bool on);                      // 750 us de-emphasis (default on) or flat audio
    void setCarrierLimit(int n);                      // carriers followed at once (1 .. 24, default 10)
    void setVolume(float v);                          // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                           // decode but do not open the sound device (tests, command line)
    // tests: the 48 kHz audio of each channel after the squelch (the gated stream; silence while closed)
    void setChannelTap(std::function<void(int number, char suffix, const float* x, size_t n)> cb);
    int carrierCount() const;
    std::pair<int, char> playingChannel() const;      // the channel whose audio plays now: number and suffix, (0, 0) none

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning marineVhfTuning();

} // namespace dect2
