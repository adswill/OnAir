// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Autonomous airband scanning (4.5): no channel list and no airport import needed. Two parts, both plain logic without DSP or UI:
//
//  AirbandAuto    decides which carriers the band watch finds (airband_seen.h) become live channels, and which auto channels go again.
//                 A carrier that keys up on the 25 kHz / 8.33 kHz raster is opened at once. A channel that has been silent for a while
//                 goes (at most kAirbandAutoMax at a time, the quietest first). A carrier that stays on for minutes (ATIS, VOLMET) is marked
//                 continuous and left out of the scan's choice. A carrier sitting right next to a much stronger active one is the stronger
//                 one's splatter or a heterodyne and is not opened. Guard (121.500 and 243.000) is always kept open and is the priority.
//                 What was heard is remembered (learned scores) and can be saved and loaded as one line of text.
//  AirbandHopper  when the radio cannot take 118 - 137 MHz at once: tiles the band into captures and says when to move to which. It stays
//                 while a transmission is open, stays longer where much was heard, comes back to busy captures more often and to dead
//                 ones rarely, and checks an idle capture quickly.
#pragma once
#include "dect2/airband/airband_rx.h"
#include "dect2/airband/airband_tel.h"
#include <cstddef>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kAirbandAutoMax = 24;          // auto channels at a time

// What the receiver knows of one auto channel now
struct AirbandAutoView {
    double freqHz = 0;
    bool is833 = false;
    bool active = false;          // squelch open
    double openSinceSec = -1;     // when the open transmission started (signal clock), -1 when not open
    double lastEndSec = -1;       // when it last closed, -1 never
    double madeSec = 0;           // when it was added
    bool pinned = false;          // the user asked to listen: kept until it has been heard and has closed
    float peakExcessDb = 0;
};

// One carrier of the learned list
struct AirbandLearned {
    double freqHz = 0;
    float score = 0;              // grows with every transmission (seconds heard, bounded), fades slowly
    uint32_t opens = 0;
    bool continuous = false;
};

struct AirbandAutoStep {
    std::vector<AirbandChannel> add;     // to open now (label "auto")
    std::vector<double> drop;            // auto channels to remove (frequencies)
};

class AirbandAuto {
public:
    // lo/hi: the part of the band the capture holds (a candidate outside it is not opened)
    AirbandAutoStep step(double nowSec, const std::vector<AirbandSeen>& seen, const std::vector<AirbandAutoView>& cur, double loHz, double hiHz);
    // A transmission of an auto or listed channel ended (feeds the learning)
    void noteTransmission(double freqHz, double durSec);
    bool continuous(double freqHz) const;
    void markContinuous(double freqHz);   // a carrier that has been on for kContinuousSec
    float score(double freqHz) const;
    // Guard channels that must always be open when inside the capture
    static bool isGuard(double freqHz);
    static std::vector<double> guardList();
    // The learned list as one line ("l1:" then freq,score,opens,continuous records split by ';'); load ignores damaged records
    std::string save() const;
    bool load(const std::string& s);
    const std::vector<AirbandLearned>& learned() const { return learn_; }
    void forget() { learn_.clear(); }
    void setEnabled(bool on) { on_ = on; }
    bool enabled() const { return on_; }
    // Seconds a carrier is on before it is called continuous, and the quiet time before an auto channel goes
    static constexpr double kContinuousSec = 90, kKeepSec = 30, kKeepHotSec = 120;
private:
    AirbandLearned* find(double f);
    const AirbandLearned* find(double f) const;
    std::vector<AirbandLearned> learn_;
    bool on_ = true;
};

// ---------------------------------------------------------------- hopping

class AirbandHopper {
public:
    static constexpr double kBandLoHz = 118.0e6, kBandHiHz = 137.0e6;
    // Tiles 118 - 137 MHz into captures that each fit in spanHz. One capture (or none): nothing to hop. Keeps what it learned
    // when the plan stays the same.
    void plan(double spanHz);
    size_t groups() const { return grp_.size(); }
    bool needed() const { return grp_.size() > 1; }
    double centerOf(size_t g) const { return g < grp_.size() ? grp_[g].center : 0; }
    double loOf(size_t g) const { return g < grp_.size() ? grp_[g].lo : 0; }
    double hiOf(size_t g) const { return g < grp_.size() ? grp_[g].hi : 0; }
    size_t current() const { return cur_; }
    // clock: seconds of signal fed so far (never goes back). Called often. anyOpen: a transmission is open in the capture now.
    // centerHz: where the radio is tuned now (the fallback when a retune that was asked for never shows).
    // Returns the capture to move to, or -1 to stay.
    int tick(double clock, bool anyOpen, double centerHz);
    // The radio is tuned to centerHz now (after a retune, or the user's own): the nearest capture is the current one and its dwell starts
    void locate(double centerHz, double clock);
    // A transmission was heard at freqHz (adds to the capture's activity)
    void noteActivity(double freqHz, double clock);
    // Prior knowledge: the learned scores raise the capture's activity before anything was heard this session
    void seed(const std::vector<AirbandLearned>& l);
    double activityOf(size_t g) const { return g < grp_.size() ? grp_[g].act : 0; }
    // The dwell now planned for the current capture, seconds
    double dwellSec() const;
    static constexpr double kMinDwell = 1.4, kMaxDwell = 8, kOpenGrace = 1.0, kOpenMax = 45, kRevisitMax = 75;
private:
    struct G { double lo = 0, hi = 0, center = 0, act = 0, lastVisit = -1e9; int dead = 0; bool heard = false; };
    std::vector<G> grp_;
    size_t cur_ = 0;
    double arrive_ = 0, lastOpen_ = -1e9, spanKey_ = 0;
    bool waiting_ = false;       // retune asked, not yet arrived
    double askedAt_ = 0;
};

} // namespace dect2
