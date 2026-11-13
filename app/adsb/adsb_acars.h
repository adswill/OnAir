// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ACARS messages attached to the aircraft the ADS-B screens (and the map upload) show. The log itself is core/src/acars/acars_log.cpp;
// this file feeds it from the ACARS, VDL2 and HFDL receivers of every receiver slot and answers "what did this aircraft say".
#pragma once
#include "dect2/acars/acars_log.h"
#include <string>
#include <vector>

struct App;

namespace acarslog {

void tick(App& first);                         // once per frame (cheap: it works about once a second)
int count(uint32_t icao);                      // interesting messages attached to this ICAO address (for the badge)
// Messages of an ADS-B aircraft, newest first. all: also the ones not judged interesting (link tests, acknowledgements, ...).
std::vector<dect2::AcarsLogEntry> entries(uint32_t icao, const std::string& callsign, bool all);
// Interesting messages for a map-upload track (id: ICAO hex, or a registration/flight for aircraft without an address).
std::vector<dect2::AcarsLogEntry> forTrack(const std::string& id, const std::string& label, int callsignHolders);
bool& showAll();                               // "Show all messages" in the aircraft detail
std::string asText(const std::vector<dect2::AcarsLogEntry>& v);   // the list as plain text for Copy
std::string timeText(int64_t wall);            // "14:03:22Z"

} // namespace acarslog
