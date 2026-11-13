// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF channel plan: see marinevhf_channels.h. The table is data/bandplans/marine_vhf_channels.csv, compiled in through the generated
// marinevhf_data.inc (CMakeLists.txt).
#include "dect2/marinevhf/marinevhf_channels.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "marinevhf_data.inc"

namespace dect2 {
namespace marinevhf {

namespace {

const char* const kCodes[kRegionCount] = {"INT", "EUR", "BAL", "USA", "CAN"};
const char* const kNames[kRegionCount] = {"International (ITU)", "Europe", "Baltic Sea", "USA", "Canada"};

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

std::vector<std::string> splitSemi(const std::string& line) {
    std::vector<std::string> f;
    size_t a = 0;
    while (true) {
        const size_t b = line.find(';', a);
        if (b == std::string::npos) { f.push_back(line.substr(a)); break; }
        f.push_back(line.substr(a, b - a));
        a = b + 1;
    }
    return f;
}

unsigned flagsFrom(const std::string& s) {
    unsigned f = 0;
    for (char c : s) switch (c) {
        case 'd': f |= kDistress; break;
        case 'c': f |= kCalling; break;
        case 's': f |= kSar; break;
        case 'b': f |= kBridge; break;
        case 'p': f |= kPort; break;
        case 'i': f |= kIntership; break;
        case 'e': f |= kPublic; break;
        case 'm': f |= kSafetyBroadcast; break;
        case 'g': f |= kGovernment; break;
        case 'l': f |= kLowPower; break;
        default: break;
    }
    return f;
}

struct Plans {
    std::vector<Channel> ch[kRegionCount];
    Plans() {
        std::vector<CsvRow> rows;
        marineVhfParseCsv(marineVhfCsvText(), rows, nullptr);
        for (auto& r : rows) ch[r.region].push_back(r.channel);
        for (auto& v : ch) std::stable_sort(v.begin(), v.end(), [](const Channel& a, const Channel& b) {
            return a.number != b.number ? a.number < b.number : a.suffix < b.suffix;
        });
    }
};

const Plans& plans() { static const Plans p; return p; }

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

const char* regionCode(Region r) { return (unsigned)r < kRegionCount ? kCodes[r] : ""; }
const char* regionName(Region r) { return (unsigned)r < kRegionCount ? kNames[r] : ""; }

bool parseRegion(const std::string& code, Region& r) {
    const std::string u = upper(code);
    for (int i = 0; i < kRegionCount; i++) if (u == kCodes[i]) { r = (Region)i; return true; }
    return false;
}

std::string Channel::label() const {
    std::string s = std::to_string(number);
    if (suffix) s += suffix;
    return s;
}

std::string Channel::title() const { return label() + " " + name; }

std::vector<double> Channel::listenMhz() const {
    std::vector<double> v{shipMhz};
    if (duplex()) v.push_back(coastMhz);
    return v;
}

const std::vector<Channel>& channels(Region r) {
    static const std::vector<Channel> none;
    return (unsigned)r < kRegionCount ? plans().ch[r] : none;
}

const Channel* findChannel(Region r, int number, char suffix) {
    for (const Channel& c : channels(r)) if (c.number == number && c.suffix == suffix) return &c;
    return nullptr;
}

const Channel* findLabel(Region r, const std::string& text) {
    std::string t = lower(text);
    // drop words like "vhf", "ch", "ch.", "channel" and spaces
    std::string s;
    for (size_t i = 0; i < t.size();) {
        if (t.compare(i, 7, "channel") == 0) { i += 7; continue; }
        if (t.compare(i, 3, "vhf") == 0) { i += 3; continue; }
        if (t.compare(i, 2, "ch") == 0) { i += 2; continue; }
        if (std::isalnum((unsigned char)t[i])) s += t[i];
        i++;
    }
    if (s.empty() || !std::isdigit((unsigned char)s[0])) return nullptr;
    size_t k = 0;
    int n = 0;
    while (k < s.size() && std::isdigit((unsigned char)s[k])) { n = n * 10 + (s[k] - '0'); k++; if (n > 9999) return nullptr; }
    char suf = 0;
    if (k < s.size()) {
        if (k + 1 != s.size() || (s[k] != 'a' && s[k] != 'b')) return nullptr;
        suf = (char)std::toupper((unsigned char)s[k]);
    }
    if (const Channel* c = findChannel(r, n, suf)) return c;
    if (!suf) for (const Channel& c : channels(r)) if (c.number == n) return &c;   // "22" where the region has only 22A
    return nullptr;
}

const Channel* findFrequency(Region r, double mhz, double tolMhz, bool* coastSide) {
    const Channel* best = nullptr;
    bool bestCoast = false;
    for (const Channel& c : channels(r)) {
        if (near(c.shipMhz, mhz, tolMhz)) {
            // prefer the plain number, then A, over a B variant that has the same frequency (a "B" simplex channel sits on a coast frequency)
            if (!best || (best->suffix > c.suffix)) { best = &c; bestCoast = false; }
        } else if (c.duplex() && near(c.coastMhz, mhz, tolMhz)) {
            if (!best) { best = &c; bestCoast = true; }
        }
    }
    if (best) {
        // a coast-side match of a duplex channel yields to a simplex "B" channel on exactly that frequency (87B, not 87)
        for (const Channel& c : channels(r)) if (!c.duplex() && c.suffix == 'B' && near(c.shipMhz, mhz, tolMhz)) { best = &c; bestCoast = false; break; }
    }
    if (coastSide) *coastSide = bestCoast;
    return best;
}

std::vector<double> allFrequencies(Region r) {
    std::vector<double> f;
    for (const Channel& c : channels(r)) for (double x : c.listenMhz()) f.push_back(x);
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end(), [](double a, double b) { return near(a, b, 0.0001); }), f.end());
    return f;
}

std::vector<double> voiceFrequencies(Region r) {
    std::vector<double> f;
    for (const Channel& c : channels(r)) if (c.voice()) for (double x : c.listenMhz()) f.push_back(x);
    std::sort(f.begin(), f.end());
    f.erase(std::unique(f.begin(), f.end(), [](double a, double b) { return near(a, b, 0.0001); }), f.end());
    return f;
}

const Channel* stepChannel(Region r, const Channel* cur, int dir) {
    std::vector<const Channel*> v;
    for (const Channel& c : channels(r)) if (c.voice()) v.push_back(&c);
    if (v.empty()) return nullptr;
    if (!cur) return dir >= 0 ? v.front() : v.back();
    size_t i = 0;
    bool found = false;
    for (; i < v.size(); i++) if (v[i]->number == cur->number && v[i]->suffix == cur->suffix) { found = true; break; }
    if (!found) {   // a channel the list does not hold (a data channel): the next voice channel by number
        for (i = 0; i < v.size(); i++) if (v[i]->number > cur->number) break;
        if (dir >= 0) return v[i % v.size()];
        return v[(i + v.size() - 1) % v.size()];
    }
    const size_t n = v.size();
    return dir >= 0 ? v[(i + 1) % n] : v[(i + n - 1) % n];
}

int channelFromDscText(const std::string& text) {
    // the first run of digits that follows "ch" (or the first run at all)
    const std::string t = lower(text);
    if (t.find("mf/hf") != std::string::npos || t.find("khz") != std::string::npos || t.find("mhz") != std::string::npos) return 0;   // not a VHF channel
    size_t p = t.find("ch");
    if (p == std::string::npos) p = 0;
    while (p < t.size() && !std::isdigit((unsigned char)t[p])) p++;
    if (p >= t.size()) return 0;
    long n = 0;
    while (p < t.size() && std::isdigit((unsigned char)t[p])) { n = n * 10 + (t[p] - '0'); p++; if (n > 99999) return 0; }
    n %= 100;
    return n >= 1 && n <= 88 ? (int)n : 0;
}

double ituShipMhz(int n) {
    if (n >= 1 && n <= 28) return std::round((156.050 + 0.05 * (n - 1)) * 1000) / 1000;
    if (n >= 60 && n <= 88) return std::round((156.025 + 0.05 * (n - 60)) * 1000) / 1000;
    return 0;
}

size_t marineVhfParseCsv(const std::string& text, std::vector<CsvRow>& out, std::vector<std::string>* errors) {
    size_t kept = 0, pos = 0, lineNo = 0;
    bool header = true;
    auto err = [&](const std::string& m) { if (errors) errors->push_back("line " + std::to_string(lineNo) + ": " + m); };
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (header) { header = false; continue; }
        const auto f = splitSemi(line);
        if (f.size() != 10) { err("expected 10 fields"); continue; }
        CsvRow row;
        if (!parseRegion(f[0], row.region)) { err("unknown region " + f[0]); continue; }
        Channel& c = row.channel;
        c.number = std::atoi(f[1].c_str());
        if (c.number < 1 || c.number > 88) { err("channel number out of range"); continue; }
        if (f[2].size() > 1 || (!f[2].empty() && f[2][0] != 'A' && f[2][0] != 'B')) { err("bad suffix"); continue; }
        c.suffix = f[2].empty() ? 0 : f[2][0];
        c.shipMhz = std::atof(f[3].c_str());
        c.coastMhz = std::atof(f[4].c_str());
        if (c.shipMhz < 156.0 || c.shipMhz > 162.1 || c.coastMhz < 156.0 || c.coastMhz > 162.1) { err("frequency out of range"); continue; }
        if (f[5] == "V") c.use = Use::Voice;
        else if (f[5] == "D") c.use = Use::Dsc;
        else if (f[5] == "A") c.use = Use::Ais;
        else if (f[5] == "L") c.use = Use::LongRangeAis;
        else { err("unknown use " + f[5]); continue; }
        c.flags = flagsFrom(f[6]);
        c.powerW = std::atof(f[7].c_str());
        c.name = f[8];
        c.purpose = f[9];
        out.push_back(std::move(row));
        kept++;
    }
    return kept;
}

const std::string& marineVhfCsvText() { static const std::string s(kMarineVhfData); return s; }

} // namespace marinevhf
} // namespace dect2
