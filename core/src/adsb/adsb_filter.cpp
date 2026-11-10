// ADS-B filter and search: see adsb_filter.h.
#include "dect2/adsb/adsb_filter.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace dect2 {

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::vector<std::string> split(const std::string& s, const char* seps) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (std::strchr(seps, c)) { if (!cur.empty()) v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
}  // namespace

bool adsbMilitaryAddress(uint32_t icao) {
    icao &= 0xFFFFFF;
    return (icao >= 0xAE0000 && icao <= 0xAFFFFF) || (icao >= 0x43C000 && icao <= 0x43CFFF);
}

bool adsbFilterMatch(const AdsbFilter& f, const AdsbAircraft& ac, const AdsbFilterInfo& info) {
    if (f.ground == 1 && !ac.ground) return false;
    if (f.ground == 2 && ac.ground) return false;
    if (f.altMinFt >= 0 || f.altMaxFt >= 0) {
        if (!ac.ground && !ac.hasAlt) return false;
        const int alt = ac.ground ? 0 : ac.altFt;
        if (f.altMinFt >= 0 && alt < f.altMinFt) return false;
        if (f.altMaxFt >= 0 && alt > f.altMaxFt) return false;
    }
    if (!f.squawks.empty()) {
        if (!ac.hasSquawk) return false;
        char sq[8];
        std::snprintf(sq, sizeof sq, "%04d", ac.squawk);
        bool any = false;
        for (const std::string& w : split(f.squawks, ", ;")) {
            if (!w.empty() && w.back() == '*') { if (std::string(sq).compare(0, w.size() - 1, w, 0, w.size() - 1) == 0) any = true; }
            else if (w == sq) any = true;
        }
        if (!any) return false;
    }
    if (!f.categories.empty()) {
        bool any = false;
        const std::string c = lower(ac.category);
        for (const std::string& w0 : split(f.categories, ", ;")) {
            const std::string w = lower(w0);
            if (c.empty()) continue;
            if (w.size() == 1 ? c[0] == w[0] : c == w) any = true;
        }
        if (!any) return false;
    }
    if (f.militaryOnly && !adsbMilitaryAddress(ac.icao)) return false;
    if (!f.text.empty()) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "%06x", ac.icao & 0xFFFFFF);
        const std::string hay = std::string(hex) + "\n" + lower(ac.callsign) + "\n" + lower(info.registration) + "\n" + lower(info.type) + "\n" + lower(info.operatorName);
        for (const std::string& w : split(lower(f.text), " \t")) if (hay.find(w) == std::string::npos) return false;
    }
    return true;
}

std::string adsbFilterToText(const AdsbFilter& f) {
    auto clean = [](std::string s) { for (char& c : s) if (c == ';' || c == '=' || c == '\n' || c == '\r') c = ' '; return s; };
    std::string o = "text=" + clean(f.text) + ";min=" + std::to_string(f.altMinFt) + ";max=" + std::to_string(f.altMaxFt) + ";sq=" + clean(f.squawks) + ";cat=" + clean(f.categories) +
                    ";ground=" + std::to_string(f.ground) + ";mil=" + (f.militaryOnly ? "1" : "0");
    return o;
}

AdsbFilter adsbFilterFromText(const std::string& s) {
    AdsbFilter f;
    for (const std::string& kv : split(s, ";")) {
        const size_t e = kv.find('=');
        if (e == std::string::npos) continue;
        const std::string k = kv.substr(0, e), v = kv.substr(e + 1);
        if (k == "text") f.text = v;
        else if (k == "min") f.altMinFt = std::max(-1, std::atoi(v.c_str()));
        else if (k == "max") f.altMaxFt = std::max(-1, std::atoi(v.c_str()));
        else if (k == "sq") f.squawks = v;
        else if (k == "cat") f.categories = v;
        else if (k == "ground") { const int g = std::atoi(v.c_str()); f.ground = g >= 0 && g <= 2 ? g : 0; }
        else if (k == "mil") f.militaryOnly = v == "1";
    }
    return f;
}

}  // namespace dect2
