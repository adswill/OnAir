// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Optimise gain": sweeps a radio's overall gain over its range, measures how good the reception is at each step and keeps the best step that
// does not overload the ADC. The selection is plain logic (no engine, no clock of its own) so it is tested on a synthetic SNR-vs-gain curve.
//
// The quality it measures, in order of preference:
//   1. the active mode's own SNR / MER / Es/N0 from its telemetry while it is locked (rxSnrDb),
//   2. else the signal-to-noise ratio of the spectrum around the tuned channel (spectrumSnrDb: channel power against the noise beside it); the two
//      are never mixed: if any safe step had the mode's number, only that number decides, else the spectrum's,
//   3. else, with neither, the ADC level closest to the AutoGain target (-16 dBFS rms).
// A step is excluded when its samples clipped or the ADC status was Overload at any moment. Among the remaining steps the best quality wins; steps
// within `snrTolDb` of it are equal, and then the lowest gain wins (more headroom against strong signals, less intermodulation).
#pragma once
#include "gain.h"
#include "dect2/t2/t2rx.h"
#include <string>
#include <vector>

namespace dect2 {

// The SNR (dB) the mode reports for what it decodes, when it is locked; false when the mode is not locked or has no such number.
// `what` (optional) names it: "SNR", "MER" ...
bool rxSnrDb(const RxTelemetry& rx, double& snrDb, const char** what = nullptr);

// SNR of the channel centred centreOffsetHz from the middle of a DC-centred spectrum (dbfs: one value per FFT bin over rateHz) and chanBwHz wide:
// the mean power inside against the noise beside it (the quietest quarter of the bins outside the channel and its skirts). False when the
// spectrum has too few bins outside the channel to tell (a capture hardly wider than the channel).
bool spectrumSnrDb(const std::vector<float>& dbfs, double rateHz, double chanBwHz, double& snrDb, double centreOffsetHz = 0);

class GainOptimiser {
public:
    struct Config {
        double settleSec = 1.0, measureSec = 2.0;   // wait after a change, then average this long
        double clipLimit = 0.001;                   // a step whose clip fraction exceeded this is excluded
        double snrTolDb = 0.5;                      // steps this close to the best SNR count as equal: the lowest gain of them wins
        double minUsableSnrDb = 3;                  // a best quality below this is noise, not a signal: that number is not used, the next one (or the level) is
        int maxPoints = 10;                         // steps of the coarse sweep (the range is divided evenly); a refinement of up to 2 more follows
        double targetDbfs = -16;                    // level fallback target
    };
    struct Sample { bool snrValid = false; double snrDb = 0; bool altValid = false; double altDb = 0; float clip = 0, rms = -120, peak = 0; };   // snr*: the mode's, alt*: the spectrum's
    struct Point {
        int gainDb = 0;
        int n = 0, nAll = 0;       // samples with a quality number / all samples
        double snrDb = -100;       // mean quality over the samples that had one
        int n2 = 0;                // samples with the spectrum's SNR (the second choice) ...
        double altDb = -100;       // ... and their mean
        float rms = -120, clip = 0;
        bool overload = false;     // the ADC status was Overload at some moment
        bool done = false;
        bool hasSnr() const { return n > 0 && n * 2 >= nAll; }   // a point that locked only now and then is not trusted
        bool hasAlt() const { return n2 > 0 && n2 * 2 >= nAll; }
        bool clipped(double lim) const { return overload || clip > lim; }
    };
    enum class Result { None, Snr, Spectrum, Level, AllClipped };   // how the winner was chosen (AllClipped: the lowest step, nothing was safe)

    GainOptimiser() = default;
    explicit GainOptimiser(const Config& c) : cfg_(c) {}
    // Starts a sweep over [minDb, maxDb] (whole dB); currentDb is remembered for cancel() and undo.
    void start(double nowSec, int minDb, int maxDb, int currentDb);
    bool active() const { return active_; }
    // Feed the newest measurement; returns true when the gain must be changed to `gainDb` (the first step, every next step, the winner at the end).
    bool update(double nowSec, const Sample& s, int& gainDb);
    // Stops the sweep and gives back the gain it started at (always true if it was active)
    bool cancel(int& gainDb);

    const std::vector<Point>& points() const { return pts_; }
    int testing() const { return idx_ >= 0 && idx_ < (int)pts_.size() ? pts_[(size_t)idx_].gainDb : startDb_; }   // the gain being measured now
    int stepsDone() const;
    int stepsTotal() const { return (int)pts_.size(); }
    int startGain() const { return startDb_; }
    int bestGain() const { return bestDb_; }
    Result result() const { return result_; }
    bool refining() const { return refined_; }
    // The winner of a set of measured points (pure; what update() applies at the end). Returns the index into pts, or -1 for an empty set.
    static int choose(const std::vector<Point>& pts, const Config& cfg, Result* how = nullptr);
    // The gains of a coarse sweep over [minDb, maxDb] with at most maxPoints steps (always includes both ends)
    static std::vector<int> coarseGains(int minDb, int maxDb, int maxPoints);

private:
    void beginPoint(double now, int gainDb);
    void closePoint();
    Config cfg_;
    std::vector<Point> pts_;
    int idx_ = -1, minDb_ = 0, maxDb_ = 0, startDb_ = 0, bestDb_ = 0, step_ = 1;
    bool active_ = false, refined_ = false;
    Result result_ = Result::None;
    double t0_ = 0, snrSum_ = 0, altSum_ = 0, rmsSum_ = 0, clipMax_ = 0;
    int nSnr_ = 0, nAlt_ = 0, nAll_ = 0;
    bool over_ = false;
};

// What the app keeps around a sweep: the optimiser and what undo needs (the gain and AGC state from before it)
struct GainOptSession {
    GainOptimiser opt;
    bool undoable = false;       // a finished sweep that changed the gain, not yet undone
    int undoDb = 0;              // the gain before it (the radio's own scale: dB, or the HackRF's total)
    GainSetting undoStages;      // the HackRF's three stages before it (restored exactly)
    double finishedAt = 0;       // when the result appeared (the app's clock), for how long it is shown
    bool undoAgc = false;        // was the AGC on
    bool finished = false;       // there is a result to show
    int resultDb = 0;
    GainOptimiser::Result how = GainOptimiser::Result::None;
};

} // namespace dect2
