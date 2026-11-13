// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather fax archive: the pure rules behind the autosave and the gallery (file names, the JSON side-car, the disk-budget cleanup,
// thumbnail scaling, sort and search). No file or UI access here, so it is unit tested (tests/marine/test_marine_wefax_archive.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct WefaxStation;

// What is stored next to each saved chart (<base>.json).
struct WefaxArchiveMeta {
    double dialKhz = 0;          // the USB dial the receiver was on
    int ioc = 576, lpm = 120;
    long long startUtc = 0;      // Unix seconds, first line
    long long stopUtc = 0;       // Unix seconds, end of the chart
    int width = 0, height = 0;
    std::string station;         // schedule station name when the frequency matched, else empty
    std::string stationId;       // WefaxStation::id, else empty
    double snrDb = 0;            // tone / phasing SNR at the end (sync quality)
    int phasingLines = 0;        // phasing lines that locked the line start (0 = no phasing seen)
    std::string endReason;       // "stop tone", "timeout", "new start", "manual"
    bool starred = false;
};

// "wefax_20261022T123045Z_7878.1kHz" (UTC start, dial in kHz with one decimal), without extension and folder.
std::string wefaxArchiveBase(long long startUtc, double dialKhz);
// A name free in `taken` (the same bases already on disk): a "_2", "_3" ... suffix is added when needed.
std::string wefaxArchiveUniqueBase(const std::string& base, const std::vector<std::string>& taken);
// The base back into start time and dial; false when the name is not one of ours.
bool wefaxArchiveParseBase(const std::string& base, long long& startUtc, double& dialKhz);

std::string wefaxMetaToJson(const WefaxArchiveMeta& m);
bool wefaxMetaFromJson(const std::string& json, WefaxArchiveMeta& m);   // false when it is not a side-car; unknown keys are ignored

// The scheduled station whose carrier or USB dial is within tolKhz of the given dial frequency; null when none.
const WefaxStation* wefaxStationForDial(double dialKhz, double tolKhz = 0.6);

// One saved chart as the folder scan sees it.
struct WefaxArchiveEntry {
    std::string base;
    long long bytes = 0;         // PNG + side-car
    long long startUtc = 0;
    double dialKhz = 0;
    std::string station;
    bool starred = false;
};

// Cleanup rule: while the total is over `budgetBytes`, drop the oldest entries first; starred ones are never dropped (the total may
// stay over budget then). budgetBytes <= 0 means no limit. Returns the indices into `e` to delete, oldest first.
std::vector<size_t> wefaxCleanupPlan(const std::vector<WefaxArchiveEntry>& e, long long budgetBytes);

enum class WefaxSort { TimeNewest, TimeOldest, Station, Frequency };
// Indices of the entries matching `query` (case-insensitive substring of station, base, frequency text; empty = all), in sort order.
// starredOnly keeps only starred entries.
std::vector<size_t> wefaxGalleryList(const std::vector<WefaxArchiveEntry>& e, WefaxSort sort, const std::string& query, bool starredOnly = false);

// Thumbnail size that fits (w x h) in a maxDim x maxDim box, keeping the aspect ratio (never larger than the source, at least 1 px).
void wefaxThumbSize(int w, int h, int maxDim, int& tw, int& th);
// Box-average downscale of an 8-bit grey picture to tw x th (tw,th from wefaxThumbSize). out is tw*th.
void wefaxThumbnail(const uint8_t* pix, int w, int h, int tw, int th, std::vector<uint8_t>& out);

} // namespace dect2
