// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker advertisements (see bletrk_proto.h).
#include "dect2/bletrk/bletrk_proto.h"

namespace dect2 {
namespace bletrk {

const char* kindName(int k) {
    switch (k) {
    case FindMy: return "Apple Find My (AirTag or compatible)";
    case Tile: return "Tile";
    case Chipolo: return "Chipolo";
    case SmartTag: return "Samsung SmartTag";
    case GoogleFmd: return "Google Find My Device tag";
    default: return "-";
    }
}
const char* kindShort(int k) {
    switch (k) {
    case FindMy: return "Find My";
    case Tile: return "Tile";
    case Chipolo: return "Chipolo";
    case SmartTag: return "SmartTag";
    case GoogleFmd: return "Google";
    default: return "-";
    }
}

namespace {
Kind kindOfUuid(int u) {
    switch (u) {
    case 0xFEED: case 0xFD84: return Tile;       // Tile, Inc.
    case 0xFE33: return Chipolo;                 // CHIPOLO d.o.o.
    case 0xFD5A: return SmartTag;                // Samsung Electronics (SmartThings Find)
    default: return None;
    }
}
}

bool detect(const uint8_t* ad, size_t n, Detection& out) {
    out = Detection();
    if (!ad) return false;
    size_t i = 0;
    while (i < n) {
        const size_t len = ad[i];
        if (len == 0) break;
        if (i + 1 + len > n) break;
        const int type = ad[i + 1];
        const uint8_t* d = ad + i + 2;
        const size_t dl = len - 1;
        if (type == 0xFF && dl >= 2) {                                   // manufacturer data
            const int company = d[0] | d[1] << 8;
            if (company == kAppleCompany && dl >= 4 && d[2] == kAppleOfflineFinding && d[3] == kFindMyLen && dl >= 2 + 2 + (size_t)kFindMyLen) {
                out.kind = FindMy; out.separated = true; out.status = d[4];
            }
        } else if (type == 0x02 || type == 0x03) {                       // 16-bit service UUID lists
            for (size_t k = 0; k + 1 < dl; k += 2) {
                const Kind kd = kindOfUuid(d[k] | d[k + 1] << 8);
                if (kd != None && out.kind == None) out.kind = kd;
            }
        } else if (type == 0x16 && dl >= 2) {                            // service data
            const int u = d[0] | d[1] << 8;
            const Kind kd = kindOfUuid(u);
            if (kd != None && out.kind == None) out.kind = kd;
            if (u == 0xFEAA && dl >= 3 && (d[2] == 0x40 || d[2] == 0x41) && out.kind == None) {   // Google FMDN, in an Eddystone service data frame
                out.kind = GoogleFmd; out.unwantedProtection = d[2] == 0x41;
            }
        }
        i += 1 + len;
    }
    return out.kind != None;
}

} // namespace bletrk
} // namespace dect2
