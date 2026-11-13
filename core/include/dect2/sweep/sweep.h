// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fast wideband sweep scanner (rtl_power style): hops the tuner across a range, averages FFT power in fixed-width bins, stitches the hops
// into one trace, measures the noise floor robustly (percentile of the bin power, corrected for the bias of averaged noise) and finds
// signals above floor + threshold. Source-agnostic: see SweepSource (the radio adapter is IqSweepSource, tests use a fake one).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/source/source.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace sweep {

struct SweepConfig {
    double startHz = 88e6, stopHz = 108e6;
    double binHz = 10e3;          // wanted bin width; the FFT size is the power of two that gives the closest bin width at the source rate
    double dwellSec = 0.010;      // capture time per hop (after the settle time); sets how many FFTs are averaged
    double settleSec = 0.003;     // discarded after each retune (calibrateSettle() measures it)
    double cropFrac = 0.15;       // fraction of the rate dropped at each edge of a hop (filter roll-off)
    double overlap = 0.5;         // FFT block overlap (Hann window)
    bool dcSkip = true;           // remove the DC spike: subtract the mean and replace the three bins at DC by their neighbours
    double floorPercentile = 50;  // noise floor = this percentile of the bin power of a hop (bias-corrected); raise it on empty bands, lower in crowded ones
    double thresholdDb = 8;       // a bin is a signal when it is this far above the local noise floor
    double minIntervalSec = 0;    // a sweep never starts sooner than this after the previous one (schedule / battery)
    int maxSweeps = 0;            // 0 = until stopped
    int threads = 0;              // 0 = the shared pool
};

// Where the samples come from. retune() returns when the radio is on the new frequency (the settle time is the caller's job).
class SweepSource {
public:
    virtual ~SweepSource() = default;
    virtual bool retune(double centerHz, std::string& err) = 0;
    virtual size_t capture(cf32* out, size_t n) = 0;   // blocks until n samples are there; returns how many came (< n: the source ended)
    virtual double sampleRate() const = 0;
    virtual void discard(double /*seconds*/) {}         // throw away what the radio delivered meanwhile (settle time)
};

struct Detection {
    double freqHz = 0, bwHz = 0;
    float peakDb = 0, snrDb = 0;
};

struct Sweep {
    double startHz = 0, binHz = 0;       // frequency of bin 0 (its centre) and the bin spacing
    double timeSec = 0;                  // wall clock (seconds since 1970) at the start of the sweep
    double elapsedSec = 0;               // how long the sweep took
    double rateHz = 0;                   // sample rate of the source
    int fftSize = 0, averages = 0, hops = 0;
    int binsPerHop = 0;
    std::vector<float> db;               // dBFS per bin (a full-scale tone reads 0 dB on its peak bin)
    std::vector<float> floorDb;          // noise floor per bin (constant over a hop)
    float floorOverallDb = -150;         // median of the hop floors
    std::vector<Detection> signals;
    size_t bins() const { return db.size(); }
    double freqOf(size_t i) const { return startHz + (double)i * binHz; }
};

// One entry of the activity log
struct SignalRecord {
    double freqHz = 0, bwHz = 0;
    float peakDb = -150, snrDb = 0, maxSnrDb = 0;
    double firstSeen = 0, lastSeen = 0;
    int count = 0;
};

class SignalLog {
public:
    void update(const Sweep& s);         // merges the sweep's detections into the log
    const std::vector<SignalRecord>& records() const { return rec_; }
    void clear() { rec_.clear(); }
    void set(std::vector<SignalRecord> r) { rec_ = std::move(r); }
private:
    std::vector<SignalRecord> rec_;
};

// ---- the sweep itself ----
// Frequencies the hops are tuned to for this config at this rate (also gives the FFT size and bins per hop)
struct HopPlan {
    int fftSize = 0, binsPerHop = 0, averages = 0;
    double binHz = 0, rateHz = 0;
    size_t samplesPerHop = 0;
    size_t totalBins = 0;
    std::vector<double> centers;
};
HopPlan planHops(const SweepConfig& c, double sourceRateHz);

// One complete sweep. false: cancelled or the source failed (err)
bool runSweep(SweepSource& src, const SweepConfig& c, Sweep& out, std::string& err, const std::atomic<bool>* cancel = nullptr);
// Repeats sweeps (honouring minIntervalSec / maxSweeps) and hands every one to onSweep; stops when cancel is set or onSweep returns false
bool runSchedule(SweepSource& src, const SweepConfig& c, const std::function<bool(const Sweep&)>& onSweep, std::string& err,
                 const std::atomic<bool>* cancel = nullptr);

// Retune latency of this source: tunes between far apart frequencies and watches when the power settles. Returns seconds (with a margin)
double calibrateSettle(SweepSource& src, double loHz, double hiHz, std::string& err);

// The noise floor (dB, mean noise power per bin) from the powers (dB) of bins that are mostly noise: percentile of the linear power,
// corrected for the bias of an average of `averages` exponentially distributed values.
float noiseFloorDb(const float* db, size_t n, double percentile, int averages);

// Signals in a sweep: runs of bins above floor + thresholdDb (gaps of one bin are bridged)
std::vector<Detection> findSignals(const Sweep& s, double thresholdDb);

// ---- files ----
// rtl_power compatible CSV: "date, time, Hz low, Hz high, Hz step, samples, dB, dB, ..." one row per hop
bool writeCsv(const std::string& path, const std::vector<Sweep>& sweeps, std::string& err);
bool readCsv(const std::string& path, std::vector<Sweep>& out, std::string& err);   // also reads rtl_power / rtl_power_fftw files
bool writeSignalsCsv(const std::string& path, const std::vector<SignalRecord>& r, std::string& err);

// Difference of two sweeps of the same range (b resampled to a's bins): per-bin dB difference and the change of the noise floor
struct SweepDiff {
    std::vector<float> deltaDb;          // b - a per bin of a
    float floorChangeDb = 0;             // median over bins of the floor difference (b - a)
    float meanChangeDb = 0;              // mean of the dB difference over all bins
    float maxRiseDb = 0; double maxRiseHz = 0;
    float maxFallDb = 0; double maxFallHz = 0;
};
SweepDiff compareSweeps(const Sweep& a, const Sweep& b);

// ---- the radio adapter ----
class IqSweepSource : public SweepSource {
public:
    IqSweepSource(const DeviceInfo& dev, const TuneSettings& base);
    ~IqSweepSource() override;
    bool open(std::string& err);          // starts the radio at base.centerHz
    void close();
    bool retune(double centerHz, std::string& err) override;
    size_t capture(cf32* out, size_t n) override;
    double sampleRate() const override;
    void discard(double seconds) override;
private:
    DeviceInfo dev_;
    TuneSettings base_;
    std::unique_ptr<IqSource> src_;
    IqRing ring_{1u << 20};
    bool open_ = false;
};

} // namespace sweep
} // namespace dect2
