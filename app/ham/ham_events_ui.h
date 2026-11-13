// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ham.decode from the HF digital modes and the CW skimmer (ham_events_ui.cpp; core: dect2/ham/ham_events.h)
#pragma once
struct App;
#include <string>
namespace dect2 { struct HfdigFtxTelemetry; struct CwTelemetry; struct HfdigKbdTelemetry; }
void hamFeedFtx(App& a, const dect2::HfdigFtxTelemetry& f);
void hamFeedCw(App& a, const dect2::CwTelemetry& t);
void hamBandOpenPanel();   // the band by UTC hour matrix of your own spots (7 or 30 days)
void hamFeedKbd(App& a, const dect2::HfdigKbdTelemetry& k, const char* mode);   // RTTY, PSK and the other keyboard modes: the last callsign
// a callsign as the logbook sees it on this frequency and mode: the country tag ("Germany, CQ 14, ITU 28") and how new it is
// (3 new DXCC, 2 new on the band, 1 not worked, 0 worked before); false when the call is empty
bool hamCallInfo(const std::string& call, double freqHz, const char* mode, std::string& countryTag, int& level);
// draws the call coloured by hamCallInfo, with the country after it (and as the tooltip)
void hamCallText(const std::string& call, double freqHz, const char* mode, bool withCountry = true);
