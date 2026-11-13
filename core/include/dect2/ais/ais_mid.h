// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The country and the kind of station behind an MMSI (ITU-R M.585): ship stations carry the MID (Maritime Identification Digits) first,
// coast stations as 00MID, groups as 0MID, SAR aircraft 111MID, auxiliary craft 98MID, aids to navigation 99MID, handhelds 8MID, and the
// 970 / 972 / 974 devices (AIS-SART, man overboard, EPIRB-AIS) carry no country. The MID list is core/src/ais/ais_mid_table.inc (ITU).
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {

enum AisMmsiKind { AIS_MMSI_UNKNOWN, AIS_MMSI_SHIP, AIS_MMSI_COAST, AIS_MMSI_GROUP, AIS_MMSI_SAR_AIRCRAFT, AIS_MMSI_AUXILIARY, AIS_MMSI_ATON, AIS_MMSI_HANDHELD,
                   AIS_MMSI_SART, AIS_MMSI_MOB, AIS_MMSI_EPIRB, AIS_MMSI_KINDS };

struct AisMmsiInfo {
    int kind = AIS_MMSI_UNKNOWN;
    int mid = 0;                    // 0: none (SART / MOB / EPIRB, or not an MMSI of this scheme)
    const char* iso = "";           // ISO 3166-1 alpha-2, "" when the MID is not in the list
    const char* country = "";
};

AisMmsiInfo aisMmsiInfo(uint32_t mmsi);
const char* aisMmsiKindText(int kind);   // "Ship", "Coast station", "Group call", "SAR aircraft", "Auxiliary craft", "Aid to navigation", "Handheld VHF", "AIS-SART", "Man overboard", "EPIRB-AIS", "-"
const char* aisMidIso(int mid);          // nullptr when the MID is not allocated
const char* aisMidCountry(int mid);

} // namespace dect2
