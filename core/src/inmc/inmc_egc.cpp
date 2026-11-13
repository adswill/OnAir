// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/inmc/inmc_egc.h"
#include <cctype>
#include <vector>

namespace dect2 {
namespace inmc {

namespace {
std::string up(const std::string& s) { std::string o = s; for (auto& c : o) c = (char)std::toupper((unsigned char)c); return o; }
bool has(const std::string& u, const char* w) { return u.find(w) != std::string::npos; }
bool wordAt(const std::string& u, size_t i, size_t n) {   // the letters at [i, i+n) form a whole word
    return (i == 0 || !std::isalnum((unsigned char)u[i - 1])) && (i + n >= u.size() || !std::isalnum((unsigned char)u[i + n]));
}
}  // namespace

std::string romanNumeral(int n) {
    static const char* r[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX", "XXI"};
    return n >= 1 && n <= 21 ? r[n] : "";
}

int areaNumberFromText(const std::string& s0) {
    const std::string s = up(s0);
    if (s.empty() || s.size() > 5) return 0;
    if (std::isdigit((unsigned char)s[0])) {
        int v = 0;
        for (char c : s) { if (!std::isdigit((unsigned char)c)) return 0; v = v * 10 + (c - '0'); }
        return v >= 1 && v <= 21 ? v : 0;
    }
    for (int n = 1; n <= 21; n++) if (s == romanNumeral(n)) return n;
    return 0;
}

EgcInfo classifyEgc(int service, int priority, const std::string& text) {
    EgcInfo r;
    const std::string u = up(text);
    // the area: the first "NAVAREA x" / "METAREA x" (x a Roman numeral or a number); a message that names both keeps the first
    for (size_t i = 0; i + 7 <= u.size() && r.areaNumber == 0; i++) {
        const bool nav = u.compare(i, 7, "NAVAREA") == 0, met = u.compare(i, 7, "METAREA") == 0;
        if ((!nav && !met) || !wordAt(u, i, 7)) continue;
        size_t j = i + 7;
        while (j < u.size() && (u[j] == ' ' || u[j] == '\n' || u[j] == '\t')) j++;
        size_t e = j;
        while (e < u.size() && std::isalnum((unsigned char)u[e])) e++;
        const int n = areaNumberFromText(u.substr(j, e - j));
        if (n) { r.areaNumber = n; r.area = std::string(nav ? "NAVAREA " : "METAREA ") + (nav ? romanNumeral(n) : std::to_string(n)); }
    }
    // the warning number "123/26" in the first 200 characters (NAVAREA IV 123/26, HYDROLANT 1234/26)
    const size_t lim = std::min<size_t>(u.size(), 200);
    for (size_t i = 0; i < lim && r.ident.empty(); i++) {
        if (!std::isdigit((unsigned char)u[i]) || (i && std::isdigit((unsigned char)u[i - 1]))) continue;
        size_t e = i;
        while (e < lim && std::isdigit((unsigned char)u[e])) e++;
        if (e - i > 4 || e + 2 >= u.size() || u[e] != '/') continue;
        if (std::isdigit((unsigned char)u[e + 1]) && std::isdigit((unsigned char)u[e + 2]) && !(e + 3 < u.size() && std::isdigit((unsigned char)u[e + 3])))
            r.ident = u.substr(i, e + 3 - i);
    }
    // the kind
    const bool relay = priority >= 3 || has(u, "MAYDAY") || (has(u, "DISTRESS") && !has(u, "DISTRESS CALL") ) || service == 0x14;
    if (service == 0x00 || service == 0x11 || service == 0x23 || service == 0x33) r.subject = "";   // system messages
    else if (relay) r.subject = "Distress relay";
    else if (service == 0x34 || service == 0x44 || has(u, "SEARCH AND RESCUE") || has(u, " SAR ") || has(u, "SAR OPERATION")) r.subject = "Search and rescue";
    else if (has(u, "FORECAST") || has(u, "WEATHER BULLETIN") || has(u, "SYNOPSIS") || has(u, "OUTLOOK")) r.subject = "Weather forecast";
    else if (has(u, "GALE") || has(u, "STORM") || has(u, "CYCLONE") || has(u, "HURRICANE") || has(u, "TYPHOON") || has(u, "SEVERE WEATHER")) r.subject = "Weather warning";
    else if (has(u, "PIRAT") || has(u, "ARMED ROBBERY") || has(u, "SECURITY")) r.subject = "Security / piracy";
    else if (has(u, "ICE ") || has(u, "ICEBERG") || has(u, "SEA ICE")) r.subject = "Ice report";
    else if (service == 0x13) r.subject = "Coastal warning";
    else if (service == 0x72) r.subject = "Chart correction";
    else if (service == 0x73) r.subject = "Chart correction";
    else if (service == 0x02) r.subject = "FleetNET message";
    else if (service == 0x31 || service == 0x04 || service == 0x24 || has(u, "NAVAREA") || has(u, "HYDROLANT") || has(u, "HYDROPAC") || has(u, "HYDROARC") || has(u, "NAV WARN"))
        r.subject = "Navigational warning";
    return r;
}

}  // namespace inmc
}  // namespace dect2
