// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Search everything heard: one box for a call sign, ICAO address, MMSI or frequency, over the history of every mode in the unified message database
// (core/msgdb), grouped per station with where and when it was heard. A live position from the map screens' tracks is added when one is known (track_hub.h).
#pragma once
#include "dect2/msgdb/msgdb.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2::station {

enum class QueryKind { Empty, Frequency, Icao, Mmsi, Callsign, Text };

struct Query {
    QueryKind kind = QueryKind::Empty;
    std::string text;          // trimmed
    double loHz = 0, hiHz = 0; // Frequency: the range searched
};

// "162.025", "162.025 MHz", "162025 kHz", "144-146" (MHz range) are frequencies (a decimal point or a unit is required, so a bare number stays an identity);
// six hexadecimal digits are an ICAO address, nine digits an MMSI, a word mixing letters and digits (optional -SSID) a call sign, anything else plain text.
Query parseQuery(const std::string& s, double tolHz = 5000);

// What kind of identity a stored name looks like
QueryKind identKind(const std::string& ident);

struct Heard {
    std::string ident;              // "" = records with no identity, grouped by mode
    QueryKind kind = QueryKind::Text;
    std::vector<std::string> modes; // sorted
    int64_t count = 0;              // messages matched
    double first = 0, last = 0;     // Unix seconds
    double lastFreqHz = 0;
    std::string lastText;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool live = false;              // the position came from a live track
};

// Live position of an identity, optional
using LiveLookup = std::function<bool(const std::string& ident, double& lat, double& lon)>;

// The matches, an exact identity first, then the most recently heard. maxRows bounds the records looked at (newest first).
std::vector<Heard> searchHeard(const msgdb::Db& db, const std::string& query, const LiveLookup& live = nullptr, int maxRows = 20000);

} // namespace dect2::station
