// Weather satellite pass report card (SatDump dataset logs, SatNOGS observations): per pass the maximum elevation, SNR, % of frames that
// decoded, lines and gaps, compared with earlier passes at a similar elevation. Kept as CSV. docs/PASS-REPORT-3.9.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct PassReport {
    int64_t startUtc = 0, endUtc = 0;
    std::string satellite;            // "METEOR-M2 4", "" when not known
    double maxElDeg = -1;             // -1 not known (no orbit data or no receiver position)
    float snrMeanDb = 0, snrMaxDb = 0;
    uint64_t framesOk = 0, framesBad = 0, packetsLost = 0;
    int lines = 0, gapLines = 0;      // lines of the picture; of them, lines no channel received
    double syncPct() const { const double n = (double)(framesOk + framesBad); return n > 0 ? 100.0 * framesOk / n : 0; }
};

// what earlier passes at a similar elevation (within elWindow degrees) did, and how this one compares
struct PassCompare {
    int similar = 0;                  // how many
    double meanLines = 0, meanSync = 0, meanSnr = 0;
    std::string verdict;              // "better than usual", "as usual", "worse than usual", "" when nothing to compare with
};
PassCompare passCompare(const std::vector<PassReport>& history, const PassReport& p, double elWindow = 10);

// CSV: start (ISO), end, satellite, max_el, snr_mean, snr_max, frames_ok, frames_bad, sync_pct, packets_lost, lines, gap_lines
std::string passCsv(const std::vector<PassReport>& v, bool header = true);
std::vector<PassReport> passFromCsv(const std::string& text);

// the receiver's running numbers turned into a report: feed telemetry while a pass is received, finish() when it ends
class PassTracker {
public:
    void update(int64_t utc, int state, float snrDb, uint64_t framesOk, uint64_t framesBad, uint64_t packetsLost, int lines, int gapLines);
    bool active() const { return active_; }
    // the pass ends after `quietSec` without frames (or when called by the receiver stopping); returns true with the report when there was one
    bool finish(int64_t utc, PassReport& out, int quietSec = 60, bool force = false);
private:
    bool active_ = false;
    PassReport cur_;
    uint64_t ok0_ = 0, bad0_ = 0, lost0_ = 0;
    double snrSum_ = 0; int snrN_ = 0;
    int64_t lastFrame_ = 0;
};

}  // namespace dect2
