// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tracker book (see bletrk_book.h).
#include "dect2/bletrk/bletrk_book.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace dect2 {

namespace {
uint32_t hash32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h ? h : 1;
}
std::string tail(const std::string& addr) { return addr.size() >= 5 ? addr.substr(addr.size() - 5) : addr; }
}

struct BletrkBook::Impl {
    struct Rec {
        BletrkTracker t;
        std::string addr;                // the current address (memory only)
        std::string tailTxt;
        double bucketT = -1;             // the last whole second counted
        uint32_t secs = 0, nearSecs = 0;
        int64_t lastMinute = -1;
        uint32_t minutes = 0;
        double histT0 = -1; double histSum = 0; int histN = 0;
        bool silentLinked = false;       // an address taken over by a successor
    };
    mutable std::mutex mu;
    BletrkConfig cfg;
    std::vector<Rec> recs;
    std::vector<BletrkAlert> alerts;
    std::unordered_map<std::string, size_t> idx;
    uint32_t seq = 0;

    void reindex() { idx.clear(); for (size_t i = 0; i < recs.size(); i++) if (!recs[i].silentLinked) idx[recs[i].addr] = i; }
    void refresh(Rec& r) {
        char b[96];
        snprintf(b, sizeof b, "%s ..%s", bletrk::kindShort(r.t.kind), r.tailTxt.c_str());
        r.t.label = b;
    }
    int proximityOf(float avg) const { return avg >= cfg.nearDbfs ? 2 : avg >= cfg.nearDbfs - 6 ? 1 : 0; }
};

BletrkBook::BletrkBook() : p_(std::make_unique<Impl>()) {}
BletrkBook::~BletrkBook() = default;
void BletrkBook::setConfig(const BletrkConfig& c) { std::lock_guard<std::mutex> lk(p_->mu); p_->cfg = c; }
BletrkConfig BletrkBook::config() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }
void BletrkBook::clear() { std::lock_guard<std::mutex> lk(p_->mu); p_->recs.clear(); p_->alerts.clear(); p_->idx.clear(); }

void BletrkBook::ignore(uint32_t id, bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& r : p_->recs) if (r.t.id == id) r.t.ignored = on;
}

void BletrkBook::hear(const std::string& addr, const bletrk::Detection& d, float rssi, double sec, int64_t wall) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    auto it = m.idx.find(addr);
    size_t at;
    if (it != m.idx.end()) at = it->second;
    else {
        // a new advertiser: the continuation of a tracker of the same kind that has just gone silent (rotating address)?
        int link = -1; double bestGap = 1e9;
        for (size_t i = 0; i < m.recs.size(); i++) {
            const auto& o = m.recs[i];
            if (o.silentLinked || o.t.kind != d.kind) continue;
            const double gap = sec - o.t.lastSec;
            if (gap < 0 || gap > m.cfg.linkGapSec) continue;
            if (std::fabs(o.t.rssiAvg - rssi) > m.cfg.linkDb) continue;
            if (gap < bestGap) { bestGap = gap; link = (int)i; }
        }
        if (link >= 0 && bestGap >= 1.0) {   // a still-talking old address is a second tag, not a rotation
            Impl::Rec& o = m.recs[(size_t)link];
            o.silentLinked = true;
            Impl::Rec n = o;
            n.silentLinked = false;
            n.addr = addr; n.tailTxt = tail(addr);
            n.t.addresses++;
            n.t.id = hash32(addr + "#" + std::to_string(n.t.addresses));
            n.t.rssi = rssi;
            o.t.lastSec = -1e9;              // the old record goes at the next tick
            m.recs.push_back(std::move(n));
        } else {
            if (m.recs.size() >= kBletrkMax) {
                size_t old = 0;
                for (size_t i = 1; i < m.recs.size(); i++) if (m.recs[i].t.lastSec < m.recs[old].t.lastSec) old = i;
                m.recs.erase(m.recs.begin() + (ptrdiff_t)old);
            }
            Impl::Rec n;
            n.addr = addr; n.tailTxt = tail(addr);
            n.t.id = hash32(addr);
            n.t.kind = d.kind;
            n.t.firstSec = sec; n.t.firstWall = wall;
            n.t.rssiAvg = rssi; n.t.rssiPeak = rssi;
            m.recs.push_back(std::move(n));
        }
        m.reindex();
        at = m.idx[addr];
    }
    Impl::Rec& r = m.recs[at];
    BletrkTracker& t = r.t;
    t.kind = d.kind;
    if (d.separated) t.separated = true;
    if (d.status >= 0) t.status = d.status;
    t.rssi = rssi;
    t.rssiAvg = t.packets == 0 ? rssi : 0.9f * t.rssiAvg + 0.1f * rssi;
    t.rssiPeak = std::max(t.rssiPeak, rssi);
    t.packets++;
    t.lastSec = sec; t.lastWall = wall;
    // seconds and minutes heard
    const double sb = std::floor(sec);
    if (sb != r.bucketT) { r.bucketT = sb; r.secs++; if (t.rssiAvg >= m.cfg.nearDbfs) r.nearSecs++; }
    const int64_t mn = (int64_t)(sec / 60);
    if (mn != r.lastMinute) { r.lastMinute = mn; r.minutes++; }
    // a level every 10 s
    if (r.histT0 < 0) r.histT0 = sec;
    r.histSum += rssi; r.histN++;
    if (sec - r.histT0 >= 10) {
        t.rssiHist.push_back((float)(r.histSum / r.histN));
        if (t.rssiHist.size() > kBletrkHist) t.rssiHist.erase(t.rssiHist.begin());
        r.histT0 = sec; r.histSum = 0; r.histN = 0;
    }
    m.refresh(r);
}

std::vector<BletrkAlert> BletrkBook::tick(double sec, int64_t wall) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    std::vector<BletrkAlert> fresh;
    bool drop = false;
    for (auto& r : m.recs) {
        BletrkTracker& t = r.t;
        if (sec - t.lastSec > m.cfg.forgetSec || r.silentLinked) { drop = true; continue; }
        t.proximity = m.proximityOf(t.rssiAvg);
        t.nearFrac = r.secs ? (float)r.nearSecs / (float)r.secs : 0.f;
        const double span = (double)(int64_t)(t.lastSec / 60) - (double)(int64_t)(t.firstSec / 60) + 1;
        t.presence = span > 0 ? (float)std::min(1.0, r.minutes / span) : 0.f;
        const size_t h = t.rssiHist.size();
        if (h >= 4) {
            const size_t k = std::min<size_t>(3, h / 2);
            float a = 0, b = 0;
            for (size_t i = 0; i < k; i++) { a += t.rssiHist[h - 1 - i]; b += t.rssiHist[h - 1 - k - i]; }
            const float dd = (a - b) / (float)k;
            t.trend = dd >= 3 ? 1 : dd <= -3 ? -1 : 0;
        }
        const double dwell = t.lastSec - t.firstSec;
        const bool recentlyHeard = sec - t.lastSec < 60;
        t.following = !t.ignored && recentlyHeard && dwell >= m.cfg.alertSec && t.presence >= m.cfg.minPresence && t.nearFrac >= m.cfg.minNear;
        if (t.following && !t.alerted) {
            t.alerted = true;
            BletrkAlert a;
            a.wallTime = wall; a.sec = sec; a.kind = t.kind; a.minutes = dwell / 60; a.rssiAvg = t.rssiAvg;
            char b[200];
            snprintf(b, sizeof b, "Unwanted tracking: a %s tracker has stayed near you for %.0f min (%d address%s)", bletrk::kindName(t.kind), a.minutes,
                     t.addresses, t.addresses == 1 ? "" : "es");
            a.text = b;
            m.alerts.insert(m.alerts.begin(), a);
            if (m.alerts.size() > kBletrkAlertMax) m.alerts.pop_back();
            fresh.push_back(std::move(a));
        }
    }
    if (drop) {
        m.recs.erase(std::remove_if(m.recs.begin(), m.recs.end(), [&](const Impl::Rec& r) { return r.silentLinked || sec - r.t.lastSec > m.cfg.forgetSec; }), m.recs.end());
        m.reindex();
    }
    return fresh;
}

void BletrkBook::fill(BletrkTelemetry& out, double sec) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl& m = *p_;
    out.trackers.clear();
    for (int i = 0; i < bletrk::kKinds; i++) out.nowByKind[i] = out.windowByKind[i] = 0;
    out.followingNow = 0;
    for (const auto& r : m.recs) {
        if (r.silentLinked) continue;
        out.trackers.push_back(r.t);
        const int k = std::max(0, std::min(bletrk::kKinds - 1, r.t.kind));
        out.windowByKind[k]++;
        if (sec - r.t.lastSec < 60) out.nowByKind[k]++;
        if (r.t.following) out.followingNow++;
    }
    std::stable_sort(out.trackers.begin(), out.trackers.end(), [](const BletrkTracker& a, const BletrkTracker& b) {
        if (a.following != b.following) return a.following;
        return (a.lastSec - a.firstSec) > (b.lastSec - b.firstSec);
    });
    out.alerts = m.alerts;
}

} // namespace dect2
