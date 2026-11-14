// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/cell/cell_ocid.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>
#include <vector>
#include "dect2/common/diag.h"

namespace dect2 {

bool OcidCellKey::operator<(const OcidCellKey& o) const { return std::tie(mcc, mnc, lac, cell, radio) < std::tie(o.mcc, o.mnc, o.lac, o.cell, o.radio); }

namespace {
std::string enc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
// the value after "name": in a flat JSON object (a number or a string), false when absent
bool field(const std::string& j, const char* name, std::string& v) {
    const std::string k = std::string("\"") + name + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < j.size() && isspace((unsigned char)j[p])) p++;
    if (p < j.size() && j[p] == '"') {
        const size_t e = j.find('"', p + 1);
        if (e == std::string::npos) return false;
        v = j.substr(p + 1, e - p - 1);
        return true;
    }
    size_t e = p;
    while (e < j.size() && j[e] != ',' && j[e] != '}' && !isspace((unsigned char)j[e])) e++;
    v = j.substr(p, e - p);
    return !v.empty() && v != "null";
}
const char* statusName(OcidStatus s) { return s == OcidStatus::Found ? "found" : "notfound"; }
} // namespace

std::string ocidUrl(const std::string& key, const OcidCellKey& k) {
    char b[160];
    snprintf(b, sizeof b, "&mcc=%d&mnc=%d&lac=%u&cellid=%u&radio=%s&format=json", k.mcc, k.mnc, k.lac, k.cell, enc(k.radio).c_str());
    return "https://opencellid.org/cell/get?key=" + enc(key) + b;
}

OcidResult ocidParse(const std::string& body) {
    OcidResult r;
    std::string v, lat, lon;
    if (field(body, "error", v) || field(body, "code", lat)) {
        r.message = v;
        int code = 0; std::string c; if (field(body, "code", c)) code = atoi(c.c_str());
        std::string low = v; for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        if (code == 1 || low.find("not found") != std::string::npos) r.status = OcidStatus::NotFound;
        else if (code == 2 || low.find("key") != std::string::npos) r.status = OcidStatus::BadKey;
        else if (code == 6 || code == 7 || low.find("limit") != std::string::npos || low.find("too many") != std::string::npos) r.status = OcidStatus::Limit;
        else r.status = OcidStatus::Error;
        return r;
    }
    if (!field(body, "lat", lat) || !field(body, "lon", lon)) { r.message = "unexpected answer"; return r; }
    r.lat = atof(lat.c_str()); r.lon = atof(lon.c_str());
    if (field(body, "range", v)) r.range = atoi(v.c_str());
    if (field(body, "samples", v)) r.samples = atoi(v.c_str());
    r.status = OcidStatus::Found;
    return r;
}

const OcidEntry* OcidCache::get(const OcidCellKey& k, int64_t now) const {
    auto it = m_.find(k);
    if (it == m_.end()) return nullptr;
    if (it->second.r.status == OcidStatus::NotFound && now - it->second.time >= kNotFoundSecs) return nullptr;
    return &it->second;
}

const OcidEntry* OcidCache::findSite(const OcidCellKey& k) const {
    if (k.radio != "LTE") return nullptr;
    for (const auto& kv : m_) {
        const OcidCellKey& o = kv.first;
        if (kv.second.r.status == OcidStatus::Found && o.radio == k.radio && o.mcc == k.mcc && o.mnc == k.mnc && (o.cell >> 8) == (k.cell >> 8)) return &kv.second;
    }
    return nullptr;
}

void OcidCache::put(const OcidCellKey& k, const OcidResult& r, int64_t now) {
    if (r.status != OcidStatus::Found && r.status != OcidStatus::NotFound) return;
    m_[k] = {r, now};
}

// radio,mcc,mnc,lac,cell,status,lat,lon,range,samples,time
bool OcidCache::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> c; std::stringstream ss(line); std::string v;
        while (std::getline(ss, v, ',')) c.push_back(v);
        if (c.size() < 11) continue;
        OcidCellKey k; k.radio = c[0]; k.mcc = atoi(c[1].c_str()); k.mnc = atoi(c[2].c_str());
        k.lac = (uint32_t)strtoul(c[3].c_str(), nullptr, 10); k.cell = (uint32_t)strtoul(c[4].c_str(), nullptr, 10);
        OcidEntry e;
        e.r.status = c[5] == "found" ? OcidStatus::Found : OcidStatus::NotFound;
        e.r.lat = atof(c[6].c_str()); e.r.lon = atof(c[7].c_str()); e.r.range = atoi(c[8].c_str()); e.r.samples = atoi(c[9].c_str());
        e.time = strtoll(c[10].c_str(), nullptr, 10);
        m_[k] = e;
    }
    return true;
}

bool OcidCache::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { DIAG(109542, "The cell lookup cache file %s could not be created, so the cache was not saved.", tmp.c_str()); return false; }
        char b[256];
        for (const auto& kv : m_) {
            const auto& k = kv.first; const auto& e = kv.second;
            snprintf(b, sizeof b, "%s,%d,%d,%u,%u,%s,%.6f,%.6f,%d,%d,%lld\n", k.radio.c_str(), k.mcc, k.mnc, k.lac, k.cell, statusName(e.r.status), e.r.lat, e.r.lon,
                     e.r.range, e.r.samples, (long long)e.time);
            f << b;
        }
        if (!f) return false;
    }
    // std::rename fails on Windows when the target exists; std::filesystem::rename replaces it there too
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

OcidLookup::OcidLookup(Fetch fetch, std::string cachePath) : fetch_(std::move(fetch)), path_(std::move(cachePath)) {
    if (!path_.empty()) cache_.load(path_);
    th_ = dect2::namedThread("onair-ocid", [this] { run(); });
}

OcidLookup::~OcidLookup() {
    { std::lock_guard<std::mutex> l(mu_); stop_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void OcidLookup::setKey(const std::string& key) {
    std::lock_guard<std::mutex> l(mu_);
    if (key == key_) return;
    key_ = key; halted_ = false; status_.clear();
    cv_.notify_all();
}

void OcidLookup::request(const OcidCellKey& k) {
    std::lock_guard<std::mutex> l(mu_);
    if (key_.empty() || halted_ || queued_.count(k) || cache_.get(k, (int64_t)std::time(nullptr))) return;
    queued_.insert(k); q_.push_back(k);
    cv_.notify_all();
}

bool OcidLookup::lookup(const OcidCellKey& k, OcidEntry& e) {
    std::lock_guard<std::mutex> l(mu_);
    const OcidEntry* p = cache_.get(k, (int64_t)std::time(nullptr));
    if (!p || p->r.status != OcidStatus::Found) { if (const OcidEntry* s = cache_.findSite(k)) p = s; }   // the site's position from another sector
    if (!p) return false;
    e = *p;
    return true;
}

std::string OcidLookup::status() { std::lock_guard<std::mutex> l(mu_); return status_; }
size_t OcidLookup::pending() { std::lock_guard<std::mutex> l(mu_); return q_.size(); }

void OcidLookup::run() {
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    OcidRateLimiter rl(1.0);
    std::unique_lock<std::mutex> l(mu_);
    while (!stop_) {
        if (q_.empty() || key_.empty() || halted_) { cv_.wait(l); continue; }
        if (probes_.count(q_.front()) && cache_.findSite(q_.front())) { queued_.erase(q_.front()); q_.pop_front(); continue; }   // the site is placed already
        const double now = std::chrono::duration<double>(clk::now() - t0).count();
        const double w = rl.waitFor(now);
        if (w > 0) { cv_.wait_for(l, std::chrono::duration<double>(w)); continue; }
        rl.tryTake(now);
        const OcidCellKey k = q_.front();
        const std::string url = ocidUrl(key_, k);
        l.unlock();
        std::string body;
        const bool ok = fetch_(url, body);
        const OcidResult r = ok ? ocidParse(body) : OcidResult{};
        l.lock();
        q_.pop_front(); queued_.erase(k);
        char b[160];
        if (!ok) { snprintf(b, sizeof b, "no answer from opencellid.org (offline?)"); q_.clear(); queued_.clear(); }
        else switch (r.status) {
            case OcidStatus::Found:
                if (probes_.count(k)) snprintf(b, sizeof b, "site %u placed from its sector %u: %d samples, range %d m", k.cell >> 8, k.cell & 255, r.samples, r.range);
                else snprintf(b, sizeof b, "found cell %u: %d samples, range %d m", k.cell, r.samples, r.range);
                break;
            case OcidStatus::NotFound:
                if (probes_.count(k)) snprintf(b, sizeof b, "site %u: sector %u not in the database either", k.cell >> 8, k.cell & 255);
                else snprintf(b, sizeof b, "cell %u is not in the database: trying the other sectors of site %u", k.cell, k.cell >> 8);
                break;
            case OcidStatus::BadKey: snprintf(b, sizeof b, "the API key was refused: check it in the key field"); halted_ = true; break;
            case OcidStatus::Limit: snprintf(b, sizeof b, "daily lookup limit reached: try again tomorrow"); halted_ = true; break;
            default: snprintf(b, sizeof b, "error: %s", r.message.empty() ? "unexpected answer" : r.message.c_str()); break;
        }
        status_ = b;
        if (r.status == OcidStatus::Found || r.status == OcidStatus::NotFound) {
            cache_.put(k, r, (int64_t)std::time(nullptr));
            if (!path_.empty()) cache_.save(path_);
        }
        // not found: ask for the usual sector numbers of the same site (operators number them 1-6, 11-16, 21-26 ...), which share its mast
        if (r.status == OcidStatus::NotFound && !probes_.count(k) && k.radio == "LTE" && !cache_.findSite(k)) {
            static const unsigned kSectors[] = {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 16, 21, 22, 23, 31, 32, 33, 41, 42, 43, 0};
            const int64_t t = (int64_t)std::time(nullptr);
            for (unsigned s : kSectors) {
                OcidCellKey p = k; p.cell = (k.cell & ~255u) | s;
                if (p.cell == k.cell || queued_.count(p) || cache_.get(p, t)) continue;
                probes_.insert(p); queued_.insert(p); q_.push_back(p);
            }
        }
        if (halted_) { q_.clear(); queued_.clear(); }
    }
}

} // namespace dect2
