// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ensemble change log (see ens_changes.h).
#include "dect2/smart5/ens_changes.h"
#include <cstdio>
#include <set>

namespace dect2 {
namespace smart5 {

void EnsChangeLog::update(const std::vector<EnsService>& list, double now) {
    char b[200];
    std::set<unsigned> seen;
    for (const auto& s : list) {
        seen.insert(s.id);
        missing_.erase(s.id);
        auto it = cur_.find(s.id);
        if (it == cur_.end()) {
            if (!first_) { snprintf(b, sizeof b, "service %04X \"%s\" added (%s%s)", s.id, s.label.c_str(), s.kind.c_str(), s.bitrateKbps ? (", " + std::to_string(s.bitrateKbps) + " kbit/s").c_str() : ""); ev_.push_back({now, s.id, "added", b}); }
            cur_[s.id] = s;
            continue;
        }
        EnsService& o = it->second;
        if (!s.label.empty() && !o.label.empty() && s.label != o.label) { snprintf(b, sizeof b, "%04X label \"%s\" -> \"%s\"", s.id, o.label.c_str(), s.label.c_str()); ev_.push_back({now, s.id, "label", b}); }
        if (s.bitrateKbps && o.bitrateKbps && s.bitrateKbps != o.bitrateKbps) { snprintf(b, sizeof b, "%04X \"%s\" bitrate %d -> %d kbit/s", s.id, s.label.c_str(), o.bitrateKbps, s.bitrateKbps); ev_.push_back({now, s.id, "bitrate", b}); }
        if (!s.protection.empty() && !o.protection.empty() && s.protection != o.protection) { snprintf(b, sizeof b, "%04X \"%s\" protection %s -> %s", s.id, s.label.c_str(), o.protection.c_str(), s.protection.c_str()); ev_.push_back({now, s.id, "protection", b}); }
        if (s.subch >= 0 && o.subch >= 0 && s.subch != o.subch) { snprintf(b, sizeof b, "%04X \"%s\" moved from sub-channel %d to %d", s.id, s.label.c_str(), o.subch, s.subch); ev_.push_back({now, s.id, "moved", b}); }
        if (!s.kind.empty() && !o.kind.empty() && s.kind != o.kind) { snprintf(b, sizeof b, "%04X \"%s\" now %s (was %s)", s.id, s.label.c_str(), s.kind.c_str(), o.kind.c_str()); ev_.push_back({now, s.id, "kind", b}); }
        EnsService m = s;
        if (m.label.empty()) m.label = o.label;
        if (!m.bitrateKbps) m.bitrateKbps = o.bitrateKbps;
        if (m.subch < 0) m.subch = o.subch;
        if (m.protection.empty()) m.protection = o.protection;
        if (m.kind.empty()) m.kind = o.kind;
        o = m;
    }
    for (auto it = cur_.begin(); it != cur_.end();) {
        if (seen.count(it->first)) { ++it; continue; }
        if (++missing_[it->first] >= miss_) {
            snprintf(b, sizeof b, "service %04X \"%s\" removed", it->first, it->second.label.c_str());
            ev_.push_back({now, it->first, "removed", b});
            missing_.erase(it->first);
            it = cur_.erase(it);
        } else ++it;
    }
    if (!list.empty()) first_ = false;
    if (ev_.size() > 2000) ev_.erase(ev_.begin(), ev_.begin() + 500);
}

std::string EnsChangeLog::csv() const {
    std::string o = "time,id,change,text\n";
    char b[64];
    for (auto& e : ev_) {
        snprintf(b, sizeof b, "%.0f,%04X,", e.t, e.id);
        std::string t = e.text;
        for (auto& c : t) if (c == '"') c = '\'';
        o += b + e.what + ",\"" + t + "\"\n";
    }
    return o;
}

} // namespace smart5
} // namespace dect2
