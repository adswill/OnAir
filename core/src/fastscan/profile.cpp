#include "dect2/fastscan/profile.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace fastscan {

namespace {
bool number(const std::string& s, double& v) {
    char* e = nullptr;
    v = std::strtod(s.c_str(), &e);
    if (e == s.c_str()) return false;
    while (*e == ' ' || *e == '\t') e++;
    return *e == 0 && std::isfinite(v);
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}
std::string clean(const std::string& s) { std::string o = s; for (char& c : o) if (c == ';' || c == '\n' || c == '=' || c == '|') c = ','; return o; }
}  // namespace

std::vector<FreqRange> parseRanges(const std::string& text, double pointKhz) {
    std::vector<FreqRange> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find_first_of(",;\n", pos);
        if (e == std::string::npos) e = text.size();
        const std::string part = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (part.empty()) continue;
        double a = 0, b = 0;
        const size_t dash = part.find('-', 1);   // a leading '-' would be a sign
        if (dash != std::string::npos) {
            if (!number(trim(part.substr(0, dash)), a) || !number(trim(part.substr(dash + 1)), b)) continue;
            if (b < a || a <= 0) continue;
            out.push_back({a * 1e6, b * 1e6});
        } else {
            if (!number(part, a) || a <= 0) continue;
            out.push_back({a * 1e6 - pointKhz * 1e3, a * 1e6 + pointKhz * 1e3});
        }
    }
    return out;
}

std::string formatRanges(const std::vector<FreqRange>& r) {
    std::string s;
    for (const auto& x : r) {
        char b[64];
        snprintf(b, sizeof b, "%s%.6g-%.6g", s.empty() ? "" : ", ", x.loHz / 1e6, x.hiHz / 1e6);
        s += b;
    }
    return s;
}

std::string serializeProfile(const ScanProfile& p) {
    char b[1024];
    snprintf(b, sizeof b, "name=%s;bands=%s;excl=%s;prio=%s;step=%.6g;dwell=%.6g;thr=%.6g;mindur=%.6g;hang=%.6g;chan=%.6g;rec=%d;iq=%d;maxrec=%d;budget=%.6g;recbw=%.6g",
             clean(p.name).c_str(), clean(p.bands).c_str(), clean(p.exclusions).c_str(), clean(p.priorityBands).c_str(), p.stepKhz, p.dwellMs, p.thresholdDb, p.minDurSec,
             p.hangSec, p.channelKhz, (int)p.record, (int)p.recordIq, p.maxRecordings, p.budgetMb, p.recordKhz);
    return b;
}

bool parseProfile(const std::string& s, ScanProfile& out) {
    ScanProfile p;
    bool any = false;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find(';', pos);
        if (e == std::string::npos) e = s.size();
        const std::string kv = s.substr(pos, e - pos);
        pos = e + 1;
        const size_t q = kv.find('=');
        if (q == std::string::npos) continue;
        const std::string k = kv.substr(0, q), v = kv.substr(q + 1);
        double d = 0;
        const bool num = number(trim(v), d);
        if (k == "name") p.name = v;
        else if (k == "bands") p.bands = v;
        else if (k == "excl") p.exclusions = v;
        else if (k == "prio") p.priorityBands = v;
        else if (!num) continue;
        else if (k == "step") p.stepKhz = std::min(1000.0, std::max(0.5, d));
        else if (k == "dwell") p.dwellMs = std::min(500.0, std::max(0.5, d));
        else if (k == "thr") p.thresholdDb = std::min(40.0, std::max(3.0, d));
        else if (k == "mindur") p.minDurSec = std::min(60.0, std::max(0.0, d));
        else if (k == "hang") p.hangSec = std::min(120.0, std::max(0.0, d));
        else if (k == "chan") p.channelKhz = std::min(1000.0, std::max(0.0, d));
        else if (k == "rec") p.record = d != 0;
        else if (k == "iq") p.recordIq = d != 0;
        else if (k == "maxrec") p.maxRecordings = (int)std::min(100.0, std::max(1.0, d));
        else if (k == "budget") p.budgetMb = std::min(1e6, std::max(16.0, d));
        else if (k == "recbw") p.recordKhz = std::min(100.0, std::max(3.0, d));
        else continue;
        any = true;
    }
    if (!any) return false;
    out = p;
    return true;
}

std::vector<ScanProfile> builtinProfiles() {
    std::vector<ScanProfile> v;
    auto add = [&](const char* name, const char* bands, double step, double chan, double thr, const char* excl = "", const char* prio = "") {
        ScanProfile p;
        p.name = name; p.bands = bands; p.stepKhz = step; p.channelKhz = chan; p.thresholdDb = thr; p.exclusions = excl; p.priorityBands = prio;
        v.push_back(p);
    };
    add("Airband 118-137", "118-137", 8.33, 25, 10, "", "121.5");
    add("Military UHF air 225-400", "225-400", 25, 25, 10, "", "243.0, 282.8");   // AM, 25 kHz channels; 243.0 guard, 282.8 NATO on-scene SAR
    add("Marine VHF", "156-162.025", 12.5, 25, 10);
    add("VHF/UHF amateur + PMR", "144-146, 430-440, 446-446.2", 12.5, 12.5, 10);
    add("Public safety VHF", "150-174", 12.5, 12.5, 10, "", "");
    add("Everything 30-1000 MHz (no FM broadcast)", "30-1000", 25, 0, 12, "87.5-108");
    add("FM broadcast", "87.5-108", 50, 100, 15);
    add("VHF + UHF 30-1000 MHz (quick look)", "30-1000", 25, 0, 12, "87.5-108");
    add("Airband + marine + public safety", "118-137, 156-162.025, 150-174", 12.5, 12.5, 10);
    return v;
}

FreqRange wholeSpan(const RadioProfile& r) {
    FreqRange f;
    double lo = 25, hi = 1700;                                  // RTL-SDR R820T and unknown radios
    if (r.id == "hackrf") { lo = 1; hi = 6000; }
    else if (r.id == "airspy") { lo = 24; hi = 1800; }
    else if (r.id == "sdrplay") { lo = 1; hi = 2000; }
    else if (r.id == "wide") { lo = 70; hi = 6000; }
    else if (r.id == "pluto") { lo = 325; hi = 3800; }
    f.loHz = lo * 1e6; f.hiHz = hi * 1e6;
    return f;
}

AutoScan autoScan(const RadioProfile& r, double spanHz) {
    AutoScan a;
    a.stepKhz = spanHz <= 100e6 ? 12.5 : (spanHz <= 1e9 ? 25 : 50);
    a.dwellMs = spanHz > 200e6 ? std::max(2.0, r.minDwellSec * 1e3) : 4.0;
    a.thresholdDb = spanHz > 1e9 ? 12 : 10;
    return a;
}

ScanProfile wholeSpanProfile(const RadioProfile& r) {
    const FreqRange f = wholeSpan(r);
    const AutoScan a = autoScan(r, f.hiHz - f.loHz);
    ScanProfile p;
    char b[64]; snprintf(b, sizeof b, "%g-%g", f.loHz / 1e6, f.hiHz / 1e6);
    p.name = "Whole span"; p.bands = b;
    p.stepKhz = a.stepKhz; p.dwellMs = a.dwellMs; p.thresholdDb = a.thresholdDb;
    p.minDurSec = 0; p.hangSec = 3;
    return p;
}

FastScanConfig toConfig(const ScanProfile& p) {
    FastScanConfig c;
    c.plan.bands = parseRanges(p.bands);
    c.plan.exclusions = parseRanges(p.exclusions);
    c.plan.priority = parseRanges(p.priorityBands);
    c.plan.binHz = p.stepKhz * 1e3;
    c.plan.dwellSec = p.dwellMs / 1e3;
    c.hits.onDb = (float)p.thresholdDb;
    c.hits.offDb = (float)std::max(2.0, p.thresholdDb - 3.0);
    c.hits.minDurSec = p.minDurSec;
    c.hits.hangSec = p.hangSec;
    c.hits.mergeHz = std::max(p.stepKhz * 1e3, 5000.0);
    if (p.channelKhz > 0 && !c.plan.bands.empty()) {
        double lo = c.plan.bands[0].loHz;
        for (const auto& b : c.plan.bands) lo = std::min(lo, b.loHz);
        c.hits.gridStepHz = p.channelKhz * 1e3;
        c.hits.gridOriginHz = lo;
    }
    return c;
}

HitMemory hitToMemory(const Hit& h, const std::string& mode) {
    HitMemory m;
    m.mhz = h.freqHz / 1e6;
    m.mode = mode;
    char b[64];
    snprintf(b, sizeof b, "Hit %.4f MHz", m.mhz);
    m.name = b;
    m.group = "Fast scan";
    return m;
}

}  // namespace fastscan
}  // namespace dect2
