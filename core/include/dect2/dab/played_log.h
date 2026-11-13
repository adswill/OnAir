// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Played list of FM (RadioText+) and DAB (DL Plus): every "artist - title" a station tagged, with the time and the station, searchable and
// exportable ("what was that song 20 minutes ago?"). One entry per song: repeats of the same tags and tags that arrive one at a time are merged.
// docs/PLAYED-3.9.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct PlayedItem {
    int64_t utc = 0;             // first heard
    int64_t lastUtc = 0;         // last heard with these tags
    std::string source;          // "FM" or "DAB"
    std::string station;         // PS name / service label
    double freqMhz = 0;          // FM frequency or DAB channel centre
    std::string artist, title;
};

class PlayedLog {
public:
    // the tags as the station sends them now (either may be empty). Returns true when this made a new entry (a new song).
    bool note(const std::string& source, const std::string& station, double freqMhz, const std::string& artist, const std::string& title, int64_t utc);
    const std::vector<PlayedItem>& items() const { return items_; }   // oldest first
    // newest first, matching every word of `query` (any case) in station, artist or title
    std::vector<PlayedItem> find(const std::string& query, size_t max = 1000) const;
    void add(const PlayedItem& p);   // from the saved file
    void clear() { items_.clear(); }
    static constexpr size_t kKeep = 20000;
    static constexpr int kSameSongSec = 20 * 60;   // the same tags again within this time are the same song (stations repeat them)
private:
    std::vector<PlayedItem> items_;
};

// CSV: utc (ISO), source, station, freq_mhz, artist, title
std::string playedCsv(const std::vector<PlayedItem>& v);
std::vector<PlayedItem> playedFromCsv(const std::string& text);

}  // namespace dect2
