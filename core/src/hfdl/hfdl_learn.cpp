#include "dect2/hfdl/hfdl_learn.h"
#include "dect2/hfdl/hfdl_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace dect2 {

HfdlFreqLearner& HfdlFreqLearner::global() {
    static HfdlFreqLearner* g = [] {
        auto* l = new HfdlFreqLearner();
        std::map<int, std::vector<int>> t;
        for (const auto& s : hfdlStations()) t[s.id] = s.khz;
        l->setBuiltin(t);
        return l;
    }();
    return *g;
}

void HfdlFreqLearner::setBuiltin(const std::map<int, std::vector<int>>& table) { std::lock_guard<std::mutex> lk(mu_); builtin_ = table; }

bool HfdlFreqLearner::builtinHas(int gs, int khz) const {
    auto it = builtin_.find(gs);
    return it != builtin_.end() && std::find(it->second.begin(), it->second.end(), khz) != it->second.end();
}

bool HfdlFreqLearner::driftLocked(const G& g) const {
    if (g.changedSec > 0) return true;
    int bad = 0;
    for (bool ok : g.consistent) if (!ok) bad++;
    return bad >= cfg_.driftMismatch;
}

bool HfdlFreqLearner::retiredLocked(const G& g, int gs, int khz, const F* f) const {
    if (!builtinHas(gs, khz) || g.changedSec <= 0) return false;
    if (f && f->hitsSince > 0) return false;
    const double listened = f ? f->listenSince : 0;
    return listened >= cfg_.retireListenSec && g.sqSince >= cfg_.retireMinSquitters;
}

void HfdlFreqLearner::squitter(int gs, int dialKhz, int version, const std::vector<int>& maskKhz, double now) {
    if (gs <= 0 || dialKhz < 2000 || dialKhz > 30000 || !std::isfinite(now)) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;
    G& g = g_[gs];
    if (version >= 0 && version <= 4095) {
        if (g.version >= 0 && version != g.version) {   // the system table changed: count the listening and the squitters from here
            g.changedSec = now; g.sqSince = 0;
            for (auto& kv : g.f) { kv.second.listenSince = 0; kv.second.hitsSince = 0; }
        }
        g.version = version;
    }
    g.squitters++; g.sqSince++; g.lastSec = now;
    F& f = g.f[dialKhz];
    f.hits++; f.hitsSince++; f.lastSec = now;
    g.consistent.push_back(std::find(maskKhz.begin(), maskKhz.end(), dialKhz) != maskKhz.end());
    while ((int)g.consistent.size() > cfg_.driftWindow) g.consistent.pop_front();
}

void HfdlFreqLearner::listen(const std::vector<int>& khz, double dt) {
    if (dt <= 0 || khz.empty() || dt > 60) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;
    for (auto& gv : g_) {
        G& g = gv.second;
        if (g.changedSec <= 0) continue;                 // only the time since a version change counts
        auto it = builtin_.find(gv.first);
        for (int k : khz) {
            const bool known = g.f.count(k) || (it != builtin_.end() && std::find(it->second.begin(), it->second.end(), k) != it->second.end());
            if (known) g.f[k].listenSince += dt;
        }
    }
}

std::vector<int> HfdlFreqLearner::planLocked(int gs) const {
    std::set<int> out;
    auto b = builtin_.find(gs);
    auto gi = g_.find(gs);
    if (b != builtin_.end()) {
        for (int k : b->second) {
            if (gi != g_.end()) {
                auto fi = gi->second.f.find(k);
                if (retiredLocked(gi->second, gs, k, fi == gi->second.f.end() ? nullptr : &fi->second)) continue;
            }
            out.insert(k);
        }
    }
    if (gi != g_.end())
        for (const auto& kv : gi->second.f)
            if (kv.second.hits >= cfg_.confirmHits && !builtinHas(gs, kv.first)) out.insert(kv.first);
    return std::vector<int>(out.begin(), out.end());
}

std::vector<int> HfdlFreqLearner::plan(int gs) const { std::lock_guard<std::mutex> lk(mu_); return planLocked(gs); }

std::vector<int> HfdlFreqLearner::allPlanKhz() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::set<int> out;
    std::set<int> gs;
    for (const auto& kv : builtin_) gs.insert(kv.first);
    for (const auto& kv : g_) gs.insert(kv.first);
    for (int g : gs) for (int k : planLocked(g)) out.insert(k);
    return std::vector<int>(out.begin(), out.end());
}

bool HfdlFreqLearner::drifted(int gs) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = g_.find(gs);
    return it != g_.end() && driftLocked(it->second);
}

std::vector<int> HfdlFreqLearner::announced(int gs, const std::vector<int>& mask, double now) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = g_.find(gs);
    if (it == g_.end() || !driftLocked(it->second)) {
        std::vector<int> o;
        for (int k : mask) {
            const F* f = nullptr;
            if (it != g_.end()) { auto fi = it->second.f.find(k); if (fi != it->second.f.end()) f = &fi->second; }
            if (it != g_.end() && retiredLocked(it->second, gs, k, f)) continue;
            o.push_back(k);
        }
        return o;
    }
    std::vector<int> o;   // drift: the mask indexes a table that is no longer the one on the air; what was heard is the truth
    for (const auto& kv : it->second.f)
        if (now - kv.second.lastSec <= cfg_.announcedAgeSec && (it->second.changedSec <= 0 || kv.second.hitsSince > 0)) o.push_back(kv.first);
    return o;
}

std::vector<HfdlFreqLearner::Row> HfdlFreqLearner::rows() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Row> out;
    for (const auto& gv : g_) {
        Row r;
        r.gsId = gv.first; r.version = gv.second.version; r.drift = driftLocked(gv.second); r.lastSquitterSec = gv.second.lastSec; r.squitters = gv.second.squitters;
        r.plan = planLocked(gv.first);
        for (const auto& kv : gv.second.f) if (kv.second.hits >= cfg_.confirmHits && !builtinHas(gv.first, kv.first)) r.learned.push_back(kv.first);
        auto b = builtin_.find(gv.first);
        if (b != builtin_.end())
            for (int k : b->second) {
                auto fi = gv.second.f.find(k);
                if (retiredLocked(gv.second, gv.first, k, fi == gv.second.f.end() ? nullptr : &fi->second)) r.retired.push_back(k);
            }
        out.push_back(r);
    }
    return out;
}

void HfdlFreqLearner::clear() { std::lock_guard<std::mutex> lk(mu_); g_.clear(); }

std::string HfdlFreqLearner::save() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string o = "hfdllearn 1\n";
    char b[200];
    for (const auto& gv : g_) {
        const G& g = gv.second;
        snprintf(b, sizeof b, "G %d %d %.0f %d %d %.0f\n", gv.first, g.version, g.changedSec, g.sqSince, g.squitters, g.lastSec);
        o += b;
        for (const auto& kv : g.f) {
            snprintf(b, sizeof b, "F %d %d %d %d %.0f %.1f\n", gv.first, kv.first, kv.second.hits, kv.second.hitsSince, kv.second.lastSec, kv.second.listenSince);
            o += b;
        }
    }
    return o;
}

void HfdlFreqLearner::load(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("hfdllearn 1", 0) != 0) return;
    while (std::getline(in, line)) {
        int gs, k, a, b, c;
        double t1, t2;
        if (line.size() > 2 && line[0] == 'G' && sscanf(line.c_str() + 2, "%d %d %lf %d %d %lf", &gs, &a, &t1, &b, &c, &t2) == 6) {
            if (gs <= 0 || gs > 127 || a < -1 || a > 4095 || b < 0 || c < 0) continue;
            G& g = g_[gs];
            g.version = a; g.changedSec = t1; g.sqSince = b; g.squitters = c; g.lastSec = t2;
        } else if (line.size() > 2 && line[0] == 'F' && sscanf(line.c_str() + 2, "%d %d %d %d %lf %lf", &gs, &k, &a, &b, &t1, &t2) == 6) {
            if (gs <= 0 || gs > 127 || k < 2000 || k > 30000 || a < 0 || b < 0 || !std::isfinite(t1 + t2) || t2 < 0) continue;
            F& f = g_[gs].f[k];
            f.hits = a; f.hitsSince = b; f.lastSec = t1; f.listenSince = t2;
        }
    }
}

} // namespace dect2
