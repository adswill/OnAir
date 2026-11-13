// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The map guard: the ONE transmit rule of OnAir. Transmitting is the operator's decision (OnAir blocks no mode, band or power), but what this app
// transmits must never come back as "received" data on the shared OnAir map (someone could transmit fake AIS or ADS-B and have it published).
//  1. While the app is transmitting, and for 10 minutes after the last transmission ends, nothing is uploaded to the map server (holding()).
//     Decoding and showing data locally go on; only the upload is held.
//  2. Whatever this app transmitted in the last 24 hours is identified (callsign, MMSI, ICAO address, name, text words) and anything whose
//     identity matches is never uploaded (idBlocked()), also after the 10 minutes.
// Pure logic with an explicit clock (seconds), thread safe; the *Now() forms use the wall clock. The transmitters call txBegin / txEnd and
// noteSent; the map uploader (app/map/map_send.cpp) asks holding() and idBlocked().
#pragma once
#include <string>
#include <vector>

namespace dect2::mapguard {

constexpr double kHoldAfterSec = 600;          // 10 minutes after the last transmission
constexpr double kIdMemorySec = 24 * 3600.0;   // identities are remembered for 24 hours

// Normal form of an identity: upper case, letters, digits and '-' only ("ly1abc-9" -> "LY1ABC-9"). A callsign with an SSID is also stored without it.
std::string normalizeId(const std::string& id);
// The identities in a transmit request: callsign, destination, name, MMSI / ICAO / capcode (decimal and 6-digit hex), and every word of the text of at least 3 characters.
std::vector<std::string> identitiesOf(const std::string& callsign, const std::string& dest, const std::string& name, unsigned address, const std::string& text);

void txBegin(double now);              // a transmitter went on air (calls nest: several transmitters at once)
void txEnd(double now);                // one of them went quiet
void noteSent(const std::string& id, double now);          // this identity was transmitted
void noteSentAll(const std::vector<std::string>& ids, double now);
bool transmitting();
bool holding(double now);              // transmitting, or the last transmission ended less than 10 minutes ago
double holdLeftSec(double now);        // 0 when not holding; 1e9 while transmitting
bool idBlocked(const std::string& id, double now);         // matched something transmitted in the last 24 hours (also "LY1ABC" for "LY1ABC-9", either way round)
void reset();                          // tests

// the same with the wall clock
void txBegin();
void txEnd();
void noteSent(const std::string& id);
bool holdingNow();
bool idBlockedNow(const std::string& id);

}  // namespace dect2::mapguard
