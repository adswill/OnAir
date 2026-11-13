// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BGAN mode telemetry, shared by the receiver and the UI. Receive only. Holds signalling and carrier structure only: no subscriber identity, no user data.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct BganCarrierRow {
    int id = 0;
    double freqHz = 0;          // absolute (0 when the radio frequency is not known)
    double offsetHz = 0;        // in the current capture
    double widthHz = 0;
    float snrDb = 0;
    int kind = 0;               // 0 not identified, 1 BGAN forward bearer, 2 GMR-2 class (IsatPhone-type) forward carrier by its spectrum line, 3 other / too narrow
    int state = 0;              // 0 searching, 1 frame sync, 2 signalling decoded
    std::string bearer;         // "F80T1Q-4B" when identified, "" otherwise
    std::string subtype;
    double symRate = 0;
    double cfoHz = 0;
    float cn0Dbhz = 0;
    uint64_t frames = 0, blocksOk = 0, blocksBad = 0;
    float load = -1;            // share of FEC blocks that carried something, -1 unknown
    bool structureOnly = false; // frame sync holds, nothing decodable (F80T1Q-1B: tables not published, or below the code's limit)
    int spotBeam = -1, rnc = -1, bctId = -1, fBearer = -1, beamType = -1;
    std::string plmn, satellite;
    double lastSeenSec = 0;
};

struct BganBeam { int spot = 0; int beamType = -1; double mhz = 0; std::string bearer; };

struct BganTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 searching, 1 carriers found, 2 signalling decoded
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;     // a bulletin board was decoded
    uint64_t blocksOk = 0, blocksBad = 0;
    double rate = 0, centerHz = 0;
    int carriers = 0, bearers = 0, identifiedGmr = 0;
    float floorDb = 0;
    std::vector<BganCarrierRow> rows;     // strongest first
    std::vector<BganBeam> beams;          // primary bearers announced by the network
    std::vector<std::string> log;         // plain-language lines, newest first, up to 200
    std::string network, satellites;
    bool scanning = false;                // the Scan button's walk across 1525 - 1559 MHz
    int scanStep = 0, scanSteps = 0;
    std::string note;
};

} // namespace dect2
