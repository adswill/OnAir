// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Automatic satellite identification for the Inmarsat all-carriers mode. Three independent kinds of evidence, each scored 0..1 per candidate satellite
// and then combined, so one wrong clue does not decide:
//   content   - the Aero P channel system table (satellite id, longitude, spot / global beam: aero_su.h, JAERO's reading of the SUs) and the
//               Inmarsat-C bulletin board (ocean region code 0 AOR-W, 1 AOR-E, 2 POR, 3 IOR);
//   frequency - the set of P carriers against the published channel lists of each satellite (a score over all carriers, not one carrier);
//   geometry  - the azimuth and elevation of each geostationary slot from the user's location: below the horizon cannot be received.
// Sources of the tables: the P channel lists are those of github.com/alphafox02/inmarsat-sniffer (satellites.c, taken from Inmarsat's L-band
// frequency list), as quoted in app/aero/aero_ui.cpp; the slot longitudes are the nominal ones of the Inmarsat ocean regions. Satellites move
// and get replaced: every table entry is UNVERIFIED against a real capture ("needs a user test with a real capture"). Pure functions, no I/O.
#pragma once
#include <string>
#include <vector>

namespace dect2::inmsat {

struct SatDef {
    const char* name;
    double lonDeg;                 // east positive
    int oceanRegion;               // Inmarsat-C code 0 AOR-W, 1 AOR-E, 2 POR, 3 IOR, -1 none
    double p600Lo, p600Hi;         // MHz, the 600 / 1200 bit/s P channels (0 = no published list)
    double p10500Lo, p10500Hi;     // MHz, the 10500 bit/s P channels
};
const std::vector<SatDef>& satTable();

struct SatCarrier { double freqHz = 0; int bitRate = 0; bool aero = true; };   // an Aero P carrier in sync or seen (freqHz absolute, 0 unknown)
struct SatSysFact { int kind = 0; int satId = -1; double lonDeg = 0; double pMHz = 0; bool spotBeam = false; int gesId = -1; };
struct SatInput {
    std::vector<SatCarrier> carriers;
    std::vector<SatSysFact> sys;            // from the Aero system table
    std::vector<int> inmcRegions;           // from Inmarsat-C bulletin boards
    bool haveLocation = false;
    double latDeg = 0, lonDeg = 0;
    std::string manual;                     // a satellite name from the table, or "" for automatic
};

struct SatEvidence {
    std::string kind;                       // "content", "frequency", "geometry"
    bool available = false;                 // false: nothing to judge from (no system table heard, no frequency, no location)
    double score = 0;                       // 0..1 when available
    std::string text;
};
struct SatCandidate {
    int index = -1;                         // into satTable()
    double confidence = 0;                  // 0..1
    double azimuthDeg = 0, elevationDeg = 0;
    bool visible = false;
    std::vector<SatEvidence> evidence;
};
struct SatResult {
    bool valid = false;                     // a satellite is named
    bool manual = false;
    std::string name;
    double lonDeg = 0;
    std::string beam;                       // "spot beam", "global beam" or "" (the beam's own name is not in the signal we decode)
    double confidence = 0;
    bool ambiguous = false;                 // the runner-up is close
    std::string summary;                    // one line for the UI
    std::vector<SatCandidate> ranked;       // best first, up to 6
};

// Azimuth and elevation of a geostationary satellite at lonSatDeg seen from the ground
void geoLook(double latDeg, double lonDeg, double lonSatDeg, double& azDeg, double& elDeg);
SatResult identifySatellite(const SatInput& in);
// The name to show on a carrier at freqHz: the identified satellite when the carrier is in its published list or the identification is sure
// enough (confidence >= 0.5), else "".
std::string satNameForCarrier(const SatResult& r, double freqHz, int bitRate);

} // namespace dect2::inmsat
