// Spectrum ghost: a stored earlier spectrum (a snapshot) drawn faintly behind the live trace of the main spectrum, with the signals that are
// new since then highlighted. Snapshots are small files in <dataDir>/ghost (one per capture; a rolling set is kept by the auto-snapshot).
// Pure (std only); the plot side is app/gfx/spec_ghost_ui.cpp. docs/SPECTRUM-GHOST.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sghost {

struct Snap {
    double loMhz = 0, hiMhz = 0;   // the frequency span the bins cover
    int64_t when = 0;              // seconds since the epoch
    std::string label;             // free text ("", "auto", or what the user typed)
    std::vector<float> db;         // dB per bin, low to high frequency (max-pooled to at most kMaxBins)
};
constexpr size_t kMaxBins = 4096;

// A snapshot of a trace (n bins from loMhz to hiMhz); more than kMaxBins bins are max-pooled so signals stay visible.
Snap make(const float* db, size_t n, double loMhz, double hiMhz, int64_t when, const std::string& label);

// The trace as npx columns over the view [vLo, vHi]: each column the highest bin that falls in it (a column between two bins takes the
// nearest); NaN where the trace does not reach.
void columns(const float* db, size_t n, double loMhz, double hiMhz, double vLo, double vHi, int npx, std::vector<float>& out);

struct Span { double loMhz = 0, hiMhz = 0; float riseDb = 0; };
// Where the live trace is more than thrDb above the ghost for at least minCols neighbouring columns (both given by columns() over the same view).
// Columns where either is NaN are skipped. Spans are in MHz of the view.
std::vector<Span> newSignals(const std::vector<float>& live, const std::vector<float>& ghost, double vLo, double vHi, float thrDb, int minCols);

// ---- the store
struct Meta { std::string path; double loMhz = 0, hiMhz = 0; int64_t when = 0; std::string label; };
std::string fileFor(const std::string& dir, int64_t when);          // <dir>/ghost-<when>.oag
bool save(const std::string& dir, const Snap& s);                   // creates the folder; false when it cannot be written
bool load(const std::string& path, Snap& out);
std::vector<Meta> list(const std::string& dir);                     // newest first (header only)
// Keep the newest `keep` snapshots, delete the rest; returns how many were deleted. keep <= 0 keeps all.
int prune(const std::string& dir, int keep);
// the fraction of [lo, hi] that a snapshot covers
double coverage(const Meta& m, double lo, double hi);
// The snapshot nearest to `target` (seconds) among those covering at least minCover of the view; -1 when none.
int pick(const std::vector<Meta>& all, double lo, double hi, int64_t target, double minCover = 0.5);
// a rolling auto-snapshot is due when none was taken for everySec (everySec <= 0: never)
bool autoDue(int64_t lastWhen, int64_t now, int64_t everySec);

}  // namespace sghost
