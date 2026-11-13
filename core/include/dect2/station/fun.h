// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Four small extras on top of My station and the message history: Surprise me, rare catch alerts, the aircraft and ship dossier, plain-English messages.
// Simple versions; pure logic except the dossier, which reads the message database. Tests in tests/station/test_fun.cpp.
#pragma once
#include "dect2/msgdb/msgdb.h"
#include "dect2/station/heard.h"
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dect2::station {

// ---- Surprise me
struct SurpriseMode {
    bool ready = true;     // can be received with this station
    bool heard = false;    // already in the history
};
// A mode to try, or -1 when none is ready. Modes never heard are three times as likely. The same seed gives the same choice.
int surprise(const std::vector<SurpriseMode>& modes, uint32_t seed);

// ---- Rare catch alerts
enum class RareKind { EmergencySquawk, SartBeacon, FirstOfMode };
struct RareCatch {
    RareKind kind = RareKind::FirstOfMode;
    std::string ident, mode;
    double time = 0;
    std::string text;      // a plain sentence
};
// Watches the records as they arrive. Each identity alerts once per kind; FirstOfMode is the first message ever of a mode (known holds the modes already
// in the history when the watcher started, so an old history does not alert).
class RareWatcher {
public:
    void setKnownModes(const std::vector<std::string>& modes);
    bool check(const msgdb::Record& r, RareCatch& out);
private:
    std::set<std::string> known_, said_;
};

// ---- Dossier
struct Dossier {
    std::string ident;
    QueryKind kind = QueryKind::Text;
    std::string country;           // from the ICAO block or the MMSI's MID, "" when unknown
    std::string what;              // "ship", "coast station", "AIS-SART" ... (MMSI), "" otherwise
    std::string registration;      // only when the address alone fixes it (US, Canada, Australia)
    int64_t messages = 0;
    double first = 0, last = 0;
    std::vector<std::string> modes;
    std::vector<double> freqsHz;   // distinct, ascending
    bool hasPos = false;
    double lat = 0, lon = 0;
    std::vector<msgdb::Record> recent;   // the newest first, up to 20
};
Dossier dossier(const msgdb::Db& db, const std::string& ident);

// ---- Plain English
// One sentence saying what a stored message means, for the few things a fixed rule can say for certain (an emergency squawk, an AIS navigation status, an
// MMSI's kind and country, an ICAO block's country); "" when no rule applies. Never guesses.
std::string plainEnglish(const msgdb::Record& r);

} // namespace dect2::station
