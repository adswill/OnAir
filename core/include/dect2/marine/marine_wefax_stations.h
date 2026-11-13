// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF radiofax stations of the world and their broadcast times: the table behind the weather fax directory and schedule.
// The data lives in core/src/marine/marine_wefax_stations.cpp, one line per frequency and per broadcast, so it is easy to update.
// Source: the NOAA "Worldwide Marine Radiofacsimile Broadcast Schedules" (weather.gov/marine/radiofax) and the stations' own
// schedules. Stations change frequencies and times: the entries marked approximate are known only roughly.
#pragma once
#include <string>
#include <vector>

namespace dect2 {

struct WefaxStation {
    const char* id;          // short key, e.g. "DDK"
    const char* name;        // e.g. "DWD Pinneberg (Hamburg)"
    const char* callsign;
    const char* country;
    const char* region;      // Europe, North America, Asia, Oceania, South America, Africa, Middle East
    std::vector<double> carrierKhz;   // the assigned (carrier) frequencies; the USB dial is 1.9 kHz below
};

struct WefaxBroadcast {
    const char* station;     // WefaxStation::id
    int utcMin;              // start, minutes after 00:00 UTC
    int lpm, ioc;
    const char* product;
    bool approximate;        // the time or the product is known only roughly
};

const std::vector<WefaxStation>& wefaxStations();
const std::vector<WefaxBroadcast>& wefaxBroadcasts();
const WefaxStation* wefaxStation(const std::string& id);
inline double wefaxDialKhz(double carrierKhz) { return carrierKhz - 1.9; }   // USB: the fax centre (1900 Hz) sits on the carrier

// The next broadcasts after `nowUtcSec` (Unix time), soonest first; `region` empty = all.
struct WefaxNext { const WefaxBroadcast* b; const WefaxStation* s; long long startUtc; };
std::vector<WefaxNext> wefaxNext(long long nowUtcSec, int count, const std::string& region = "");

} // namespace dect2
