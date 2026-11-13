// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF propagation dashboard model: what the user's own receiver has heard, per amateur HF band, over the last 24 hours. Observations come from
// the beacon monitor (NCDXF/IARU, Long Dashes, single letters, carriers / time signals) and from the WSPR / FT8 / FT4 / FT2 decodes the HF
// digital receiver already makes (a spot is a station heard, with its distance from the receiver when both locators are known). Nothing is
// downloaded: no solar flux or K index (a public-data feed would need the owner's approval and is not part of this model).
// Per band the model keeps 96 bins of 15 minutes; "open now" is judged over the last 30 minutes from the number of distinct stations,
// the best SNR and the farthest distance. Pure code, no threads, no clock of its own (the caller passes UTC seconds).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dect2::hfp {

constexpr int kBands = 11;                  // 160 m ... 6 m (index 0 = 160 m)
constexpr int kBins = 96;                   // 24 h in 15 minute bins
constexpr double kBinSec = 900;
const char* bandName(int band);             // "160 m" ...
// The band a frequency (Hz) is in (the amateur band plus the broadcast / beacon margins used here), -1 outside every band
int bandOf(double hz);

enum Source { Ncdxf = 0, Letter, Dashes, TimeSignal, Wspr, Ft8, Ft4, Ft2, kSources };
const char* sourceName(int s);

struct Obs {
    double utc = 0;
    double hz = 0;                          // radio frequency
    int source = Ncdxf;
    std::string id;                         // the station: beacon name or callsign (distinct ids are counted)
    float snrDb = 0;                        // each source's own scale; compared per source class only through "heard"
    double distKm = -1;                     // < 0 unknown
};

struct Bin {
    int count = 0;                          // observations
    int stations = 0;                       // distinct stations in the bin
    float bestSnr = -200;
    double maxKm = 0;
    double stamp = -1;                      // bin start (UTC s) this slot holds: a stale slot is ignored
};

enum State { Closed = 0, Marginal = 1, Open = 2 };

struct BandNow {
    int band = 0;
    int stations = 0;                       // distinct in the last 30 minutes
    int obs = 0;
    float bestSnr = -200;
    double maxKm = 0;
    double score = 0;                       // 0 .. 1
    int state = Closed;
    int bySource[kSources] = {};
};

class Model {
public:
    void add(const Obs& o);
    // The state of every band at `nowUtc`
    std::vector<BandNow> now(double nowUtc) const;
    // 96 bins of a band, oldest first, the last one holds `nowUtc`; stale slots come back empty
    std::vector<Bin> history(int band, double nowUtc) const;
    // The band to try now: highest score among bands that are at least Marginal; -1 when nothing is heard. A short reason goes to *why.
    int best(double nowUtc, std::string* why = nullptr) const;
    // Text form for keeping the history between runs (one line per bin) and back; load() ignores damaged lines and bins older than 24 h
    std::string save() const;
    void load(const std::string& text, double nowUtc);
    void clear();
    uint64_t total() const { return total_; }

private:
    struct Slot { Bin b; std::set<std::string> ids; int bySource[kSources] = {}; };
    Slot slots_[kBands][kBins];
    uint64_t total_ = 0;
};

} // namespace dect2::hfp
