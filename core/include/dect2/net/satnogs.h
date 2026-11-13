// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SatNOGS DB (db.satnogs.org), read only: the public lists of satellites and transmitters, parsed from the JSON the API answers with,
// the mapping of SatNOGS mode names to OnAir modes and the list filters. Nothing here touches the network (the panel downloads with curl).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct SatnogsTx {
    std::string uuid, satName, description, mode, status, service;
    int norad = 0;
    double downHz = 0, upHz = 0, baud = 0;   // 0 = not listed
    bool alive = false;                      // the transmitter is alive and its status is "active"
};

// /api/satellites/: NORAD number -> name. Returns the number of satellites read (0 for text that is not a JSON array of objects).
int parseSatnogsSatellites(const std::string& json, std::map<int, std::string>& names);
// /api/transmitters/: the downlinks and uplinks, named from `names` where the NORAD number is known. Entries without a frequency are kept
// (downHz = 0). Returns the number read.
int parseSatnogsTransmitters(const std::string& json, const std::map<int, std::string>& names, std::vector<SatnogsTx>& out);

// The OnAir mode family (the number selectMode takes) that receives a SatNOGS mode name at this baud rate, 0 when OnAir has none (then the panel
// only tunes). Case does not matter; "AFSK1k2", "BPSK1k2", "GMSK9k6", "FM", "CW", "LRPT", "APT" and their relatives are known.
int satnogsFamily(const std::string& mode, double baud);

struct SatnogsFilter {
    bool aliveOnly = true;
    double minMhz = 0, maxMhz = 0;   // 0 = no limit on that side
    std::string modeText, text;      // case-insensitive substrings of the mode / of satellite name and description
};
bool satnogsMatches(const SatnogsTx& t, const SatnogsFilter& f);
// A cached download is used for a day: true when it is missing (fetchedSec = 0), older than maxAgeSec or from the future.
bool satnogsCacheStale(double nowSec, double fetchedSec, double maxAgeSec = 86400);

}
