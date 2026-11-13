// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB ensemble watch: tells you when a multiplex changes. Fed with the census rows (or one ensemble's service list) on every update; reports
//   - a new ensemble on a block, an ensemble that went away (no signal for `goneSec`),
//   - services added or removed, a service whose bit rate or codec (DAB / DAB+) changed, the ensemble ID or label changed.
// Two guards against false alarms from a weak signal: (1) a change must be seen `confirm` updates in a row before it is reported (a half-received FIC has missing services);
// (2) the first `learnUpdates` updates of an ensemble only build its baseline (the list grows while the FIC arrives).
// Pure; tests/dab40/test_dab_watch.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::dabwatch {

struct Svc { uint32_t sid = 0; std::string label; bool dabPlus = false; int bitrate = 0; };
struct Obs {                       // one ensemble as seen now
    std::string channel;           // "11D"
    bool active = false;           // a signal is there
    uint16_t eid = 0;
    std::string ensemble;
    std::vector<Svc> services;
};
enum class Kind { EnsembleAppeared, EnsembleGone, ServiceAdded, ServiceRemoved, ServiceChanged, EnsembleChanged };
struct Event { Kind kind; std::string channel; std::string text; double at = 0; };

class Watch {
public:
    int confirm = 3;
    int learnUpdates = 6;
    double goneSec = 120;
    // returns the events of this update (also kept in log())
    std::vector<Event> update(double nowSec, const std::vector<Obs>& obs) {
        std::vector<Event> out;
        for (const Obs& o : obs) {
            Ch& c = ch_[o.channel];
            if (!o.active || o.services.empty()) {
                if (c.known && c.present && nowSec - c.lastSeen >= goneSec) {
                    c.present = false;
                    emit(out, {Kind::EnsembleGone, o.channel, "Ensemble " + c.base.ensemble + " is gone from " + o.channel, nowSec});
                }
                c.pending = 0;
                continue;
            }
            c.lastSeen = nowSec;
            if (!c.known || !c.present) {
                const bool again = c.known;
                c.known = true; c.present = true; c.updates = 1; c.base = o; c.pending = 0;
                emit(out, {Kind::EnsembleAppeared, o.channel, std::string(again ? "Ensemble is back on " : "New ensemble on ") + o.channel + ": " + o.ensemble + " (" + std::to_string(o.services.size()) + " services)", nowSec});
                continue;
            }
            c.updates++;
            if (c.updates <= learnUpdates) {   // baseline still growing: keep the union of what is seen
                merge(c.base, o);
                continue;
            }
            const std::vector<Event> diff = compare(c.base, o, nowSec);
            if (diff.empty()) { c.pending = 0; continue; }
            if (diff.size() == c.pendingEvents && sameText(diff, c.pendingList)) c.pending++; else { c.pending = 1; }
            c.pendingEvents = diff.size();
            c.pendingList = diff;
            if (c.pending >= confirm) {
                for (const Event& e : diff) emit(out, e);
                c.base = o;
                c.pending = 0;
                c.pendingEvents = 0;
                c.pendingList.clear();
            }
        }
        return out;
    }
    const std::vector<Event>& log() const { return log_; }
    void clear() { ch_.clear(); log_.clear(); }
private:
    struct Ch { bool known = false, present = false; int updates = 0, pending = 0; size_t pendingEvents = 0; std::vector<Event> pendingList; double lastSeen = 0; Obs base; };
    std::map<std::string, Ch> ch_;
    std::vector<Event> log_;
    void emit(std::vector<Event>& out, const Event& e) { out.push_back(e); log_.push_back(e); if (log_.size() > 500) log_.erase(log_.begin()); }
    static bool sameText(const std::vector<Event>& a, const std::vector<Event>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) if (a[i].text != b[i].text) return false;
        return true;
    }
    static void merge(Obs& base, const Obs& o) {
        base.eid = o.eid; base.ensemble = o.ensemble;
        for (const Svc& s : o.services) {
            auto it = std::find_if(base.services.begin(), base.services.end(), [&](const Svc& x) { return x.sid == s.sid; });
            if (it == base.services.end()) base.services.push_back(s); else *it = s;
        }
    }
    static std::vector<Event> compare(const Obs& base, const Obs& now, double t) {
        std::vector<Event> d;
        if (now.eid != base.eid || now.ensemble != base.ensemble) d.push_back({Kind::EnsembleChanged, now.channel, "Ensemble on " + now.channel + " changed from " + base.ensemble + " to " + now.ensemble, t});
        for (const Svc& s : now.services) {
            auto it = std::find_if(base.services.begin(), base.services.end(), [&](const Svc& x) { return x.sid == s.sid; });
            if (it == base.services.end()) d.push_back({Kind::ServiceAdded, now.channel, "Added on " + now.channel + ": " + s.label + " (" + std::to_string(s.bitrate) + " kbit/s" + (s.dabPlus ? ", DAB+" : "") + ")", t});
            else if (it->bitrate != s.bitrate || it->dabPlus != s.dabPlus) d.push_back({Kind::ServiceChanged, now.channel, s.label + " on " + now.channel + ": " + std::to_string(it->bitrate) + (it->dabPlus ? " DAB+" : " DAB") + " -> " + std::to_string(s.bitrate) + (s.dabPlus ? " DAB+" : " DAB"), t});
        }
        for (const Svc& s : base.services) {
            auto it = std::find_if(now.services.begin(), now.services.end(), [&](const Svc& x) { return x.sid == s.sid; });
            if (it == now.services.end()) d.push_back({Kind::ServiceRemoved, now.channel, "Removed from " + now.channel + ": " + s.label, t});
        }
        return d;
    }
};

}  // namespace dect2::dabwatch
