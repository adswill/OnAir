// Full control over transmitting: calibration, IQ correction, timeout and schedule, and the per-driver plans (tx_control.h).
#include "dect2/tx/tx_control.h"
#include "dect2/tx/tx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>

namespace dect2 {

uint32_t hackrfAutoFilterHz(double sampleRate, double channelMhz);   // hackrf_usb.cpp

namespace {

constexpr double kPi = 3.14159265358979323846;

double num(const std::string& s) {
    char* end = nullptr;
    const double v = strtod(s.c_str(), &end);
    return end && end != s.c_str() && std::isfinite(v) ? v : 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(sep, pos);
        if (e == std::string::npos) e = s.size();
        v.push_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return v;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

// the points of one amplifier setting, sorted by gain, one per gain (the last measured wins)
std::vector<TxPowerPoint> pointsOf(const TxCalibration& c, bool amp) {
    std::vector<TxPowerPoint> v;
    for (const auto& p : c.power) {
        if (p.amp != amp || !std::isfinite(p.gainDb) || !std::isfinite(p.dbm)) continue;
        auto it = std::find_if(v.begin(), v.end(), [&](const TxPowerPoint& q) { return std::fabs(q.gainDb - p.gainDb) < 1e-9; });
        if (it != v.end()) *it = p; else v.push_back(p);
    }
    std::sort(v.begin(), v.end(), [](const TxPowerPoint& a, const TxPowerPoint& b) { return a.gainDb < b.gainDb; });
    return v;
}

double ppmOf(const TxTune& t) { TuneSettings ts; ts.radio = t.radio; return radioPpm(ts); }

double autoFilter(const TxTune& t, double runRateHz) { return t.bandwidthHz > 0 ? t.bandwidthHz : runRateHz * 0.95; }

void addFreqNote(TxHwPlan& p, const TxTune& t) {
    if (!std::isfinite(txRadioHz(t)) || txRadioHz(t) < 0) p.note = "a negative frequency cannot be sent; the radio is asked for 0 Hz";
}

} // namespace

// ---- calibration ---------------------------------------------------------------------------------------------------------------------------
std::string txCalToString(const TxCalibration& c) {
    char b[256];
    snprintf(b, sizeof b, "ppm=%.6g;hz=%.6g;dci=%.6g;dcq=%.6g;iqg=%.6g;iqp=%.6g", c.ppm, c.offsetHz, c.dcI, c.dcQ, c.iqGainDb, c.iqPhaseDeg);
    std::string s = b;
    if (!c.power.empty()) {
        s += ";pw=";
        for (size_t i = 0; i < c.power.size(); i++) {
            snprintf(b, sizeof b, "%s%.6g:%d:%.6g", i ? "/" : "", c.power[i].gainDb, c.power[i].amp ? 1 : 0, c.power[i].dbm);
            s += b;
        }
    }
    return s;
}

TxCalibration txCalFromString(const std::string& s) {
    TxCalibration c;
    for (const std::string& item : split(s, ';')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(item.substr(0, eq)), v = item.substr(eq + 1);
        if (k == "ppm") c.ppm = num(v);
        else if (k == "hz") c.offsetHz = num(v);
        else if (k == "dci") c.dcI = num(v);
        else if (k == "dcq") c.dcQ = num(v);
        else if (k == "iqg") c.iqGainDb = num(v);
        else if (k == "iqp") c.iqPhaseDeg = num(v);
        else if (k == "pw") {
            for (const std::string& pt : split(v, '/')) {
                const std::vector<std::string> f = split(pt, ':');
                if (f.size() != 3 || f[0].empty() || f[2].empty()) continue;
                c.power.push_back({num(f[0]), num(f[1]) != 0, num(f[2])});
            }
        }
    }
    return c;
}

double txCalPowerDbm(const TxCalibration& c, double gainDb, bool amp) {
    const std::vector<TxPowerPoint> v = pointsOf(c, amp);
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (v.size() == 1) return v[0].dbm + (gainDb - v[0].gainDb);
    size_t i = 1;
    while (i + 1 < v.size() && gainDb > v[i].gainDb) i++;   // the segment [i-1, i] holding gainDb, or the end segment beyond the table
    const TxPowerPoint& a = v[i - 1];
    const TxPowerPoint& b = v[i];
    const double slope = (b.dbm - a.dbm) / (b.gainDb - a.gainDb);
    return a.dbm + slope * (gainDb - a.gainDb);
}

double txCalGainForDbm(const TxCalibration& c, double dbm, bool amp) {
    const std::vector<TxPowerPoint> v = pointsOf(c, amp);
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (v.size() == 1) return v[0].gainDb + (dbm - v[0].dbm);
    // the table is measured, so it may not be monotonic: take the first segment that holds dbm, else extrapolate from the nearer end
    for (size_t i = 1; i < v.size(); i++) {
        const double lo = std::min(v[i - 1].dbm, v[i].dbm), hi = std::max(v[i - 1].dbm, v[i].dbm);
        if (dbm >= lo && dbm <= hi && hi > lo) return v[i - 1].gainDb + (dbm - v[i - 1].dbm) * (v[i].gainDb - v[i - 1].gainDb) / (v[i].dbm - v[i - 1].dbm);
    }
    const bool below = dbm < v.front().dbm;
    const TxPowerPoint& a = below ? v[0] : v[v.size() - 2];
    const TxPowerPoint& b = below ? v[1] : v.back();
    const double slope = (b.dbm - a.dbm) / (b.gainDb - a.gainDb);
    if (std::fabs(slope) < 1e-9) return (below ? a : b).gainDb;
    return a.gainDb + (dbm - a.dbm) / slope;
}

void txApplyCalibration(const TxCalibration& c, TxRequest& r) {
    if (c.ppm != 0 && !r.tune.radio.count("ppm")) {
        char b[32];
        snprintf(b, sizeof b, "%.6g", c.ppm);
        r.tune.radio["ppm"] = b;
    }
    r.tune.offsetHz += c.offsetHz;
    auto put = [&](const char* k, double v) { if (v != 0 && !r.opt.count(k)) r.opt[k] = v; };
    put("out.dcI", c.dcI);
    put("out.dcQ", c.dcQ);
    put("out.iqGainDb", c.iqGainDb);
    put("out.iqPhaseDeg", c.iqPhaseDeg);
}

// ---- IQ correction -------------------------------------------------------------------------------------------------------------------------
void txIqCorrect(cf32* x, size_t n, double gainDb, double phaseDeg, double dcI, double dcQ) {
    if (gainDb == 0 && phaseDeg == 0 && dcI == 0 && dcQ == 0) return;
    const float a = (float)std::pow(10.0, gainDb / 40.0), b = (float)std::pow(10.0, -gainDb / 40.0);
    const float cp = (float)std::cos(phaseDeg * kPi / 180.0), sp = (float)std::sin(phaseDeg * kPi / 180.0);
    const float di = (float)dcI, dq = (float)dcQ;
    for (size_t i = 0; i < n; i++) {
        const float I = x[i].real(), Q = x[i].imag();
        x[i] = cf32(a * I + di, b * (Q * cp + I * sp) + dq);
    }
}

// ---- timeout and schedule ------------------------------------------------------------------------------------------------------------------
double txTimeoutSec(const TxRequest& r) {
    const double t = r.o("tx.totSec", 0);
    return std::isfinite(t) && t > 0 ? t : 0;
}

double txScheduleNext(const TxSchedule& s, double nowUtc, double lastStartUtc, int done) {
    if (s.count > 0 && done >= s.count) return -1;
    if (done <= 0) return s.startUtc > nowUtc ? s.startUtc : nowUtc;
    if (s.everySec <= 0) return -1;
    double next = lastStartUtc + s.everySec;
    if (next < nowUtc) next = nowUtc;   // a transmission that ran longer than the interval: the next one starts at once, none is skipped twice
    return next;
}

double txParseUtcTime(const std::string& txt, double nowUtc) {
    const std::vector<std::string> f = split(trim(txt), ':');
    if (f.size() < 2 || f.size() > 3) return -1;
    int v[3] = {0, 0, 0};
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i].empty() || f[i].size() > 2) return -1;
        for (char ch : f[i]) if (ch < '0' || ch > '9') return -1;
        v[i] = atoi(f[i].c_str());
    }
    if (v[0] > 23 || v[1] > 59 || v[2] > 59) return -1;
    const double day = std::floor(nowUtc / 86400.0) * 86400.0;
    double t = day + v[0] * 3600.0 + v[1] * 60.0 + v[2];
    if (t < nowUtc - 0.5) t += 86400.0;
    return t;
}

// ---- the driver plans ------------------------------------------------------------------------------------------------------------------------
bool txHwApply(const TxHwPlan& p, TxHwOps& ops, bool withRate, std::string& err) {
    if (!p.err.empty()) { err = p.err; return false; }
    if (withRate) {
        if (!ops.setRate(p, err)) return false;
        if (!ops.setFilter(p.filterHz, err)) return false;
    }
    if (!ops.setFrequency(p.loHz, err)) return false;
    for (const auto& g : p.gains) if (!ops.setGain(g.first, g.second, err)) return false;
    if (p.ampOn >= 0 && !ops.setAmp(p.ampOn == 1, err)) return false;
    return true;
}

std::vector<std::pair<std::string, double>> txStageGains(const std::map<std::string, std::string>& radio) {
    std::vector<std::pair<std::string, double>> v;
    for (const auto& kv : radio) {
        if (kv.first.rfind("txgain.", 0) != 0 || kv.first.size() <= 7) continue;
        char* end = nullptr;
        const double g = strtod(kv.second.c_str(), &end);
        if (end == kv.second.c_str() || !std::isfinite(g)) continue;
        v.push_back({kv.first.substr(7), g});
    }
    return v;
}

bool txParseStageGains(const std::string& text, std::map<std::string, std::string>& radio) {
    std::map<std::string, std::string> add;
    for (std::string item : split(text, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) return false;
        const std::string name = trim(item.substr(0, eq)), val = trim(item.substr(eq + 1));
        char* end = nullptr;
        const double g = strtod(val.c_str(), &end);
        if (name.empty() || val.empty() || !end || *end || !std::isfinite(g)) return false;
        char b[32];
        snprintf(b, sizeof b, "%.6g", g);
        add["txgain." + name] = b;
    }
    for (auto it = radio.begin(); it != radio.end();) it = it->first.rfind("txgain.", 0) == 0 ? radio.erase(it) : std::next(it);
    radio.insert(add.begin(), add.end());
    return true;
}

std::string txStageGainsText(const std::map<std::string, std::string>& radio) {
    std::string s;
    for (const auto& g : txStageGains(radio)) {
        char b[96];
        snprintf(b, sizeof b, "%s%s=%.6g", s.empty() ? "" : ", ", g.first.c_str(), g.second);
        s += b;
    }
    return s;
}

TxHwPlan hackrfTxPlan(const TxTune& t) {
    TxHwPlan p;
    p.channel = t.channel;
    if (t.channel != 0) { p.err = "this HackRF has one transmit output (TX1)"; return p; }
    p.rateHz = t.sampleRate;
    p.rateDiv = 1;
    p.rateNum = (uint32_t)std::llround(t.sampleRate);
    for (uint32_t i = 1; i < 32; i++) {   // the firmware takes the rate as a fraction of whole numbers
        const double v = t.sampleRate * i;
        if (std::fabs(v - std::round(v)) < 1e-6) { p.rateDiv = i; p.rateNum = (uint32_t)std::llround(v); break; }
    }
    p.filterHz = t.bandwidthHz > 0 ? (double)(uint32_t)t.bandwidthHz : (double)hackrfAutoFilterHz(t.sampleRate, 0);
    addFreqNote(p, t);
    p.loHz = std::max(ppmCorrectedHz(txRadioHz(t), ppmOf(t)), 0.0);
    if (!std::isfinite(p.loHz)) p.loHz = 0;
    if (p.loHz / 1e6 >= 4294967295.0) p.err = "the HackRF's tuning request holds at most 4.29e9 MHz: " + std::to_string(p.loHz) + " Hz does not fit it";
    p.loHz = std::floor(p.loHz);   // whole hertz: MHz and Hz fields of the request
    p.gains.push_back({"TXVGA", (double)std::min(47, std::max(0, (int)std::lround(t.gainDb)))});
    p.ampOn = t.ampOn ? 1 : 0;
    return p;
}

TxHwPlan plutoTxPlan(const TxTune& t, double runRateHz, TxHwRange gain, double xoPpm) {
    TxHwPlan p;
    p.channel = t.channel < 0 ? 0 : t.channel;
    if (p.channel > 1) { p.err = "this Pluto has at most two transmit outputs"; return p; }
    p.rateHz = t.sampleRate;
    if (t.sampleRate < 2.083e6) { p.err = "PlutoSDR: the converter runs from 2.083 Msps up; " + std::to_string(t.sampleRate / 1e6) + " Msps is below that"; return p; }
    p.filterHz = (double)std::llround(std::min(56e6, std::max(0.2e6, autoFilter(t, runRateHz > 0 ? runRateHz : t.sampleRate))));
    addFreqNote(p, t);
    p.loHz = (double)std::llround(std::max(ppmCorrectedHz(txRadioHz(t), ppmOf(t)) * (1.0 + xoPpm * 1e-6), 0.0));
    if (!gain.known()) gain = {-89.75, 0};
    p.gains.push_back({"hardwaregain", gain.clamp(t.gainDb)});
    return p;
}

TxHwPlan usrpTxPlan(const TxTune& t, double runRateHz, TxHwRange gain) {
    TxHwPlan p;
    p.channel = std::max(0, t.channel);
    p.rateHz = std::max(t.sampleRate > 0 ? t.sampleRate : 1e6, 0.2e6);
    p.filterHz = std::max(0.2e6, autoFilter(t, runRateHz > 0 ? runRateHz : p.rateHz));
    p.antenna = "TX/RX";
    addFreqNote(p, t);
    p.loHz = std::max(ppmCorrectedHz(txRadioHz(t), ppmOf(t)), 0.0);
    if (!gain.known()) gain = {0, 89.75};
    p.gains.push_back({"", gain.clamp(t.gainDb)});
    return p;
}

TxHwPlan soapyTxPlan(const TxTune& t, double runRateHz, TxHwRange gain, const std::map<std::string, TxHwRange>& stages) {
    TxHwPlan p;
    p.channel = t.channel < 0 ? 0 : t.channel;
    p.rateHz = t.sampleRate;
    p.filterHz = autoFilter(t, runRateHz > 0 ? runRateHz : t.sampleRate);
    addFreqNote(p, t);
    p.loHz = std::max(ppmCorrectedHz(txRadioHz(t), ppmOf(t)), 0.0);
    p.gains.push_back({"", gain.clamp(t.gainDb)});   // only the radio's own scale clamps
    for (const auto& g : txStageGains(t.radio)) {
        const auto it = stages.find(g.first);
        if (!stages.empty() && it == stages.end()) {
            std::string names;
            for (const auto& s : stages) names += (names.empty() ? "" : ", ") + s.first;
            p.note += (p.note.empty() ? "" : "; ") + std::string("this radio lists no TX gain stage \"") + g.first + "\" (it has " + names + "); it is asked anyway";
        }
        p.gains.push_back({g.first, it == stages.end() ? g.second : it->second.clamp(g.second)});
    }
    return p;
}

} // namespace dect2
