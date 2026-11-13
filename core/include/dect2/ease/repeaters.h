// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Repeaters near me": a list the user imported as CSV, or downloaded on request from Open Repeater (openrepeater.org, CC0 data, the
// user's own free API key); OnAir ships none. Filtered by mode and distance. Pure, no I/O.
#pragma once
#include <string>
#include <vector>

namespace dect2::ease {

struct Repeater { double mhz = 0, lat = 0, lon = 0; std::string call, where, mode; int colourCode = -1; };
// A CSV with a header row naming at least a frequency column and either latitude and longitude or a locator (common spellings are accepted).
// Fills skipped with the rows that had no usable frequency or position. On a file without those columns returns an empty list and sets err.
std::vector<Repeater> parseRepeaterCsv(const std::string& text, int& skipped, std::string& err);
// Open Repeater's REST API (https://www.openrepeater.org/api/v1, docs at /api-docs): a personal API key from a free account, 30 requests per
// key per day. openRepeaterSearchUrl is /search for a position; the key goes in the X-API-Key header, never in the address.
std::string openRepeaterSearchUrl(double lat, double lon, double radiusKm, int page, int limit);
// The rows of an answer ({"data":[...]}, {"repeaters":[...]}, {"results":[...]} or a bare array; fields callsign, frequency, city, lat, lng, mode);
// the frequency may come in MHz, kHz or Hz. A row with no usable frequency or position is skipped. Sets err when the text is not such an answer.
std::vector<Repeater> parseOpenRepeaterJson(const std::string& text, int& skipped, std::string& err);
struct RepeaterNear { const Repeater* r = nullptr; double km = 0; };
// Modes: "DMR", "D-STAR", "Fusion", "M17". A row with no mode text is kept (the list may be of that mode only).
bool repeaterMatchesMode(const Repeater& r, const std::string& mode);
std::vector<RepeaterNear> nearestRepeaters(const std::vector<Repeater>& list, const std::string& mode, double lat, double lon, int maxCount = 40, double maxKm = 300);

} // namespace dect2::ease
