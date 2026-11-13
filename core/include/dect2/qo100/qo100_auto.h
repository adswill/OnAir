// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QO-100 narrowband auto decoder and SSB conversation manager (docs/qo100-auto.md).
//
//  - Qo100Tracker follows every signal of the 500 kHz narrowband transponder over time from the averaged spectrum (about 75 Hz bins, a frame every
//    0.1 s): where it is, how wide, how loud, on or off, and from that what it is: beacon, unmodulated carrier, CW (keyed), SSB (wide, syllabic
//    level), an FT8 / JS8 style 15 s slot signal, FT4 (7.5 s slots), PSK31 / RTTY (narrow, continuous) or SSTV (about 1 kHz, steady).
//  - Qo100Conversations groups the SSB tracks by frequency and time into conversations: start, end, level, drift, estimated station count.
//  - Qo100AutoDecoder runs the ticked decoders (the HF digital receiver for FT8, FT4, SSTV and RTTY, the JS8Call, PSK and CW receivers) on every
//    matching signal, one channel per cluster of signals inside 3.4 kHz of audio, all fed by the one wide capture. A CPU meter drops the weakest
//    channel when the limit is passed.
// Pure DSP and bookkeeping, no UI. qo100_rx.cpp owns one of each.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/qo100/qo100_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// the decoders that can read a class (Qo100Dec bits)
int qo100ClassDecoders(int cls);

class Qo100Tracker {
public:
    Qo100Tracker();
    ~Qo100Tracker();
    void reset();
    // One frame of the spectrum: dB per bin, bin k at f0Hz + k * binHz (corrected downlink), the noise floor, receiver seconds (increasing).
    void update(const float* db, size_t n, double f0Hz, double binHz, float floorDb, double tSec);
    const std::vector<Qo100Track>& tracks() const;       // confirmed signals, by frequency
    std::vector<Qo100Track>& mutableTracks();            // the conversation manager sets conv, the decoder manager decMask / idName
    void setDecoderMask(int trackId, int mask);
    // the signal identifier's opinion of a track (from qo100_auto.cpp's async job)
    void setIdentified(int trackId, const std::string& name, int cls);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

class Qo100Conversations {
public:
    Qo100Conversations();
    ~Qo100Conversations();
    void reset();
    // Called after every tracker update: tracks of class SSB are grouped (their conv field is set); tSec receiver seconds, utc wall clock.
    void update(std::vector<Qo100Track>& tracks, double tSec, double utc);
    const std::vector<Qo100Conversation>& list() const;   // newest first (ended ones stay, at most 60)
    Qo100Conversation* find(int id);
    void setGapSec(double s);                              // silence that ends a conversation (default 90 s)
    void setSnippet(int id, const std::string& text);      // speech to text arrived for it (callsigns are looked for in it)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Callsigns in text; with spoken = true NATO phonetic words ("lima yankee two alpha bravo charlie") and digit words are turned into letters first.
std::vector<std::string> qo100CallsFromText(const std::string& text, bool spoken = false);

// The decoders on the tracks. All calls from the analysis thread.
class Qo100AutoDecoder {
public:
    Qo100AutoDecoder();
    ~Qo100AutoDecoder();
    void configure(double rateHz);                     // sample rate of what feed() gets (the narrowband channel, centre = nbCentreRf)
    void reset();
    void setEnabled(unsigned mask);                    // Qo100Dec bits (kQoDecSsb has no decoder here)
    void setMaxSlots(int n);                           // default 6
    void setCpuLimit(float cores);                     // default 0.7
    void setStartTime(double utc);                     // tests: the slot clock of the first sample (NaN: the system clock)
    // Which tracks get which decoder: tracks as the tracker reports them; apparentCentreRf is the downlink frequency (nominal) of the stream's
    // 0 Hz, corrHz the LNB correction (true = apparent + corr). decMask of every track is set.
    void plan(std::vector<Qo100Track>& tracks, double apparentCentreRf, double corrHz, double tSec);
    void feed(const cf32* x, size_t n);
    void fill(Qo100AutoTelemetry& t);                  // slots, decodes, texts, images, cpu
    void clearResults();
    // The signal identifier is asked (on a thread of its own) about signals the tracker cannot place; plan() starts the jobs, this returns a finished one
    bool takeIdentified(int& trackId, std::string& name, int& cls);   // a finished job
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
