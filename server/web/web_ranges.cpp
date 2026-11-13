// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "web_ranges.h"
#include "server_config.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace onair_web {

// ---------------------------------------------------------------- classes
RangeClass rangeClassOf(UserClass c) { return c == UserClass::Member ? RangeClass::Member : c == UserClass::Invited ? RangeClass::Invite : RangeClass::Open; }
const char* rangeClassName(RangeClass c) { return c == RangeClass::Member ? "member" : c == RangeClass::Invite ? "invite" : "open"; }
bool rangeClassFromName(const std::string& s, RangeClass& c) {
    if (s == "open" || s == "public") c = RangeClass::Open;
    else if (s == "member" || s == "password") c = RangeClass::Member;
    else if (s == "invite" || s == "invited") c = RangeClass::Invite;
    else return false;
    return true;
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------- parsing
bool parseFreqHz(const std::string& text, double defaultMul, double& hz, bool& hadUnit, std::string& err) {
    std::string t;
    for (char c : text) if (!std::isspace((unsigned char)c)) t += (char)std::tolower((unsigned char)c);
    hadUnit = false;
    if (t.empty()) { err = "a frequency is missing"; return false; }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) { err = "'" + text + "' is not a frequency"; return false; }
    if (!std::isfinite(v)) { err = "'" + text + "' is not a finite number"; return false; }
    const std::string unit = end;
    double mul = defaultMul;
    if (!unit.empty()) {
        hadUnit = true;
        if (unit == "hz") mul = 1;
        else if (unit == "khz" || unit == "k") mul = 1e3;
        else if (unit == "mhz" || unit == "m") mul = 1e6;
        else if (unit == "ghz" || unit == "g") mul = 1e9;
        else { err = "unknown unit '" + unit + "' (use Hz, kHz, MHz or GHz)"; return false; }
    }
    const double h = v * mul;
    if (v < 0 || h < 0) { err = "a frequency cannot be negative"; return false; }
    if (!(h <= kMaxRangeHz)) { err = "'" + text + "' is far too high (the limit is 1000 GHz)"; return false; }
    hz = std::round(h * 1000.0) / 1000.0;
    return true;
}

bool parseRangeSpec(const std::string& lineIn, RangeRule& out, std::string& err) {
    const std::string line = trim(lineIn);
    if (line.empty()) { err = "empty range"; return false; }
    // the note is everything after "note=" (it may contain commas)
    std::string head = line, note;
    {
        const std::string low = lower(line);
        size_t p = std::string::npos;
        for (size_t i = 0; i + 5 <= low.size(); i++)
            if (low.compare(i, 5, "note=") == 0 && (i == 0 || low[i - 1] == ',' || low[i - 1] == ' ')) { p = i; break; }
        if (p != std::string::npos) { head = line.substr(0, p); note = trim(line.substr(p + 5)); }
    }
    std::vector<std::string> f;
    {
        std::string cur;
        for (char c : head) { if (c == ',') { f.push_back(trim(cur)); cur.clear(); } else cur += c; }
        f.push_back(trim(cur));
        while (f.size() > 1 && f.back().empty()) f.pop_back();   // "a-b," before a note
    }
    const std::string rg = f[0];
    // the separator: the first '-' after the first character that does not follow an exponent 'e'
    size_t sep = std::string::npos;
    for (size_t i = 1; i < rg.size(); i++)
        if (rg[i] == '-' && std::tolower((unsigned char)rg[i - 1]) != 'e') { sep = i; break; }
    if (!rg.empty() && rg[0] == '-') { err = "a frequency cannot be negative"; return false; }
    if (sep == std::string::npos) { err = "write a range as <start>-<end>, e.g. 144-146MHz"; return false; }
    double endHz = 0, startHz = 0;
    bool endUnit = false, startUnit = false;
    if (!parseFreqHz(rg.substr(sep + 1), 1, endHz, endUnit, err)) return false;
    double endMul = 1;
    if (endUnit) {   // the start takes the end's unit when it has none
        const std::string tail = lower(rg.substr(sep + 1));
        const size_t u = tail.find_first_not_of("0123456789.+- e");
        const std::string unit = u == std::string::npos ? "" : trim(tail.substr(u));
        endMul = (unit == "khz" || unit == "k") ? 1e3 : (unit == "mhz" || unit == "m") ? 1e6 : (unit == "ghz" || unit == "g") ? 1e9 : 1;
    }
    if (!parseFreqHz(rg.substr(0, sep), endMul, startHz, startUnit, err)) return false;
    if (startHz > endHz) { err = "the start is above the end (write the lower frequency first)"; return false; }
    if (startHz == endHz) { err = "an empty range (start equals end)"; return false; }
    RangeRule r;
    r.r.lo = startHz; r.r.hi = endHz;
    unsigned modes = 0, classes = 0;
    for (size_t i = 1; i < f.size(); i++) {
        const std::string w = lower(f[i]);
        Demod d; RangeClass c;
        if (w.empty()) continue;
        if (demodFromName(w, d)) modes |= 1u << (unsigned)d;
        else if (rangeClassFromName(w, c)) classes |= classBit(c);
        else if (w == "all") {}
        else { err = "unknown word '" + f[i] + "' (a mode, open, member or invite)"; return false; }
    }
    r.r.modes = modes ? modes : kAllModes;
    r.classes = classes ? classes : kAllClasses;
    for (char c : note) if ((unsigned char)c < 32) { note.clear(); break; }
    if (note.size() > 160) note.resize(160);
    r.note = note;
    r.r.note = note;
    out = r;
    return true;
}

static std::string fmtHz(double h) {
    char b[48];
    if (h == std::floor(h) && h < 1e15) snprintf(b, sizeof b, "%.0f", h);
    else { snprintf(b, sizeof b, "%.3f", h); std::string s = b; while (!s.empty() && s.back() == '0') s.pop_back(); if (!s.empty() && s.back() == '.') s.pop_back(); return s; }
    return b;
}

std::string formatRangeSpec(const RangeRule& r) {
    std::string s = fmtHz(r.r.lo) + "-" + fmtHz(r.r.hi);
    if (r.r.modes != kAllModes) for (int d = 0; d < (int)Demod::Count; d++) if (r.r.modes & (1u << d)) s += std::string(",") + demodName((Demod)d);
    if (r.classes != kAllClasses) for (int c = 0; c < 3; c++) if (r.classes & (1u << c)) s += std::string(",") + rangeClassName((RangeClass)c);
    if (!r.note.empty()) s += ",note=" + r.note;
    return s;
}

// ---------------------------------------------------------------- set operations
std::vector<FreqRange> mergeRanges(std::vector<FreqRange> v) {
    std::sort(v.begin(), v.end(), [](const FreqRange& a, const FreqRange& b) { return a.lo < b.lo; });
    std::vector<FreqRange> o;
    for (auto& r : v) {
        if (!(r.hi >= r.lo)) continue;   // a reversed or NaN entry never gets in
        if (!o.empty() && r.lo <= o.back().hi + kRangeEpsHz) {
            o.back().hi = std::max(o.back().hi, r.hi);
            o.back().modes |= r.modes;
        } else o.push_back(r);
    }
    return o;
}

std::vector<FreqRange> rangesForMode(const std::vector<FreqRange>& v, Demod d) {
    std::vector<FreqRange> o;
    for (const auto& r : v) if (r.modes & (1u << (unsigned)d)) o.push_back(r);
    return mergeRanges(o);
}

std::vector<FreqRange> subtractRanges(const std::vector<FreqRange>& a, const std::vector<FreqRange>& b) {
    std::vector<FreqRange> o;
    for (FreqRange cur : a) {
        bool gone = false;
        for (const auto& x : b) {
            if (x.hi < cur.lo || x.lo > cur.hi) continue;
            if (x.lo > cur.lo) { FreqRange left = cur; left.hi = x.lo; o.push_back(left); }   // [cur.lo, x.lo] stays (x.lo itself is blocked: the callers use a tolerance)
            if (x.hi >= cur.hi) { gone = true; break; }
            cur.lo = x.hi;
        }
        if (!gone && cur.hi > cur.lo) o.push_back(cur);
    }
    return o;
}

// ---------------------------------------------------------------- evaluation
double guardMarginHz(double bw) {
    if (!std::isfinite(bw) || bw < 0) bw = 0;
    return std::min(5000.0, std::max(200.0, 0.05 * bw));
}

const char* verdictText(RangeVerdict v) {
    switch (v) {
    case RangeVerdict::Ok: return "";
    case RangeVerdict::NotAllowed: return "the host allows listening only in certain frequency ranges, and this is not one of them";
    case RangeVerdict::ModeNotAllowed: return "the host does not allow this mode in this frequency range";
    case RangeVerdict::Blocked: return "the host has blocked this frequency range";
    case RangeVerdict::OutsideCapture: return "the passband reaches the edge of the capture, where the host's ranges cannot be kept (tune nearer the middle)";
    case RangeVerdict::Invalid: return "not a usable frequency or passband";
    }
    return "";
}

static bool containsSpan(const std::vector<FreqRange>& merged, double lo, double hi) {
    for (const auto& e : merged) if (e.lo - kRangeEpsHz <= lo && hi <= e.hi + kRangeEpsHz) return true;
    return false;
}

RangeVerdict checkPassband(const RangeView& v, Demod demod, double f, double low, double high, double centre, double rate) {
    if (!std::isfinite(f) || !std::isfinite(low) || !std::isfinite(high) || !(low < high) || f < 0 || f > kMaxRangeHz) return RangeVerdict::Invalid;
    if (!v.active()) return RangeVerdict::Ok;
    const double g = guardMarginHz(high - low), lo = f + low - g, hi = f + high + g;
    if (rate > 0) {
        const double u = rate * 0.48;
        if (lo < centre - u || hi > centre + u) return RangeVerdict::OutsideCapture;
    }
    for (const auto& b : v.block)
        if ((b.modes & (1u << (unsigned)demod)) && b.lo <= hi && b.hi >= lo) return RangeVerdict::Blocked;
    if (!v.restricted) return RangeVerdict::Ok;
    if (containsSpan(rangesForMode(v.allow, demod), lo, hi)) return RangeVerdict::Ok;
    return containsSpan(mergeRanges(v.allow), lo, hi) ? RangeVerdict::ModeNotAllowed : RangeVerdict::NotAllowed;
}

RangeVerdict checkWindow(const RangeView& v, double lo, double hi) {
    if (!std::isfinite(lo) || !std::isfinite(hi) || !(lo < hi)) return RangeVerdict::Invalid;
    if (!v.active()) return RangeVerdict::Ok;
    for (const auto& b : v.block) if (b.lo <= hi && b.hi >= lo) return RangeVerdict::Blocked;   // any mode: a decoder or the capture shows everything in the window
    if (!v.restricted) return RangeVerdict::Ok;
    return containsSpan(mergeRanges(v.allow), lo, hi) ? RangeVerdict::Ok : RangeVerdict::NotAllowed;
}

bool captureVisible(const RangeView& v, double centre, double rate) {
    if (!v.active()) return true;
    if (!(rate > 0)) return false;
    return checkWindow(v, centre - rate * 0.5, centre + rate * 0.5) == RangeVerdict::Ok;
}

bool snapFreq(const RangeView& v, Demod demod, double freq, double low, double high, double centre, double rate, double& out) {
    if (!std::isfinite(freq) || !std::isfinite(low) || !std::isfinite(high) || !(low < high)) return false;
    if (checkPassband(v, demod, freq, low, high, centre, rate) == RangeVerdict::Ok) { out = freq; return true; }
    const double g = guardMarginHz(high - low);
    std::vector<FreqRange> areas = v.restricted ? rangesForMode(v.allow, demod) : std::vector<FreqRange>{FreqRange{0, kMaxRangeHz}};
    if (rate > 0) {   // keep to the usable capture
        const double u = rate * 0.48;
        for (auto& a : areas) { a.lo = std::max(a.lo, centre - u + 0.01); a.hi = std::min(a.hi, centre + u - 0.01); }   // a little inside: no rounding trouble on the edge
    }
    bool have = false;
    double best = 0;
    auto consider = [&](double c) {
        if (checkPassband(v, demod, c, low, high, centre, rate) != RangeVerdict::Ok) return;
        if (!have || std::fabs(c - freq) < std::fabs(best - freq)) { best = c; have = true; }
    };
    for (const auto& a : areas) {
        if (!(a.hi > a.lo)) continue;
        const double fmin = a.lo - low + g, fmax = a.hi - high - g;
        if (fmin > fmax) continue;
        consider(std::clamp(freq, fmin, fmax));
        for (const auto& b : v.block) {   // just outside each block that may be in the way
            if (!(b.modes & (1u << (unsigned)demod))) continue;
            consider(std::clamp(b.hi + 0.01 - low + g, fmin, fmax));
            consider(std::clamp(b.lo - 0.01 - high - g, fmin, fmax));
        }
        consider(fmin);
        consider(fmax);
    }
    if (have) out = best;
    return have;
}

size_t maskSpectrumRow(std::vector<float>& db, double start, double stop, const RangeView& v) {
    const size_t n = db.size();
    if (n == 0 || !std::isfinite(start) || !std::isfinite(stop) || !(stop > start) || !v.active()) return 0;
    std::vector<FreqRange> vis = v.restricted ? mergeRanges(v.allow) : std::vector<FreqRange>{FreqRange{0, kMaxRangeHz}};
    std::vector<FreqRange> hide;
    for (const auto& b : v.block) if (b.modes == kAllModes) hide.push_back(b);   // a mode-limited block is about listening only
    vis = subtractRanges(vis, mergeRanges(hide));
    const double w = (stop - start) / (double)n, tol = 1e-6 * w + kRangeEpsHz;
    std::vector<char> bad(n, 1);
    for (size_t i = 0; i < n; i++) {
        const double a = start + (double)i * w, b = a + w;
        for (const auto& r : vis) if (r.lo - tol <= a && b <= r.hi + tol) { bad[i] = 0; break; }
    }
    std::vector<char> hid(bad);
    for (size_t i = 0; i < n; i++) if (bad[i]) { if (i > 0) hid[i - 1] = 1; if (i + 1 < n) hid[i + 1] = 1; }
    std::vector<float> seen;
    for (size_t i = 0; i < n; i++) if (!hid[i]) seen.push_back(db[i]);
    float fill = -150.f;
    if (!seen.empty()) {
        const size_t k = seen.size() / 5;
        std::nth_element(seen.begin(), seen.begin() + (long)k, seen.end());
        fill = seen[k];
    }
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) if (hid[i]) { db[i] = fill; cnt++; }
    return cnt;
}

// ---------------------------------------------------------------- presets
// Typical band plans, NOT authoritative: allocations differ by country and region (ITU regions 1, 2 and 3), change over time and may be
// narrower where you are. The host is responsible for what the preset allows or blocks; check the national plan. Frequencies in Hz.
namespace {
struct PR { double lo, hi; const char* modes; const char* note; };
const PR kHamHf[] = {
    {1.8e6, 2.0e6, "", "160 m"},         {3.5e6, 4.0e6, "", "80 m (3.5-3.8 in Region 1, 3.5-4.0 in Region 2)"},
    {5.3515e6, 5.3665e6, "", "60 m (channels differ by country)"}, {7.0e6, 7.3e6, "", "40 m (7.0-7.2 in Regions 1 and 3)"},
    {10.1e6, 10.15e6, "", "30 m"},       {14.0e6, 14.35e6, "", "20 m"},       {18.068e6, 18.168e6, "", "17 m"},
    {21.0e6, 21.45e6, "", "15 m"},       {24.89e6, 24.99e6, "", "12 m"},      {28.0e6, 29.7e6, "", "10 m"},
};
const PR kHamVhfUhf[] = {
    {50.0e6, 54.0e6, "", "6 m (50-52 in much of Region 1)"}, {70.0e6, 70.5e6, "", "4 m (some Region 1 countries only)"},
    {144.0e6, 148.0e6, "", "2 m (144-146 in Region 1)"},     {222.0e6, 225.0e6, "", "1.25 m (Region 2 only)"},
    {420.0e6, 450.0e6, "", "70 cm (430-440 in Region 1)"},   {902.0e6, 928.0e6, "", "33 cm (Region 2 only)"},
    {1240.0e6, 1300.0e6, "", "23 cm"},
};
const PR kBroadcast[] = {
    {153e3, 279e3, "am", "long wave (Region 1)"},  {531e3, 1710e3, "am", "medium wave (Region 2: 530-1700)"},
    {2.3e6, 2.495e6, "am,usb,lsb", "120 m shortwave"}, {3.2e6, 3.4e6, "am,usb,lsb", "90 m"}, {3.9e6, 4.0e6, "am,usb,lsb", "75 m"},
    {4.75e6, 5.06e6, "am,usb,lsb", "60 m"},        {5.9e6, 6.2e6, "am,usb,lsb", "49 m"},      {7.2e6, 7.6e6, "am,usb,lsb", "41 m"},
    {9.4e6, 9.9e6, "am,usb,lsb", "31 m"},          {11.6e6, 12.1e6, "am,usb,lsb", "25 m"},    {13.57e6, 13.87e6, "am,usb,lsb", "22 m"},
    {15.1e6, 15.8e6, "am,usb,lsb", "19 m"},        {17.48e6, 17.9e6, "am,usb,lsb", "16 m"},   {18.9e6, 19.02e6, "am,usb,lsb", "15 m"},
    {21.45e6, 21.85e6, "am,usb,lsb", "13 m"},      {25.67e6, 26.1e6, "am,usb,lsb", "11 m"},
    {87.5e6, 108.0e6, "wfm,nfm", "FM broadcast (Japan 76-95, OIRT 65.8-74 not included)"},
};
const PR kAirband[] = {
    {118.0e6, 137.0e6, "am", "civil aircraft voice (8.33 / 25 kHz channels)"},
};
const PR kMarine[] = {
    {156.0e6, 162.025e6, "nfm", "marine VHF channels incl. AIS 161.975 / 162.025 MHz"},
};
const PR kWeatherSat[] = {
    {137.0e6, 138.0e6, "nfm,wfm", "NOAA APT / Meteor LRPT downlinks (137.1 .. 137.9 MHz)"},
    {400.15e6, 406.0e6, "nfm", "meteorological aids / radiosondes (400.15-406 MHz)"},
    {1670.0e6, 1710.0e6, "", "GOES / polar HRPT and LRIT region (1670-1710 MHz)"},
};
const PR kAll[] = {{0, kMaxRangeHz, "", "no limit"}};
// Blocks: services whose content is protected by law in most countries (typical positions only; the plan differs per country).
const PR kNoCellPs[] = {
    {380e6, 400e6, "", "TETRA / emergency services (EU 380-400 MHz)"},
    {410e6, 430e6, "", "TETRA / professional mobile radio (EU 410-430 MHz)"},
    {450e6, 470e6, "", "public safety and business radio, cellular (US 450-470 MHz, CDMA 450)"},
    {470e6, 512e6, "", "public safety T-band (US, parts of 470-512 MHz)"},
    {698e6, 806e6, "", "LTE 700 / US public safety broadband (700 MHz)"},
    {806e6, 960e6, "", "cellular (GSM 900, CDMA 850, LTE 800), trunked public safety (806-824 / 851-869 MHz US)"},
    {1710e6, 2170e6, "", "cellular (GSM 1800, PCS 1900, UMTS 2100)"},
    {2300e6, 2400e6, "", "LTE 2300"},
    {2500e6, 2690e6, "", "LTE 2600"},
    {3400e6, 3800e6, "", "5G mid-band"},
};
struct PresetDef { PresetInfo info; const PR* r; size_t n; };
#define PD(name, blk, desc, arr) {{name, blk, desc}, arr, sizeof(arr) / sizeof(arr[0])}
const PresetDef kPresets[] = {
    PD("ham_hf", false, "Amateur radio HF bands (typical Region 1/2/3 union; narrower in many countries)", kHamHf),
    PD("ham_vhf_uhf", false, "Amateur radio 6 m to 23 cm (typical; many bands are regional)", kHamVhfUhf),
    PD("broadcast_am_fm", false, "Broadcast LW/MW/SW and VHF FM (typical)", kBroadcast),
    PD("airband", false, "Civil aircraft voice 118-137 MHz (typical; navigation aids 108-118 MHz not included)", kAirband),
    PD("marine", false, "Marine VHF 156-162 MHz (typical)", kMarine),
    PD("weather_sat", false, "Weather satellite downlinks and radiosondes (typical)", kWeatherSat),
    PD("no_cellular_no_public_safety", true, "BLOCKS typical cellular and public-safety / trunked radio ranges (typical, not complete, differs by country)", kNoCellPs),
    PD("all", false, "Everything (no restriction for the class it is given to)", kAll),
};
#undef PD
} // namespace

const std::vector<PresetInfo>& rangePresets() {
    static const std::vector<PresetInfo> v = [] { std::vector<PresetInfo> o; for (const auto& p : kPresets) o.push_back(p.info); return o; }();
    return v;
}

bool presetRules(const std::string& nameIn, unsigned classes, std::vector<RangeRule>& out, bool& isBlock) {
    const std::string name = lower(nameIn);
    for (const auto& p : kPresets) {
        if (name != p.info.name) continue;
        isBlock = p.info.block;
        for (size_t i = 0; i < p.n; i++) {
            RangeRule r;
            r.r.lo = p.r[i].lo; r.r.hi = p.r[i].hi;
            unsigned m = 0;
            std::string w;
            for (const char* c = p.r[i].modes;; c++) {
                if (*c == ',' || *c == 0) { Demod d; if (!w.empty() && demodFromName(w, d)) m |= 1u << (unsigned)d; w.clear(); if (!*c) break; }
                else w += *c;
            }
            r.r.modes = m ? m : kAllModes;
            r.classes = classes;
            r.note = p.r[i].note;
            r.r.note = r.note;
            out.push_back(r);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- configuration
static std::vector<std::string> splitCommas(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) { if (c == ',' || c == ' ' || c == ';') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

bool WebRanges::addPreset(const std::string& tokenIn, std::string& err) {
    std::string token = lower(tokenIn), name = token;
    unsigned classes = kAllClasses;
    const size_t at = token.find('@');
    if (at != std::string::npos) {
        name = token.substr(0, at);
        classes = 0;
        std::string w;
        for (size_t i = at + 1; i <= token.size(); i++) {
            if (i == token.size() || token[i] == '+') { RangeClass c; if (!rangeClassFromName(w, c)) { err = "unknown class '" + w + "' in preset " + tokenIn; return false; } classes |= classBit(c); w.clear(); }
            else w += token[i];
        }
    }
    std::vector<RangeRule> tmp;
    bool blk = false;
    if (!presetRules(name, classes, tmp, blk)) { err = "unknown preset '" + name + "'"; return false; }
    for (auto& p : presets_) if (p.name == name) { p.classes |= classes; return true; }
    presets_.push_back(PresetUse{name, classes});
    return true;
}

void WebRanges::load(const onair_server::ConfigFile& c, std::vector<std::string>* warnings) {
    allow_.clear(); block_.clear(); presets_.clear();
    auto warn = [&](const std::string& w) { if (warnings) warnings->push_back(w); };
    for (const auto& l : c.getAll("web_allow_range")) {
        if (trim(l).empty()) continue;
        RangeRule r; std::string e;
        if (parseRangeSpec(l, r, e)) allow_.push_back(r); else warn("web_allow_range '" + l + "': " + e + " (line ignored)");
    }
    for (const auto& l : c.getAll("web_block_range")) {
        if (trim(l).empty()) continue;
        RangeRule r; std::string e;
        if (parseRangeSpec(l, r, e)) block_.push_back(r); else warn("web_block_range '" + l + "': " + e + " (line ignored)");
    }
    for (const auto& l : c.getAll("web_range_preset"))
        for (const auto& t : splitCommas(l)) { std::string e; if (!addPreset(t, e)) warn("web_range_preset: " + e + " (ignored)"); }
    const std::string a = lower(c.get("web_range_action", "refuse"));
    clamp_ = a == "clamp";
    if (a != "refuse" && a != "clamp") warn("web_range_action must be refuse or clamp (using refuse)");
    // a mistake must never open things up: a configured allow-list whose every line was bad would leave everything allowed, so say it loudly
    if (!c.getAll("web_allow_range").empty() && allow_.empty() && presets_.empty())
        warn("every web_allow_range line is invalid: listeners are NOT restricted. Fix the lines or remove them.");
    gen_++;
}

static void setLines(onair_server::ConfigFile& c, const std::string& key, const std::vector<std::string>& lines) {
    c.set(key, lines.empty() ? std::string() : lines[0]);
    for (size_t i = 1; i < lines.size(); i++) {
        std::string t = c.text();
        if (!t.empty() && t.back() != '\n') t += '\n';
        c.parse(t + key + "=" + lines[i] + "\n");
    }
}

std::vector<std::string> WebRanges::allowSpecs() const { std::vector<std::string> a; for (const auto& r : allow_) a.push_back(formatRangeSpec(r)); return a; }
std::vector<std::string> WebRanges::blockSpecs() const { std::vector<std::string> a; for (const auto& r : block_) a.push_back(formatRangeSpec(r)); return a; }
std::vector<std::string> WebRanges::presetSpecs() const {
    std::vector<std::string> p;
    {
    for (const auto& u : presets_) {
        std::string s = u.name;
        if (u.classes != kAllClasses) {
            s += "@";
            bool first = true;
            for (int k = 0; k < 3; k++) if (u.classes & (1u << k)) { s += (first ? "" : "+") + std::string(rangeClassName((RangeClass)k)); first = false; }
        }
        p.push_back(s);
    }
    }
    return p;
}

void WebRanges::save(onair_server::ConfigFile& c) const {
    setLines(c, "web_allow_range", allowSpecs());
    setLines(c, "web_block_range", blockSpecs());
    setLines(c, "web_range_preset", presetSpecs());
    c.set("web_range_action", clamp_ ? "clamp" : "refuse");
}

bool WebRanges::replace(const std::vector<std::string>& allow, const std::vector<std::string>& block, const std::vector<std::string>& presets,
                        const std::string& action, std::string& err) {
    WebRanges n;
    for (const auto& l : allow) { RangeRule r; if (!parseRangeSpec(l, r, err)) { err = "allow '" + l + "': " + err; return false; } n.allow_.push_back(r); }
    for (const auto& l : block) { RangeRule r; if (!parseRangeSpec(l, r, err)) { err = "block '" + l + "': " + err; return false; } n.block_.push_back(r); }
    for (const auto& l : presets) for (const auto& t : splitCommas(l)) if (!n.addPreset(t, err)) return false;
    const std::string a = lower(action.empty() ? (clamp_ ? "clamp" : "refuse") : action);
    if (a != "refuse" && a != "clamp") { err = "action must be refuse or clamp"; return false; }
    if (n.allow_.size() + n.block_.size() > 128) { err = "at most 128 ranges"; return false; }
    allow_ = n.allow_; block_ = n.block_; presets_ = n.presets_;
    clamp_ = a == "clamp";
    gen_++;
    return true;
}

void WebRanges::clear() { allow_.clear(); block_.clear(); presets_.clear(); gen_++; }

RangeView WebRanges::view(RangeClass cl) const {
    RangeView v;
    const unsigned bit = classBit(cl);
    auto note = [&](const std::string& n) { if (!n.empty() && std::find(v.notes.begin(), v.notes.end(), n) == v.notes.end()) v.notes.push_back(n); };
    for (const auto& r : allow_) if (r.classes & bit) { v.restricted = true; v.allow.push_back(r.r); note(r.note); }
    for (const auto& r : block_) if (r.classes & bit) { v.block.push_back(r.r); note(r.note); }
    for (const auto& u : presets_) {
        if (!(u.classes & bit)) continue;
        std::vector<RangeRule> rules;
        bool blk = false;
        if (!presetRules(u.name, kAllClasses, rules, blk)) continue;
        for (const auto& r : rules) { (blk ? v.block : v.allow).push_back(r.r); }
        if (!blk) v.restricted = true;
        for (const auto& p : rangePresets()) if (u.name == p.name) note(std::string(p.name) + ": " + p.description);
    }
    return v;
}

static void jsonRange(std::string& o, const FreqRange& r, const char* extra = "") {
    o += "{\"lo\":" + jsonNumber(r.lo) + ",\"hi\":" + jsonNumber(r.hi);
    if (r.modes != kAllModes) {
        o += ",\"modes\":[";
        bool first = true;
        for (int d = 0; d < (int)Demod::Count; d++) if (r.modes & (1u << d)) { o += (first ? "\"" : ",\"") + std::string(demodName((Demod)d)) + "\""; first = false; }
        o += "]";
    }
    if (!r.note.empty()) o += ",\"note\":\"" + jsonEsc(r.note) + "\"";
    o += extra;
    o += "}";
}

std::string rangesJson(const RangeView& v, bool clamp) {
    // jsonNumber keeps 10 significant digits: enough for 1 Hz up to 9.99 GHz; the browser rounds the edges inward, never outward
    std::string o = std::string("{\"restricted\":") + (v.restricted ? "true" : "false") + ",\"action\":\"" + (clamp ? "clamp" : "refuse") + "\",\"guard\":{\"min_hz\":200,\"max_hz\":5000,\"fraction\":0.05},\"allow\":[";
    for (size_t i = 0; i < v.allow.size(); i++) { if (i) o += ","; jsonRange(o, v.allow[i]); }
    o += "],\"block\":[";
    for (size_t i = 0; i < v.block.size(); i++) { if (i) o += ","; jsonRange(o, v.block[i]); }
    o += "],\"notes\":[";
    for (size_t i = 0; i < v.notes.size(); i++) { if (i) o += ","; o += "\"" + jsonEsc(v.notes[i]) + "\""; }
    o += "]}";
    return o;
}

std::string WebRanges::json() const {
    std::string o = std::string("{\"action\":\"") + (clamp_ ? "clamp" : "refuse") + "\",\"allow\":[";
    auto classes = [](unsigned m) {
        std::string s = ",\"classes\":[";
        bool first = true;
        for (int k = 0; k < 3; k++) if (m & (1u << k)) { s += (first ? "\"" : ",\"") + std::string(rangeClassName((RangeClass)k)) + "\""; first = false; }
        return s + "]";
    };
    for (size_t i = 0; i < allow_.size(); i++) { if (i) o += ","; jsonRange(o, allow_[i].r, classes(allow_[i].classes).c_str()); }
    o += "],\"block\":[";
    for (size_t i = 0; i < block_.size(); i++) { if (i) o += ","; jsonRange(o, block_[i].r, classes(block_[i].classes).c_str()); }
    o += "],\"presets\":[";
    for (size_t i = 0; i < presets_.size(); i++) { if (i) o += ","; o += "{\"name\":\"" + presets_[i].name + "\"" + classes(presets_[i].classes) + "}"; }
    o += "],\"available_presets\":[";
    const auto& ps = rangePresets();
    for (size_t i = 0; i < ps.size(); i++) { if (i) o += ","; o += std::string("{\"name\":\"") + ps[i].name + "\",\"block\":" + (ps[i].block ? "true" : "false") + ",\"description\":\"" + jsonEsc(ps[i].description) + "\"}"; }
    o += "],\"specs\":{\"allow\":[";
    for (size_t i = 0; i < allow_.size(); i++) { if (i) o += ","; o += "\"" + jsonEsc(formatRangeSpec(allow_[i])) + "\""; }
    o += "],\"block\":[";
    for (size_t i = 0; i < block_.size(); i++) { if (i) o += ","; o += "\"" + jsonEsc(formatRangeSpec(block_[i])) + "\""; }
    o += "]}}";
    return o;
}

} // namespace onair_web
