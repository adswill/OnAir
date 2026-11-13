// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ham.decode: one event per station heard on the HF digital modes (FT8, FT4, FT2, WSPR, FST4, JT, MSK144, Q65) and CW, with the logbook's
// marks (new DXCC, new on the band, new grid, worked before). Feeds the automations (JTAlert-style alerts), Integrations and the message
// database ("Search everything heard"). docs/HAM-ALERTS-3.9.md.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/ham/ham_log.h"
#include <cstdint>
#include <deque>
#include <set>
#include <string>

namespace dect2::ham {

struct HamHeard {
    std::string call, grid, msg, mode, band, country;
    double freqHz = 0, utc = 0;
    float snrDb = 0, dt = 0, audioHz = 0;
    bool cq = false;
    int dbm = -1;
};

// the event: from (the call), call, grid, text (the message), mode, band, country, snr_db, dt, audio_hz, dial_mhz, cq, dbm, lat, lon (from the grid),
// new_dxcc, new_dxcc_band, new_grid, new_grid_band, worked, worked_band
automation::Event hamDecodeEvent(const HamHeard& h, const DecodeMarks& m);

// each decode once: a decode is the same as one already sent when call, mode, message and slot (rounded to 1 s) match
class HamDecodeOnce {
public:
    bool first(const std::string& call, const std::string& mode, const std::string& msg, double slotUtc);
private:
    std::set<std::string> seen_;
    std::deque<std::string> order_;
};

// CW: a call is sent once per signal and call within `holdSec` (the skimmer keeps the calls of a signal's text)
class HamCallOnce {
public:
    bool first(const std::string& key, double now, double holdSec = 600);
private:
    std::deque<std::pair<std::string, double>> recent_;
};

}  // namespace dect2::ham
