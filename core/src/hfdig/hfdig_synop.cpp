// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/hfdig/hfdig_synop.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace dect2 {
namespace hfdig {
namespace {

bool digits(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return true;
}
int num(const std::string& s, size_t pos, size_t len) { return std::stoi(s.substr(pos, len)); }

// "sTTT": sign digit (0 positive, 1 negative) and tenths of a degree; false for a group that is not a temperature
bool temp(const std::string& g, double& out) {
    if (!digits(g, 5) || (g[1] != '0' && g[1] != '1')) return false;
    out = num(g, 2, 3) / 10.0 * (g[1] == '1' ? -1 : 1);
    return true;
}
// "PPPP": tenths of hPa without the thousands: 0000..0999 are 1000.0..1099.9
double pressure(const std::string& g) {
    const int p = num(g, 1, 4);
    return (p < 5000 ? p + 10000 : p) / 10.0;
}

// the fixed groups after the identification of a report: iRiXhVV, Nddff, then the numbered groups until section 2 or 3
void section1(const std::vector<std::string>& t, size_t i, int windUnitKn, WxReport& r) {
    if (i < t.size() && t[i].size() == 5) i++;   // iR iX h VV (precipitation / weather indicators, cloud base, visibility): not read
    if (i < t.size() && t[i].size() == 5 && (std::isdigit((unsigned char)t[i][0]) || t[i][0] == '/')) {
        const std::string& g = t[i++];
        if (std::isdigit((unsigned char)g[0])) r.oktas = g[0] - '0';
        if (digits(g.substr(1), 4)) {
            const int dd = num(g, 1, 2) * 10, ff = num(g, 3, 2);
            if (dd <= 360 && ff < 99) { r.hasWind = true; r.windDeg = dd; r.windKn = windUnitKn ? ff : ff * 1.943844; }
        }
    }
    for (; i < t.size(); i++) {
        const std::string& g = t[i];
        if (g == "333" || g == "555" || g == "222" || g.rfind("222", 0) == 0) {
            if (r.ship) for (size_t k = i + 1; k < t.size() && k < i + 3; k++) if (t[k][0] == '0' && temp(t[k], r.seaC)) { r.hasSeaTemp = true; break; }
            return;
        }
        if (!digits(g, 5)) continue;
        switch (g[0]) {
            case '1': if (temp(g, r.tempC)) r.hasTemp = true; break;
            case '2': if (g[1] != '9' && temp(g, r.dewC)) r.hasDew = true; break;   // 29: relative humidity, not a dew point
            case '3': r.hasPress = true; r.stationHpa = pressure(g); break;
            case '4': r.hasSlp = true; r.slpHpa = pressure(g); break;
            default: break;
        }
    }
}

}  // namespace

std::vector<WxReport> parseWxReports(const std::string& text) {
    std::vector<WxReport> out;
    std::istringstream in(text);
    std::vector<std::string> tok;
    for (std::string w; in >> w;) tok.push_back(w);
    for (size_t i = 0; i < tok.size(); i++) {
        const bool land = tok[i] == "AAXX", ship = tok[i] == "BBXX";
        if (!land && !ship) continue;
        size_t e = i + 1;
        while (e < tok.size() && tok[e].back() != '=') e++;
        if (e >= tok.size()) break;   // not finished yet
        std::vector<std::string> t(tok.begin() + (long)i + 1, tok.begin() + (long)e + 1);
        t.back().pop_back();
        i = e;
        WxReport r; r.ship = ship;
        size_t k = 0;
        if (land) {   // AAXX YYGGi IIiii ...
            if (t.size() < 3 || !digits(t[0], 5) || !digits(t[1], 5)) continue;
            r.day = num(t[0], 0, 2); r.hour = num(t[0], 2, 2); r.id = t[1]; k = 2;
            section1(t, k, (t[0][4] == '3' || t[0][4] == '4') ? 1 : 0, r);
        } else {      // BBXX call YYGGi 99LaLaLa QcLoLoLoLo ...
            if (t.size() < 5 || !digits(t[1], 5) || !digits(t[2], 5) || !digits(t[3], 5) || t[2].substr(0, 2) != "99") continue;
            r.id = t[0]; r.day = num(t[1], 0, 2); r.hour = num(t[1], 2, 2);
            const int qc = t[3][0] - '0';
            if (qc == 1 || qc == 3 || qc == 5 || qc == 7) {
                r.hasPos = true; r.lat = num(t[2], 2, 3) / 10.0; r.lon = num(t[3], 1, 4) / 10.0;
                if (qc == 3 || qc == 5) r.lat = -r.lat;
                if (qc == 5 || qc == 7) r.lon = -r.lon;
            }
            section1(t, 4, (t[1][4] == '3' || t[1][4] == '4') ? 1 : 0, r);
        }
        if (r.day >= 1 && r.day <= 31 && r.hour <= 24) out.push_back(r);
    }
    return out;
}

std::string wxLine(const WxReport& r) {
    char b[320]; size_t n = 0;
    n += (size_t)snprintf(b + n, sizeof b - n, "%s %s  day %d %02d UTC", r.ship ? "SHIP" : "SYNOP", r.id.c_str(), r.day, r.hour);
    if (r.hasPos) n += (size_t)snprintf(b + n, sizeof b - n, "  %.1f%c %.1f%c", std::abs(r.lat), r.lat < 0 ? 'S' : 'N', std::abs(r.lon), r.lon < 0 ? 'W' : 'E');
    if (r.hasWind) n += (size_t)snprintf(b + n, sizeof b - n, "  wind %d deg %.0f kn", r.windDeg, r.windKn);
    if (r.hasTemp) n += (size_t)snprintf(b + n, sizeof b - n, "  %.1f C", r.tempC);
    if (r.hasDew) n += (size_t)snprintf(b + n, sizeof b - n, "  dew %.1f C", r.dewC);
    if (r.hasSeaTemp) n += (size_t)snprintf(b + n, sizeof b - n, "  sea %.1f C", r.seaC);
    if (r.hasSlp) n += (size_t)snprintf(b + n, sizeof b - n, "  %.1f hPa", r.slpHpa);
    else if (r.hasPress) n += (size_t)snprintf(b + n, sizeof b - n, "  station %.1f hPa", r.stationHpa);
    if (r.oktas >= 0 && r.oktas <= 8) n += (size_t)snprintf(b + n, sizeof b - n, "  cloud %d/8", r.oktas);
    return std::string(b, n < sizeof b ? n : sizeof b - 1);
}

}  // namespace hfdig
}  // namespace dect2
