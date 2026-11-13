// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham operating helpers: see ham_ops.h.
#include "dect2/ham/ham_ops.h"
#include "dect2/ham/ham.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace dect2::ham {

namespace {
constexpr double kPi = 3.14159265358979323846, kD = kPi / 180;
double wrap180(double x) { x = std::fmod(x + 180, 360); if (x < 0) x += 360; return x - 180; }
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
}  // namespace

// ---- the sun (low-precision almanac formulas, about 0.01 degree: plenty for a map) ----
void subsolarPoint(double utc, double& lat, double& lon) {
    const double n = (utc - 946728000.0) / 86400.0;          // days from J2000.0 (2000-01-01 12:00 UTC)
    const double L = std::fmod(280.460 + 0.9856474 * n, 360), g = std::fmod(357.528 + 0.9856003 * n, 360) * kD;
    const double lam = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kD, eps = (23.439 - 0.0000004 * n) * kD;
    const double dec = std::asin(std::sin(eps) * std::sin(lam));
    const double ra = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam)) / kD;
    const double gmst = std::fmod(280.46061837 + 360.98564736629 * n, 360);
    lat = dec / kD;
    lon = wrap180(ra - gmst);
}
double sunElevation(double lat, double lon, double utc) {
    double sl, so;
    subsolarPoint(utc, sl, so);
    const double s = std::sin(lat * kD) * std::sin(sl * kD) + std::cos(lat * kD) * std::cos(sl * kD) * std::cos((lon - so) * kD);
    return std::asin(std::max(-1.0, std::min(1.0, s))) / kD;
}
std::vector<std::pair<double, double>> terminator(double utc, int n) {
    double sl, so;
    subsolarPoint(utc, sl, so);
    if (std::fabs(sl) < 0.01) sl = sl < 0 ? -0.01 : 0.01;    // equinox: the terminator is (almost) a meridian pair
    std::vector<std::pair<double, double>> out;
    n = std::max(2, n);
    for (int i = 0; i < n; i++) {
        const double lon = -180 + 360.0 * i / (n - 1);
        const double lat = std::atan(-std::cos((lon - so) * kD) / std::tan(sl * kD)) / kD;
        out.push_back({lat, lon});
    }
    return out;
}
bool inGrayline(double lat, double lon, double utc, double bandDeg) { return std::fabs(sunElevation(lat, lon, utc)) <= bandDeg; }

// ---- paths ----
std::vector<std::pair<double, double>> gcPath(double lat1, double lon1, double lat2, double lon2, int n, bool longPath) {
    auto vec = [](double la, double lo, double v[3]) { v[0] = std::cos(la * kD) * std::cos(lo * kD); v[1] = std::cos(la * kD) * std::sin(lo * kD); v[2] = std::sin(la * kD); };
    double a[3], b[3];
    vec(lat1, lon1, a); vec(lat2, lon2, b);
    const double dot = std::max(-1.0, std::min(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
    const double d = std::acos(dot);
    double w[3] = {b[0] - a[0] * dot, b[1] - a[1] * dot, b[2] - a[2] * dot};
    double wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (wl < 1e-12) {   // the same point or antipodes: any great circle; take the one through the pole
        const double pole[3] = {0, 0, 1};
        w[0] = pole[0] - a[0] * a[2]; w[1] = pole[1] - a[1] * a[2]; w[2] = pole[2] - a[2] * a[2];
        wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (wl < 1e-12) { w[0] = 1; w[1] = 0; w[2] = 0; wl = 1; }
    }
    for (double& x : w) x /= wl;
    const double span = longPath ? 2 * kPi - d : d, sign = longPath ? -1 : 1;
    std::vector<std::pair<double, double>> out;
    n = std::max(1, n);
    for (int i = 0; i <= n; i++) {
        const double t = span * i / n;
        double p[3];
        for (int k = 0; k < 3; k++) p[k] = a[k] * std::cos(t) + sign * w[k] * std::sin(t);
        out.push_back({std::asin(std::max(-1.0, std::min(1.0, p[2]))) / kD, std::atan2(p[1], p[0]) / kD});
    }
    return out;
}
bool pathBetween(const std::string& fromGrid, const std::string& toGrid, PathInfo& o) {
    double la1, lo1, la2, lo2;
    if (!fromMaidenhead(fromGrid, la1, lo1) || !fromMaidenhead(toGrid, la2, lo2)) return false;
    greatCircle(la1, lo1, la2, lo2, o.km, o.bearing);
    o.longKm = 40030.2 - o.km;
    o.longBearing = std::fmod(o.bearing + 180, 360);
    return true;
}

// ---- the band bar ----
const std::vector<BandDial>& bandDials() {
    static const std::vector<BandDial> d = {
        {"160m", 1.840, 1.840, 1.838, 1.839, 1.8366}, {"80m", 3.573, 3.575, 3.570, 3.572, 3.5686}, {"60m", 5.357, 0, 0, 0, 5.2872},
        {"40m", 7.074, 7.0475, 7.076, 7.078, 7.0386},  {"30m", 10.136, 10.140, 10.138, 10.140, 10.1387}, {"20m", 14.074, 14.080, 14.076, 14.078, 14.0956},
        {"17m", 18.100, 18.104, 18.102, 18.104, 18.1046}, {"15m", 21.074, 21.140, 21.076, 21.078, 21.0946}, {"12m", 24.915, 24.919, 24.917, 24.919, 24.9246},
        {"11m", 27.265, 0, 0, 0, 0},   // CB: the dial CB FT8 groups use by custom, no standard; not an amateur band: transmit shows a warning, it is not refused
        {"10m", 28.074, 28.180, 28.076, 28.078, 28.1246}, {"6m", 50.313, 50.318, 50.310, 50.312, 50.293}, {"2m", 144.174, 144.170, 144.120, 0, 144.489}};
    return d;
}
double dialFor(const std::string& band, int mode) {
    for (const auto& b : bandDials()) {
        if (band != b.band) continue;
        switch (mode) {
        case 0: return b.ft8;
        case 1: case 2: return b.ft4;
        case 3: return b.wspr;
        case 4: case 10: return b.jt65;
        case 5: return b.jt9;
        default: return b.ft8;
        }
    }
    return 0;
}
bool regionAllows(int region, double hz, double bwHz) {
    struct E { double lo, hi; unsigned regions; };   // MHz; bit 0 region 1, bit 1 region 2, bit 2 region 3
    static const E kE[] = {{0.1357, 0.1378, 7}, {0.472, 0.479, 7}, {1.810, 2.000, 1}, {1.800, 2.000, 6}, {3.500, 3.800, 1}, {3.500, 4.000, 2}, {3.500, 3.900, 4},
                           {5.3515, 5.3665, 7}, {7.000, 7.200, 5}, {7.000, 7.300, 2}, {10.100, 10.150, 7}, {14.000, 14.350, 7}, {18.068, 18.168, 7},
                           {21.000, 21.450, 7}, {24.890, 24.990, 7}, {28.000, 29.700, 7}, {50.000, 52.000, 1}, {50.000, 54.000, 6}, {70.000, 70.500, 1},
                           {144.000, 146.000, 1}, {144.000, 148.000, 6}, {430.000, 440.000, 5}, {420.000, 450.000, 2}, {1240.000, 1300.000, 7}};
    if (region < 1 || region > 3) return true;
    const double lo = hz / 1e6, hi = (hz + std::max(0.0, bwHz)) / 1e6;
    for (const E& e : kE) if ((e.regions >> (region - 1) & 1) && lo >= e.lo && hi <= e.hi) return true;
    return false;
}

// ---- contests ----
const std::vector<ContestDef>& contestDefs() {
    static const std::vector<ContestDef> d = {
        {"DX", "Any contest: RST + serial", Exch::Serial, true, false},
        {"CQ-WW-CW", "CQ WW DX CW (RST + CQ zone)", Exch::CqZone, true, false},
        {"CQ-WW-SSB", "CQ WW DX SSB (RST + CQ zone)", Exch::CqZone, true, false},
        {"CQ-WW-RTTY", "CQ WW RTTY (RST + CQ zone)", Exch::CqZone, true, false},
        {"CQ-WPX-CW", "CQ WPX CW (RST + serial)", Exch::Serial, true, false},
        {"CQ-WPX-SSB", "CQ WPX SSB (RST + serial)", Exch::Serial, true, false},
        {"CQ-WPX-RTTY", "CQ WPX RTTY (RST + serial)", Exch::Serial, true, false},
        {"IARU-HF", "IARU HF Championship (RST + ITU zone)", Exch::ItuZone, true, true},
        {"CQ-WW-DIGI", "CQ WW Digi (grid)", Exch::Grid, true, false},
        {"ARRL-DX-CW", "ARRL DX CW (RST + state or power)", Exch::Free, true, false},
        {"ARRL-DX-SSB", "ARRL DX SSB (RST + state or power)", Exch::Free, true, false},
        {"ARRL-FD", "ARRL Field Day (class + section)", Exch::Free, true, true},
    };
    return d;
}
std::string ContestSession::sent(const std::string& rst) const {
    const auto& d = contestDefs()[(size_t)std::max(0, std::min(def, (int)contestDefs().size() - 1))];
    char b[16]; snprintf(b, sizeof b, "%03d", nextSerial);
    switch (d.exch) {
    case Exch::Serial: return rst + " " + b;
    case Exch::Grid: return upper(myExch.substr(0, 4));
    default: return d.exch == Exch::Free && rst.empty() ? upper(myExch) : rst + " " + upper(myExch);
    }
}
bool ContestSession::dupe(const Logbook& log, const std::string& call, const std::string& band, const std::string& mode) const {
    const auto& d = contestDefs()[(size_t)std::max(0, std::min(def, (int)contestDefs().size() - 1))];
    const std::string bc = baseCall(call), b = band;
    for (const LogQso& q : log.qsos()) {
        if (q.contestId != d.id || baseCall(q.call) != bc) continue;
        if (d.perBand && q.band != b) continue;
        if (d.perMode && modeClass(q.mode) != modeClass(mode)) continue;
        return true;
    }
    return false;
}
std::map<std::string, std::vector<std::string>> ContestSession::sheet(const Logbook& log) const {
    const auto& d = contestDefs()[(size_t)std::max(0, std::min(def, (int)contestDefs().size() - 1))];
    std::map<std::string, std::set<std::string>> s;
    for (const LogQso& q : log.qsos()) if (q.contestId == d.id) s[q.band].insert(baseCall(q.call));
    std::map<std::string, std::vector<std::string>> out;
    for (auto& kv : s) out[kv.first].assign(kv.second.begin(), kv.second.end());
    return out;
}
void ContestSession::resume(const Logbook& log) {
    const auto& d = contestDefs()[(size_t)std::max(0, std::min(def, (int)contestDefs().size() - 1))];
    int hi = 0;
    for (const LogQso& q : log.qsos()) {
        if (q.contestId != d.id) continue;
        hi = std::max(hi, std::atoi(q.stx.c_str()));
        const size_t sp = q.stx.rfind(' ');
        if (sp != std::string::npos) hi = std::max(hi, std::atoi(q.stx.c_str() + sp + 1));
    }
    nextSerial = hi + 1;
}
std::string cabrilloMode(const std::string& m) {
    const std::string u = upper(m);
    if (u == "CW") return "CW";
    if (u == "RTTY") return "RY";
    if (u == "FM") return "FM";
    if (u == "SSB" || u == "USB" || u == "LSB" || u == "AM") return "PH";
    return "DG";
}
std::string cabrilloExport(const std::vector<LogQso>& log, const CabrilloHeader& h) {
    std::string o = "START-OF-LOG: 3.0\nCREATED-BY: OnAir\n";
    auto line = [&o](const char* k, const std::string& v) { if (!v.empty()) o += std::string(k) + ": " + v + "\n"; };
    line("CONTEST", h.contest); line("CALLSIGN", upper(h.call)); line("CATEGORY", h.category); line("LOCATION", h.location);
    line("GRID-LOCATOR", upper(h.grid)); line("NAME", h.name); line("CLUB", h.club); line("OPERATORS", upper(h.operators)); line("SOAPBOX", h.soapbox);
    for (const LogQso& q : log) {
        if (!h.contest.empty() && q.contestId != h.contest) continue;
        char b[256];
        const std::string date = adifDate(q.startUtc), tm = adifTime(q.startUtc);
        const std::string d = date.size() == 8 ? date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2) : date;
        const std::string sent = (q.rstSent.empty() ? "" : q.rstSent + " ") + q.stx, rcvd = (q.rstRcvd.empty() ? "" : q.rstRcvd + " ") + q.srx;
        snprintf(b, sizeof b, "QSO: %5ld %-2s %s %s %-13s %-10s %-13s %s\n", std::lround(q.freqHz / 1e3), cabrilloMode(q.mode).c_str(), d.c_str(),
                 tm.substr(0, 4).c_str(), upper(q.myCall.empty() ? h.call : q.myCall).c_str(), sent.c_str(), q.call.c_str(), rcvd.c_str());
        std::string l = b;
        while (l.size() > 1 && l[l.size() - 2] == ' ') l.erase(l.size() - 2, 1);   // no trailing spaces
        o += l;
    }
    o += "END-OF-LOG:\n";
    return o;
}

// ---- spots ----
void SpotList::heard(const std::string& call, const std::string& grid, const std::string& band, const std::string& mode, double utc, double dialHz,
                     double hz, int snr, bool cq, const std::string& msg) {
    if (call.empty()) return;
    Spot sp;
    for (size_t i = 0; i < s_.size(); i++)
        if (s_[i].call == call && s_[i].band == band) { sp = s_[i]; s_.erase(s_.begin() + (long)i); break; }
    sp.call = call; sp.band = band; sp.mode = mode;
    if (!grid.empty()) sp.grid = grid;
    sp.lastUtc = std::max(sp.lastUtc, utc); sp.dialHz = dialHz; sp.hz = hz; sp.snr = snr; sp.bestSnr = std::max(sp.bestSnr, snr);
    sp.count++; sp.cq = cq; sp.lastMsg = msg;
    s_.insert(s_.begin(), sp);
    if (s_.size() > 2000) s_.pop_back();
}
void SpotList::expire(double now, double maxAge) {
    s_.erase(std::remove_if(s_.begin(), s_.end(), [&](const Spot& s) { return now - s.lastUtc > maxAge; }), s_.end());
}

// ---- macros ----
std::string expandMacro(const std::string& t, const MacroVars& v) {
    std::string o;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '{') {
            const size_t e = t.find('}', i);
            if (e != std::string::npos) {
                const std::string k = upper(t.substr(i + 1, e - i - 1));
                const std::string* r = k == "MYCALL" ? &v.myCall : k == "MYGRID" ? &v.myGrid : k == "CALL" ? &v.call : k == "GRID" ? &v.grid : k == "RST" ? &v.rst
                                     : k == "SNR" ? &v.snr : k == "SERIAL" ? &v.serial : k == "EXCH" ? &v.exch : k == "NAME" ? &v.name : k == "QTH" ? &v.qth : nullptr;
                if (r) { o += *r; i = e; continue; }
            }
        }
        o += t[i];
    }
    return o;
}

}  // namespace dect2::ham
