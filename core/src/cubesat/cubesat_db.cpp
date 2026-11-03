// The Smart CubeSat satellite tables and pass planner: see cubesat_db.h.
#include "dect2/cubesat/cubesat_db.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace cubesat {

namespace {

struct CsTxRow { int norad; const char* name; long long hz; int mod, hint, baud; const char* desc; const char* mode; };
struct CsTleRow { int norad; const char* name; const char* l1; const char* l2; };
#include "cubesat_db_data.inc"

const std::vector<TxEntry>& builtin() {
    static const std::vector<TxEntry> v = [] {
        std::vector<TxEntry> t;
        for (const CsTxRow& r : kCsTx) {
            TxEntry e; e.norad = r.norad; e.name = r.name; e.hz = (double)r.hz; e.mod = r.mod; e.hint = r.hint; e.baud = r.baud; e.desc = r.desc; e.mode = r.mode;
            t.push_back(e);
        }
        return t;
    }();
    return v;
}

} // namespace

const char* modName(int m) {
    switch (m) { case ModCw: return "CW / OOK"; case ModAfsk: return "AFSK"; case ModFsk: return "FSK / GMSK"; case ModBpsk: return "BPSK"; case ModLora: return "LoRa"; default: return "unknown"; }
}
const char* hintName(int h) {
    switch (h) { case HintAx25: return "AX.25"; case HintAx100M5: return "AX100 Mode 5"; case HintG3ruh: return "AX.25 G3RUH"; case HintUsp: return "USP"; case HintTubix: return "TUBiX10"; case HintDoka: return "DOKA"; default: return "auto"; }
}
bool decodable(const TxEntry& t) { return t.hint != HintUsp && t.hint != HintTubix && t.hint != HintDoka; }

const std::vector<TxEntry>& txTable() { return builtin(); }

std::string tleSnapshotText() {
    std::string s;
    for (const CsTleRow& r : kCsTle) { s += r.name; s += '\n'; s += r.l1; s += '\n'; s += r.l2; s += '\n'; }
    return s;
}

std::string txToText(const std::vector<TxEntry>& v) {
    std::string s;
    for (const TxEntry& t : v) {
        std::string n = t.name;
        for (char& c : n) if (c == '|' || c == ';') c = ' ';
        char b[256];
        snprintf(b, sizeof b, "%d|%s|%.4f|%d|%d|%d;", t.norad, n.c_str(), t.hz / 1e6, t.mod, t.hint, t.baud);
        s += b;
    }
    return s;
}

std::vector<TxEntry> txFromText(const std::string& s) {
    std::vector<TxEntry> v;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ';');) {
        std::stringstream is(item);
        std::string f[6];
        int k = 0;
        while (k < 6 && std::getline(is, f[k], '|')) k++;
        if (k < 6 || atoi(f[0].c_str()) <= 0 || atof(f[2].c_str()) <= 0) continue;
        TxEntry t;
        t.norad = atoi(f[0].c_str()); t.name = f[1]; t.hz = atof(f[2].c_str()) * 1e6;
        t.mod = std::max(0, std::min((int)ModLora, atoi(f[3].c_str()))); t.hint = std::max(0, std::min((int)HintDoka, atoi(f[4].c_str()))); t.baud = std::max(0, atoi(f[5].c_str()));
        t.desc = "user"; t.mode = "user";
        v.push_back(t);
    }
    return v;
}

PassPlanner::PassPlanner() {
    addTleText(tleSnapshotText());
    rebuildTx();
}

void PassPlanner::setObserver(const Observer& o) { obs_ = o; }

size_t PassPlanner::addTleText(const std::string& text) {
    size_t n = 0;
    for (const Tle& t : parseTleText(text)) {
        const auto it = tle_.find(t.norad);
        if (it != tle_.end() && it->second.epochJd >= t.epochJd) continue;
        Sgp4 s;
        if (!s.init(t)) continue;                 // deep space (the high orbits of the old amateur satellites) or unusable elements
        tle_[t.norad] = t;
        sgp_[t.norad] = s;
        n++;
    }
    return n;
}

void PassPlanner::rebuildTx() {
    tx_ = builtin();
    tx_.insert(tx_.end(), extra_.begin(), extra_.end());
}

void PassPlanner::setExtraTx(const std::vector<TxEntry>& v) { extra_ = v; rebuildTx(); }

double PassPlanner::newestEpochJd() const {
    double m = 0;
    for (const auto& kv : tle_) m = std::max(m, kv.second.epochJd);
    return m;
}

std::string PassPlanner::nameOf(int norad) const {
    const auto it = tle_.find(norad);
    if (it != tle_.end() && !it->second.name.empty()) return it->second.name;
    for (const TxEntry& t : tx_) if (t.norad == norad) return t.name;
    return "NORAD " + std::to_string(norad);
}

bool PassPlanner::look(int norad, double t, Look& l) const {
    const auto it = sgp_.find(norad);
    return it != sgp_.end() && lookAt(it->second, obs_, t, l);
}

bool PassPlanner::doppler(int norad, double t, double freqHz, double& shift) const {
    Look l;
    if (!look(norad, t, l)) return false;
    shift = dopplerHz(freqHz, l.rangeRateKmS);
    return true;
}

std::vector<PlannedPass> PassPlanner::upcoming(double now, double hours, double minEl, size_t max) const {
    std::vector<PlannedPass> out;
    std::map<int, std::vector<size_t>> byNorad;
    for (size_t i = 0; i < tx_.size(); i++) if (decodable(tx_[i])) byNorad[tx_[i].norad].push_back(i);
    for (const auto& kv : byNorad) {
        const auto it = sgp_.find(kv.first);
        if (it == sgp_.end()) continue;
        for (const Pass& p : findPasses(it->second, obs_, now, now + hours * 3600.0, minEl)) {
            PlannedPass pp;
            pp.norad = kv.first; pp.name = nameOf(kv.first); pp.p = p; pp.tx = kv.second;
            pp.epochAgeDays = (unixToJd(p.tca) - it->second.tle().epochJd);
            out.push_back(std::move(pp));
        }
    }
    std::sort(out.begin(), out.end(), [](const PlannedPass& a, const PlannedPass& b) { return a.p.aos < b.p.aos; });
    if (max && out.size() > max) out.resize(max);
    return out;
}

} // namespace cubesat
} // namespace dect2
