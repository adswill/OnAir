// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Export this session as one bundle" (docs/SESSION-BUNDLE.md): one plain zip (stored, opens with any tool) with the decoded log
// (CSV, plus KML and GPX when rows have positions), the recordings list, the settings (credentials, folders and the position left out),
// the radio, the location (only when the user ticks it) and a text summary. No GUI includes, so tests link it.
#pragma once
#include "results_export.h"
#include "dect2/backup/backup.h"
#include <cstdint>
#include <string>
#include <vector>

namespace sessionbundle {

struct Recording { std::string name; uint64_t bytes = 0; int64_t mtime = 0; };   // mtime: unix seconds

struct Input {
    std::string version, mode, device;
    double freqHz = 0, sampleRateHz = 0;
    double t0 = 0, t1 = 0;                       // the session span, unix seconds (0 = unknown)
    resexp::Table log;                           // decoded rows of every table (clips::tableLog().range)
    std::vector<Recording> recordings;
    std::vector<dect2::backup::PrefItem> prefs;
    bool withRecordings = true, withSettings = true, withLocation = false;
    bool hasLocation = false; double lat = 0, lon = 0; std::string grid;
};

std::string recordingsCsv(const std::vector<Recording>& r);
// Settings text for the bundle: credentials, folder paths and the position keys are dropped.
std::string settingsText(const std::vector<dect2::backup::PrefItem>& prefs);
std::string summaryText(const Input& in);
std::vector<dect2::backup::Entry> entries(const Input& in);
std::string build(const Input& in);              // the zip bytes
std::string fileName(int64_t epochSec);          // onair-session-YYYYMMDD-HHMMSS.zip (UTC)

}  // namespace sessionbundle
