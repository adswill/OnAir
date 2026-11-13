// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Elektro-L HRIT / LRIT / GGAK telemetry (Roscosmos geostationary weather: N3, N4, N5).
#pragma once
#include "elektro_ggak.h"
#include "dect2/goes/goes_tel.h"
#include <cstdio>
#include <string>

namespace dect2 {

inline const char* elektroCraftName(int craft) {
    switch (craft) {
    case 1: return "Elektro-L N4";
    case 2: return "Elektro-L N5";
    default: return "Elektro-L N3";
    }
}
inline double elektroCraftMhz(int craft) {
    (void)craft;
    return 1691.0;   // L-band HRIT / LRIT; LRIT is also listed on 1693 MHz
}
inline const char* elektroCraftPos(int craft) {
    switch (craft) {
    case 1: return "165.8°E";
    case 2: return "14.5°W";
    default: return "76°E";
    }
}
inline bool elektroIsGgak(int link) { return link == 4; }
inline const char* elektroLinkName(int link) {
    switch (link) {
    case 1: return "HRIT 1.0 Msym/s";
    case 2: return "LRIT 128 ksym/s";
    case 3: return "LRIT 64 ksym/s";
    case 4: return "GGAK 5 ksym/s";
    case 5: return "LRIT 294 ksym/s";
    case 6: return "HRIT 1.15 Msym/s OQPSK";
    default: return "HRIT 665.4 ksym/s";
    }
}
inline double elektroSymbolRate(int link) {
    switch (link) {
    case 1: return 1e6;
    case 2: return 128e3;
    case 3: return 64e3;
    case 4: return kGgakSymbolRate;
    case 5: return 294000.0;
    case 6: return 1150000.0;
    default: return 665400.0;
    }
}
inline double elektroLinkMhz(int link) { return elektroIsGgak(link) ? kGgakMhz : 1691.0; }

struct ElektroTelemetry {
    GoesTelemetry hrit;
    GgakTelemetry ggak;
    int craft = 0;                // 0 N3, 1 N4, 2 N5
    int link = 0;                 // 0 HRIT 665.4k, 1 HRIT 1M, 2 LRIT 128k, 3 LRIT 64k, 4 GGAK
};

inline std::string elektroSummary(const ElektroTelemetry& t) {
    char b[240];
    const char* sat = elektroCraftName(t.craft);
    if (elektroIsGgak(t.link)) {
        const GgakTelemetry& g = t.ggak;
        if (g.state >= 2) {
            snprintf(b, sizeof b, "%s GGAK: %llu frames (%llu bad), marker %d, Es/N0 %.1f dB", sat,
                     (unsigned long long)g.blocksOk, (unsigned long long)g.blocksBad, g.lastMarker, g.snrDb);
            return b;
        }
        if (g.state == 1) {
            snprintf(b, sizeof b, "%s GGAK: carrier locked (%.1f kHz), looking for frames", sat, g.cfoHz / 1000);
            return b;
        }
        snprintf(b, sizeof b, "%s GGAK: searching for the 5 ksym/s PCM/PM carrier on 1693 MHz", sat);
        return b;
    }
    const GoesTelemetry& h = t.hrit;
    if (h.state >= 2) {
        snprintf(b, sizeof b, "%s %s: %llu frames (%llu bad), %zu products, Es/N0 %.1f dB", sat, elektroLinkName(t.link),
                 (unsigned long long)h.blocksOk, (unsigned long long)h.blocksBad, h.products.size(), h.snrDb);
        return b;
    }
    if (h.state == 1) {
        snprintf(b, sizeof b, "%s: carrier locked (%.1f kHz), looking for frames", sat, h.cfoHz / 1000);
        return b;
    }
    snprintf(b, sizeof b, "%s: searching for the %.1f ksym/s BPSK carrier", sat, elektroSymbolRate(t.link) / 1e3);
    return b;
}

} // namespace dect2
