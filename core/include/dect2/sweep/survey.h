// Band survey: many sweeps of one range over hours or days, kept as a compact time x frequency grid (level above the noise floor per bin),
// with an occupancy heatmap, the persistent signals, a "what changed since the last survey" diff and a quiet-frequency finder.
// Source-agnostic: it takes the Sweep structs runSchedule() produces.
#pragma once
#include "dect2/sweep/sweep.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace sweep {

struct SurveyConfig {
    double thresholdDb = 8;      // a bin is occupied when it is this far above the noise floor of its sweep
    size_t maxRows = 4000;       // memory cap: past this, neighbouring rows are merged (the older half, by maximum)
};

struct PersistentSignal {
    double freqHz = 0, bwHz = 0;
    float occupancy = 0;         // fraction of the sweeps in which the signal was above the threshold (0..1)
    float meanDb = 0, maxDb = 0; // level above the floor while present
};

// A signal that comes and goes at regular times (an hourly bulletin, a beacon, a satellite's fixed schedule)
struct RecurringSignal {
    double freqHz = 0;
    double periodSec = 0;        // the median time between its starts
    double nextSec = 0;          // the predicted next start (wall clock, seconds since 1970)
    int starts = 0;              // how many times it started during the survey
    float jitter = 0;            // the spread of the intervals as a share of the period
    float occupancy = 0;
};

struct QuietSlot {
    double startHz = 0, widthHz = 0;
    float occupancy = 0;         // mean occupancy over the slot
    float maxDb = 0;             // strongest level above the floor anywhere in the slot, ever
};

struct SurveyDiff {
    std::vector<PersistentSignal> appeared, vanished;   // persistent in one survey only
    float floorChangeDb = 0;                             // mean noise floor of b minus a
    float occupancyChange = 0;                           // mean occupancy of b minus a
};

class Survey {
public:
    explicit Survey(const SurveyConfig& c = SurveyConfig()) : cfg_(c) {}
    // Adds a sweep. The first one fixes the range and bin width; a later one of a different range is resampled onto the same grid
    // (maximum where it is finer); false when it does not overlap the range at all.
    bool add(const Sweep& s);
    void clear();
    size_t rows() const { return times_.size(); }
    size_t bins() const { return bins_; }
    double startHz() const { return startHz_; }
    double binHz() const { return binHz_; }
    double freqOf(size_t i) const { return startHz_ + (double)i * binHz_; }
    double firstTime() const { return times_.empty() ? 0 : times_.front(); }
    double lastTime() const { return times_.empty() ? 0 : times_.back(); }
    const SurveyConfig& config() const { return cfg_; }
    void setThreshold(double db) { cfg_.thresholdDb = db; }

    // level above the floor (dB) of bin i in row r, 0 when below the floor
    float level(size_t r, size_t i) const { return (float)cells_[r * bins_ + i] * 0.5f; }
    float floorDb(size_t r) const { return floor_[r]; }
    // per-bin statistics over all rows
    std::vector<float> occupancy() const;      // 0..1
    std::vector<float> maxLevel() const;       // dB above the floor
    std::vector<float> meanLevel() const;      // mean dB above the floor over the rows in which the bin was occupied
    float meanFloorDb() const;

    // Runs of bins whose occupancy is at least minOccupancy: always-on carriers, repeaters, spurs. Strongest (most occupied) first.
    std::vector<PersistentSignal> persistent(double minOccupancy = 0.5, int bridgeBins = 1) const;
    // The n emptiest slots of the given width (no overlap): lowest mean occupancy, then lowest strongest-ever level.
    std::vector<QuietSlot> quiet(double widthHz, int n = 5) const;
    // Signals on for a part of the time at a regular interval: at least minStarts starts, intervals within maxJitter of their median (they are found per run of adjacent bins). Most starts first.
    std::vector<RecurringSignal> recurring(int minStarts = 4, double maxJitter = 0.15) const;

    // RGBA picture, time down (oldest at the top), frequency across; width x height pixels
    std::vector<uint8_t> heatmap(int width, int height, double fullScaleDb = 30) const;

    bool save(const std::string& path, std::string& err) const;     // compact binary file, loadable later for a diff
    bool load(const std::string& path, std::string& err);
    bool exportOccupancyCsv(const std::string& path, std::string& err) const;   // freq Hz, occupancy, mean dB, max dB per bin
    bool exportSignalsCsv(const std::string& path, double minOccupancy, std::string& err) const;

private:
    void merge();
    SurveyConfig cfg_;
    double startHz_ = 0, binHz_ = 0;
    size_t bins_ = 0;
    std::vector<double> times_;
    std::vector<float> floor_;
    std::vector<uint8_t> cells_;   // rows x bins, half dB above the floor, 0..255 (127.5 dB)
};

// What changed between an older survey a and a newer b over the same range (b is compared on a's bins)
SurveyDiff compareSurveys(const Survey& a, const Survey& b, double minOccupancy = 0.5);

} // namespace sweep
} // namespace dect2
