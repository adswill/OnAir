// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS interference and spoofing monitor: receive-only, in the spirit of the interference panels of u-blox receivers.
// It looks at the raw input of the GNSS receiver (before the excision of narrowband lines) and at the receiver's own solution:
//  - the noise floor (median of the averaged spectrum) and the total power in the band, a second at a time, against a baseline learned at the start;
//  - narrowband lines in the averaged spectrum (frequency, level above the floor);
//  - a swept-tone (chirp) detector: the peak of short spectra moving steadily from one to the next, or short bursts whose frequency runs one way;
//  - a pulse detector: short windows far above the band's median power;
//  - the spectral flatness, which tells a flat (wideband) rise from a few strong lines;
//  - the mean C/N0 of the tracked satellites against a clean-sky reference;
//  - spoofing indicators: equal C/N0 on every satellite, position or clock jumps, a fix far from the long-term average, ephemeris against almanac.
// Status: clear, raised noise, interference, with the reason. Events go to a log with timestamps; gnssInterfCsv() exports the history and the log.
#pragma once
#include "gnss_tel.h"
#include "dect2/common/ring.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

class GnssInterfMonitor {
public:
    GnssInterfMonitor();
    void configure(double rateHz);
    void reset();
    // raw input samples (DC not removed: dcI, dcQ is the receiver's estimate of it); a block at a time, from the receiver's thread
    void process(const cf32* x, size_t n, double dcI, double dcQ);
    // the receiver's latest report (channels and fix): C/N0 and the spoofing indicators. almMismatchKm: the largest distance between the
    // ephemeris and the almanac position of a satellite with both (-1 when none has both), almPrn: that satellite
    void update(const GnssTelemetry& t, double almMismatchKm = -1, int almPrn = 0);
    const GnssInterference& report() const { return r_; }
    void setLogCallback(std::function<void(const std::string&)> cb) { log_ = std::move(cb); }
    // tests: a fixed clock instead of the computer's (seconds since 1970)
    void setClock(std::function<double()> now) { clock_ = std::move(now); }

    static constexpr int kLongN = 1024;      // averaged spectrum
    static constexpr int kShortN = 64;       // chirp detector
    static constexpr int kWin = 16;          // pulse detector window
    static constexpr double kLearnSecs = 5;  // the baseline of the noise floor
private:
    void endSecond();
    void event(int level, const std::string& text);
    void pushHistory(const GnssInterfPoint& p);
    double now() const;

    double rate_ = 0;
    std::function<void(const std::string&)> log_;
    std::function<double()> clock_;
    GnssInterference r_;
    // spectrum accumulation
    std::vector<float> winL_, winS_, re_, im_;
    std::vector<double> acc_;
    int accFrames_ = 0;
    double sumPow_ = 0;
    int64_t sumN_ = 0;
    int64_t secSamples_ = 0, perSec_ = 0;
    double sigT_ = 0;
    // chirp: short spectra
    int lastPeak_ = -1, lastDelta_ = 0;
    bool lastStrong_ = false;
    int strongPairs_ = 0, sweptPairs_ = 0;
    std::vector<int> deltas_;
    // pulses
    std::vector<float> wpow_;
    bool inPulse_ = false;
    int pulseWins_ = 0, pulses_ = 0, hotWins_ = 0, totalWins_ = 0;
    double pulseLevel_ = 0;
    int pulseSweptUp_ = 0, pulseSweptDown_ = 0, pulseMulti_ = 0;
    cf32 pulseFirst_{0, 0}, pulseLast_{0, 0};
    // baseline
    std::vector<float> learnFloors_;
    std::vector<double> knownSpikes_;
    double baseline_ = 0;
    int clearSecs_ = 0;
    int prevStatus_ = 0;
    std::string prevReason_;
    unsigned prevKinds_ = 0;
    // C/N0
    double cn0Sum_ = 0;
    int cn0Count_ = 0, cn0Tracked_ = 0;
    std::vector<float> cn0Recent_;
    bool cn0RefSet_ = false;
    // spoofing
    uint32_t lastFixCount_ = 0;
    bool havePrevFix_ = false;
    double prevEcef_[3] = {0, 0, 0}, prevBias_ = 0, prevFixT_ = 0, prevDrift_ = 0;
    double sumEcef_[3] = {0, 0, 0};
    int nAvg_ = 0;
    double posJumpUntil_ = -1, clockJumpUntil_ = -1;
    bool prevSpoof_ = false;
    std::string prevSpoofReason_;
    uint8_t spoofSec_ = 0;
    double farSince_ = -1;
    // history
    GnssInterfPoint pend_;
    int pendN_ = 0;
};

// the history and the event log as CSV (a header row each; the history first, then a blank line and the events)
std::string gnssInterfCsv(const GnssInterference& r);

} // namespace dect2
