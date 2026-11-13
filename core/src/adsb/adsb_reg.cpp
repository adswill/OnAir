// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/adsb/adsb_reg.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include "dect2/common/str.h"

namespace dect2 {

namespace {

struct Block { uint32_t first, last; const char* iso; const char* name; };

// ICAO Annex 10 Vol III, Appendix to Chapter 9 (allocation of 24-bit addresses to States): the blocks in use that are listed here, sorted by address.
const Block kBlocks[] = {
    {0x004000, 0x0043FF, "ZW", "Zimbabwe"}, {0x006000, 0x006FFF, "MZ", "Mozambique"}, {0x008000, 0x00FFFF, "ZA", "South Africa"},
    {0x010000, 0x017FFF, "EG", "Egypt"}, {0x018000, 0x01FFFF, "LY", "Libya"}, {0x020000, 0x027FFF, "MA", "Morocco"}, {0x028000, 0x02FFFF, "TN", "Tunisia"},
    {0x040000, 0x040FFF, "ET", "Ethiopia"}, {0x044000, 0x044FFF, "GH", "Ghana"}, {0x04C000, 0x04CFFF, "KE", "Kenya"},
    {0x060000, 0x0603FF, "MU", "Mauritius"}, {0x064000, 0x064FFF, "NG", "Nigeria"}, {0x06A000, 0x06A3FF, "QA", "Qatar"}, {0x06E000, 0x06EFFF, "RW", "Rwanda"},
    {0x070000, 0x070FFF, "SN", "Senegal"}, {0x0A0000, 0x0A7FFF, "DZ", "Algeria"}, {0x0AC000, 0x0AFFFF, "CO", "Colombia"},
    {0x0C0000, 0x0C3FFF, "BO", "Bolivia"}, {0x0D0000, 0x0D7FFF, "MX", "Mexico"},
    {0x100000, 0x1FFFFF, "RU", "Russia"},
    {0x300000, 0x33FFFF, "IT", "Italy"}, {0x340000, 0x37FFFF, "ES", "Spain"}, {0x380000, 0x3BFFFF, "FR", "France"}, {0x3C0000, 0x3FFFFF, "DE", "Germany"},
    {0x400000, 0x43FFFF, "GB", "United Kingdom"}, {0x440000, 0x447FFF, "AT", "Austria"}, {0x448000, 0x44FFFF, "BE", "Belgium"}, {0x450000, 0x457FFF, "BG", "Bulgaria"},
    {0x458000, 0x45FFFF, "DK", "Denmark"}, {0x460000, 0x467FFF, "FI", "Finland"}, {0x468000, 0x46FFFF, "GR", "Greece"}, {0x470000, 0x477FFF, "HU", "Hungary"},
    {0x478000, 0x47FFFF, "NO", "Norway"}, {0x480000, 0x487FFF, "NL", "Netherlands"}, {0x488000, 0x48FFFF, "PL", "Poland"}, {0x490000, 0x497FFF, "PT", "Portugal"},
    {0x498000, 0x49FFFF, "CZ", "Czechia"}, {0x4A0000, 0x4A7FFF, "RO", "Romania"}, {0x4A8000, 0x4AFFFF, "SE", "Sweden"}, {0x4B0000, 0x4B7FFF, "CH", "Switzerland"},
    {0x4B8000, 0x4BFFFF, "TR", "Turkey"}, {0x4C0000, 0x4C7FFF, "RS", "Serbia"}, {0x4C8000, 0x4C83FF, "CY", "Cyprus"}, {0x4CA000, 0x4CAFFF, "IE", "Ireland"},
    {0x4CC000, 0x4CCFFF, "IS", "Iceland"}, {0x4D0000, 0x4D03FF, "LU", "Luxembourg"}, {0x4D2000, 0x4D23FF, "MT", "Malta"}, {0x501C00, 0x501FFF, "HR", "Croatia"},
    {0x502C00, 0x502FFF, "LV", "Latvia"}, {0x503C00, 0x503FFF, "LT", "Lithuania"}, {0x504C00, 0x504FFF, "MD", "Moldova"}, {0x505000, 0x5053FF, "SK", "Slovakia"},
    {0x506000, 0x5063FF, "SI", "Slovenia"}, {0x508000, 0x50FFFF, "UA", "Ukraine"}, {0x510000, 0x5103FF, "BY", "Belarus"}, {0x511000, 0x5113FF, "EE", "Estonia"},
    {0x512000, 0x5123FF, "MK", "North Macedonia"}, {0x513000, 0x5133FF, "BA", "Bosnia and Herzegovina"}, {0x514000, 0x5143FF, "GE", "Georgia"},
    {0x600000, 0x6003FF, "AM", "Armenia"}, {0x600800, 0x600BFF, "AZ", "Azerbaijan"}, {0x683000, 0x6833FF, "KZ", "Kazakhstan"},
    {0x700000, 0x700FFF, "AF", "Afghanistan"}, {0x702000, 0x702FFF, "BD", "Bangladesh"}, {0x706000, 0x706FFF, "KW", "Kuwait"}, {0x70C000, 0x70C3FF, "OM", "Oman"},
    {0x710000, 0x717FFF, "SA", "Saudi Arabia"}, {0x718000, 0x71FFFF, "KR", "South Korea"}, {0x720000, 0x727FFF, "KP", "North Korea"}, {0x728000, 0x72FFFF, "IQ", "Iraq"},
    {0x730000, 0x737FFF, "IR", "Iran"}, {0x738000, 0x73FFFF, "IL", "Israel"}, {0x740000, 0x747FFF, "JO", "Jordan"}, {0x748000, 0x74FFFF, "LB", "Lebanon"},
    {0x750000, 0x757FFF, "MY", "Malaysia"}, {0x758000, 0x75FFFF, "PH", "Philippines"}, {0x760000, 0x767FFF, "PK", "Pakistan"}, {0x768000, 0x76FFFF, "SG", "Singapore"},
    {0x770000, 0x777FFF, "LK", "Sri Lanka"}, {0x778000, 0x77FFFF, "SY", "Syria"}, {0x780000, 0x7BFFFF, "CN", "China"}, {0x7C0000, 0x7FFFFF, "AU", "Australia"},
    {0x800000, 0x83FFFF, "IN", "India"}, {0x840000, 0x87FFFF, "JP", "Japan"}, {0x880000, 0x887FFF, "TH", "Thailand"}, {0x888000, 0x88FFFF, "VN", "Vietnam"},
    {0x890000, 0x890FFF, "YE", "Yemen"}, {0x894000, 0x894FFF, "BH", "Bahrain"}, {0x896000, 0x896FFF, "AE", "United Arab Emirates"}, {0x899000, 0x8993FF, "TW", "Taiwan"},
    {0x8A0000, 0x8A7FFF, "ID", "Indonesia"},
    {0xA00000, 0xAFFFFF, "US", "United States"},
    {0xC00000, 0xC3FFFF, "CA", "Canada"}, {0xC80000, 0xCBFFFF, "NZ", "New Zealand"},
    {0xE00000, 0xE3FFFF, "AR", "Argentina"}, {0xE40000, 0xE7FFFF, "BR", "Brazil"}, {0xE80000, 0xE80FFF, "CL", "Chile"}, {0xE84000, 0xE84FFF, "EC", "Ecuador"},
    {0xE88000, 0xE88FFF, "PY", "Paraguay"}, {0xE8C000, 0xE8CFFF, "PE", "Peru"}, {0xE90000, 0xE90FFF, "UY", "Uruguay"}, {0xE94000, 0xE94FFF, "VE", "Venezuela"},
};

const char kNLetters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ";   // 24 letters: no I, no O
const uint32_t kNSuffix = 600;                          // one or two letters: 24 * (1 + 24)
const uint32_t kNB4 = 1 + 24 + 10;                      // after four digits: nothing, a letter or a digit
const uint32_t kNB3 = 1 + kNSuffix + 10 * kNB4;
const uint32_t kNB2 = 1 + kNSuffix + 10 * kNB3;
const uint32_t kNB1 = 1 + kNSuffix + 10 * kNB2;
const uint32_t kNBase = 0xA00001, kNLast = 0xADF7C7;

int nLetter(char c) { const char* p = c ? strchr(kNLetters, c) : nullptr; return p ? (int)(p - kNLetters) : -1; }

} // namespace

AdsbCountry adsbCountry(uint32_t icao) {
    icao &= 0xFFFFFF;
    size_t lo = 0, hi = sizeof kBlocks / sizeof kBlocks[0];
    while (lo < hi) {   // the first block that starts after the address
        const size_t mid = (lo + hi) / 2;
        if (kBlocks[mid].first <= icao) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && icao <= kBlocks[lo - 1].last) return {kBlocks[lo - 1].iso, kBlocks[lo - 1].name};
    return {"", "unknown"};
}

bool adsbNNumberFromIcao(uint32_t icao, std::string& reg) {
    icao &= 0xFFFFFF;
    if (icao < kNBase || icao > kNLast) return false;
    uint32_t i = icao - kNBase;
    std::string out = "N";
    out += (char)('1' + i / kNB1);
    i %= kNB1;
    if (i == 0) { reg = out; return true; }
    i--;
    auto suffix = [&](uint32_t s) {   // 0 .. 599: A, AA, AB .. AZ, B, BA ..
        out += kNLetters[s / 25];
        if (s % 25) out += kNLetters[s % 25 - 1];
    };
    if (i < kNSuffix) { suffix(i); reg = out; return true; }
    i -= kNSuffix;
    const uint32_t bk[3] = {kNB2, kNB3, kNB4};
    for (int level = 0; level < 3; level++) {
        out += (char)('0' + i / bk[level]);
        i %= bk[level];
        if (i == 0) { reg = out; return true; }
        i--;
        if (level == 2) {   // after four digits
            if (i < 24) out += kNLetters[i];
            else out += (char)('0' + (i - 24));
            reg = out; return true;
        }
        if (i < kNSuffix) { suffix(i); reg = out; return true; }
        i -= kNSuffix;
    }
    return false;
}

bool adsbIcaoFromNNumber(const std::string& s, uint32_t& icao) {
    if (s.size() < 2 || s.size() > 6) return false;
    std::string r;
    for (char c : s) r += (char)toupper((unsigned char)c);
    if (r[0] != 'N') return false;
    size_t nd = 0;
    while (1 + nd < r.size() && isdigit((unsigned char)r[1 + nd])) nd++;
    if (nd < 1 || nd > 5 || r[1] == '0') return false;
    const std::string suf = r.substr(1 + nd);
    if (nd == 5 ? !suf.empty() : (nd == 4 ? suf.size() > 1 : suf.size() > 2)) return false;
    for (char c : suf) if (nLetter(c) < 0) return false;
    uint32_t off = (uint32_t)(r[1] - '1') * kNB1;
    const uint32_t bk[3] = {kNB2, kNB3, kNB4};
    for (size_t k = 1; k < nd && k <= 3; k++) off += 1 + kNSuffix + (uint32_t)(r[1 + k] - '0') * bk[k - 1];
    if (nd == 5) off += 1 + 24 + (uint32_t)(r[5] - '0');
    else if (!suf.empty()) {
        if (nd == 4) off += 1 + (uint32_t)nLetter(suf[0]);
        else off += 1 + (uint32_t)nLetter(suf[0]) * 25 + (suf.size() == 2 ? 1 + (uint32_t)nLetter(suf[1]) : 0);
    }
    icao = kNBase + off;
    return icao <= kNLast;
}

// Canada: C-FAAA .. C-FZZZ then C-GAAA .. C-GZZZ in order from C00001.
bool adsbCanadaFromIcao(uint32_t icao, std::string& reg) {
    icao &= 0xFFFFFF;
    if (icao < 0xC00001 || icao > 0xC00000 + 2 * 17576) return false;
    uint32_t i = icao - 0xC00001;
    const char first = i >= 17576 ? 'G' : 'F';
    i %= 17576;
    reg = std::string("C-") + first + (char)('A' + i / 676) + (char)('A' + i / 26 % 26) + (char)('A' + i % 26);
    return true;
}
bool adsbIcaoFromCanada(const std::string& s, uint32_t& icao) {
    if (s.size() != 6 || s.compare(0, 2, "C-") != 0 || (s[2] != 'F' && s[2] != 'G')) return false;
    for (int k = 3; k < 6; k++) if (s[k] < 'A' || s[k] > 'Z') return false;
    icao = 0xC00001 + (s[2] == 'G' ? 17576 : 0) + (uint32_t)(s[3] - 'A') * 676 + (uint32_t)(s[4] - 'A') * 26 + (uint32_t)(s[5] - 'A');
    return true;
}

// Australia: VH-AAA .. VH-ZZZ from 7C0000 on
bool adsbAustraliaFromIcao(uint32_t icao, std::string& reg) {
    icao &= 0xFFFFFF;
    if (icao < 0x7C0000 || icao >= 0x7C0000 + 17576) return false;
    const uint32_t i = icao - 0x7C0000;
    reg = std::string("VH-") + (char)('A' + i / 676) + (char)('A' + i / 26 % 26) + (char)('A' + i % 26);
    return true;
}
bool adsbIcaoFromAustralia(const std::string& s, uint32_t& icao) {
    if (s.size() != 6 || s.compare(0, 3, "VH-") != 0) return false;
    for (int k = 3; k < 6; k++) if (s[k] < 'A' || s[k] > 'Z') return false;
    icao = 0x7C0000 + (uint32_t)(s[3] - 'A') * 676 + (uint32_t)(s[4] - 'A') * 26 + (uint32_t)(s[5] - 'A');
    return true;
}

std::string adsbAlgorithmicRegistration(uint32_t icao) {
    std::string r;
    if (adsbNNumberFromIcao(icao, r) || adsbCanadaFromIcao(icao, r) || adsbAustraliaFromIcao(icao, r)) return r;
    return "";
}

std::string adsbCategoryText(const std::string& c) {
    static const struct { const char* c; const char* t; } k[] = {
        {"A1", "Light (< 15 500 lb)"}, {"A2", "Small (15 500 - 75 000 lb)"}, {"A3", "Large (75 000 - 300 000 lb)"}, {"A4", "High vortex large (B-757)"},
        {"A5", "Heavy (> 300 000 lb)"}, {"A6", "High performance"}, {"A7", "Rotorcraft"}, {"B1", "Glider / sailplane"}, {"B2", "Lighter-than-air"},
        {"B3", "Parachutist"}, {"B4", "Ultralight"}, {"B6", "Unmanned aircraft"}, {"B7", "Space vehicle"},
        {"C1", "Surface emergency vehicle"}, {"C2", "Surface service vehicle"}, {"C3", "Obstacle"},
    };
    for (const auto& e : k) if (c == e.c) return e.t;
    return "";
}

// ---------------------------------------------------------------- the user's database

static std::vector<std::string> splitFields(const std::string& line, char sep, bool& ok) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (c == '"') { if (q && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; } else q = !q; }
        else if (c == sep && !q) { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    ok = !q;   // an unterminated quote: not a usable line
    v.push_back(cur);
    return v;
}
static std::string trim(std::string s) { return dect2::str::trimSpace(s); }

size_t AdsbRegDb::loadText(const std::string& text, size_t* skipped) {
    size_t n = 0, bad = 0;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t sc = line.find(';'), cm = line.find(',');
        const char sep = sc != std::string::npos && (cm == std::string::npos || sc < cm) ? ';' : ',';
        bool ok = true;
        std::vector<std::string> f = splitFields(line, sep, ok);
        if (first) {
            first = false;
            std::string h = trim(f[0]);
            for (char& c : h) c = (char)tolower((unsigned char)c);
            if (h == "icao" || h == "icao24" || h == "hex") continue;   // the header line
        }
        const std::string hex = trim(f[0]);
        char* end = nullptr;
        const unsigned long v = strtoul(hex.c_str(), &end, 16);
        if (!ok || f.size() < 2 || hex.empty() || hex.size() > 6 || !end || *end || !isxdigit((unsigned char)hex[0])) { bad++; continue; }
        AdsbRegEntry e;
        e.registration = trim(f[1]);
        if (f.size() > 2) e.type = trim(f[2]);
        if (sep == ';') { if (f.size() > 8) e.operatorName = trim(f[8]); }
        else if (f.size() > 3) e.operatorName = trim(f[3]);
        if (e.registration.empty() && e.type.empty() && e.operatorName.empty()) { bad++; continue; }
        map_[(uint32_t)v] = std::move(e);
        n++;
    }
    if (skipped) *skipped = bad;
    return n;
}

bool AdsbRegDb::loadFile(const std::string& path, size_t* count, size_t* skipped) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const size_t n = loadText(ss.str(), skipped);
    if (count) *count = n;
    return true;
}

const AdsbRegEntry* AdsbRegDb::find(uint32_t icao) const {
    auto it = map_.find(icao & 0xFFFFFF);
    return it == map_.end() ? nullptr : &it->second;
}

std::string AdsbRegDb::registration(uint32_t icao) const {
    if (const AdsbRegEntry* e = find(icao)) if (!e->registration.empty()) return e->registration;
    return adsbAlgorithmicRegistration(icao);
}

} // namespace dect2
