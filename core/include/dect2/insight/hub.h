// The insight hub (docs/SMART5-*-3.9.md): one place that feeds the ham, voice, pager and HF utility analysers from the
// automation events the decoders already send ("aprs.packet", "pager.message", "session.call") and from FT8 / FT4 / JS8
// decodes, and gives findings back as Insight rows and as automation events "insight.<kind>" (rules, notifications and the
// integrations can use them). Offline; per talkgroup or system, never per radio; nothing is decrypted.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/insight/aprs_health.h"
#include "dect2/insight/ham_heard.h"
#include "dect2/insight/insight.h"
#include "dect2/insight/pager_patterns.h"
#include "dect2/insight/schedule_watch.h"
#include "dect2/insight/voice_activity.h"
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2::insight {

// the voice families: modes whose "session.call" rows are talkgroup calls
inline bool isVoiceMode(const std::string& mode) {
    static const char* const m[] = {"DMR", "P25", "NXDN", "DPMR", "TETRA", "TETRAPOL", "EDACS", "SMARTNET", "MPT1327", "LTR", "D-STAR", "DSTAR",
                                    "FUSION", "YSF", "M17", "TRUNK", "FM", "NFM", "ANALOG"};
    const std::string u = upper(mode);
    for (const char* x : m) if (u.find(x) != std::string::npos) return true;
    return false;
}

inline automation::Event toEvent(const Insight& i) {
    automation::Event e("insight." + i.kind);
    e.time = i.time;
    e.str("area", i.area).str("key", i.key).str("text", i.text).num("severity", i.severity);
    if (i.hasPos) e.num("lat", i.lat).num("lon", i.lon);
    return e;
}

class Hub {
public:
    explicit Hub(int tzOffsetSec = 0) : heat_(tzOffsetSec) {}
    void setStation(const std::string& call, const std::string& grid) { hears_.setStation(call, grid); }
    void setTzOffset(int sec) { heat_.setTzOffset(sec); }

    // an automation event from any decoder; the ones it does not know are ignored
    void onEvent(const automation::Event& e) {
        auto s = [&](const char* k) { auto it = e.f.find(k); return it == e.f.end() ? std::string() : it->second.text(); };
        auto n = [&](const char* k, double d) { auto it = e.f.find(k); return it == e.f.end() || it->second.t != automation::Value::Num ? d : it->second.n; };
        auto b = [&](const char* k) { auto it = e.f.find(k); return it != e.f.end() && it->second.truthy(); };
        if (e.type == "aprs.packet") {
            const bool pos = e.f.count("lat") && e.f.count("lon") && e.f.at("lat").t == automation::Value::Num;
            aprs_.add(e.time, s("from"), s("path"), s("info"), pos, n("lat", 0), n("lon", 0), pending_);
        } else if (e.type == "pager.message") {
            char ch[48];
            snprintf(ch, sizeof ch, "%.4f MHz", e.freqHz / 1e6);
            const std::string proto = s("protocol").empty() ? e.mode : s("protocol");
            pager_.add(e.time, proto + " " + s("address"), ch, s("text"), pending_);
        } else if (e.type == "session.call" && isVoiceMode(e.mode)) {
            const std::string tg = talkgroupOf(s("key"));
            const double dur = n("duration_s", 0), end = e.time, start = end - dur;
            const std::string sys = e.mode;
            char chan[64];
            snprintf(chan, sizeof chan, "%.5f/%s", e.freqHz / 1e6, slotOf(s("key")).c_str());
            trunk_[sys].addCall(start, end, chan);
            heat_.markHeard(start, end);
            if (!tg.empty()) {
                const std::string k = sys + " " + tg;
                heat_.addCall(start, dur, k, &pending_);
                changes_.addCall(end, k, s("verdict") == "encrypted", b("emergency"), pending_);
            }
        } else if ((e.type.rfind("session.", 0) == 0 || e.type.rfind("msg.", 0) == 0) && e.freqHz >= 1e5 && e.freqHz < 30e6 && !isVoiceMode(e.mode)) {
            // an HF / MF utility decode (a chart, a bulletin, a sounding): its schedule per mode and frequency
            sched_.decoded(e.time, e.mode.empty() ? e.type : e.mode, e.freqHz);
        }
    }

    // an FT8 / FT4 / JS8 / FST4 decode: who hears me, the antenna pattern, call checks
    void onFtx(double t, const std::string& band, const std::string& msg, float snr) {
        const bool me = hears_.add(t, band, msg, snr);
        const auto w = words(msg);
        // the sender: "CQ X GRID", "CQ DX X GRID", "A X GRID|report"
        std::string from;
        if (w.size() >= 2 && w[0] == "CQ") from = w.size() >= 3 && !validCall(w[1]) ? w[2] : w[1];
        else if (w.size() >= 2) from = w[1];
        if (!from.empty() && validCall(from)) {
            calls_.add(from);
            double hl, ho, la, lo;
            auto g = hears_.grids().find(from);
            if (g != hears_.grids().end() && hears_.home(hl, ho) && gridToLatLon(g->second, la, lo))
                pattern_.add(bearingDeg(hl, ho, la, lo), distanceKm(hl, ho, la, lo), snr);
        }
        if (me) {
            for (const auto& r : hears_.list()) {
                if (r.lastTime != t || r.times != 1) continue;   // the first time a station is heard hearing you, per band
                Insight i;
                i.time = t; i.area = "ham"; i.kind = "heard_me"; i.key = r.call + "|" + r.band;
                char b[200];
                if (r.distanceKm >= 0) snprintf(b, sizeof b, "%s hears you on %s: %s, %.0f km %s", r.call.c_str(), r.band.c_str(), r.hasReport ? (std::to_string(r.theirReport) + " dB").c_str() : "no report", r.distanceKm, compass8(r.bearing));
                else snprintf(b, sizeof b, "%s hears you on %s: %s", r.call.c_str(), r.band.c_str(), r.hasReport ? (std::to_string(r.theirReport) + " dB").c_str() : "no report");
                i.text = b;
                if (r.distanceKm >= 0) { double la, lo; if (gridToLatLon(r.grid, la, lo)) { i.hasPos = true; i.lat = la; i.lon = lo; } }
                pending_.push_back(i);
            }
        }
    }

    // a decode of an HF utility station (WEFAX, RTTY weather, SITOR, STANAG, ALE, beacons, time signals)
    void onUtility(double t, const std::string& station, double freqHz) { sched_.decoded(t, station, freqHz); }

    // about once a minute: the frequencies the receivers are on
    void tick(double now, const std::vector<double>& tunedHz = {}) {
        aprs_.tick(now, pending_);
        pager_.tick(now, pending_);
        for (auto& kv : trunk_) kv.second.tick(now, kv.first, pending_);
        sched_.tick(now, tunedHz, pending_);
    }

    // a finding from an analyser outside the hub (the smart5 broadcast, TV, satellite and science cards): shown and sent with the others
    void post(const Insight& i) { pending_.push_back(i); }

    // findings since the last call; also kept in recent()
    std::vector<Insight> take() {
        std::vector<Insight> v;
        v.swap(pending_);
        for (const auto& i : v) { recent_.push_back(i); if (recent_.size() > 500) recent_.pop_front(); }
        return v;
    }
    const std::deque<Insight>& recent() const { return recent_; }

    WhoHearsMe& whoHearsMe() { return hears_; }
    AntennaPattern& antennaPattern() { return pattern_; }
    BustedCalls& bustedCalls() { return calls_; }
    AprsHealth& aprsHealth() { return aprs_; }
    PagerPatterns& pagerPatterns() { return pager_; }
    TalkgroupHeatmap& heatmap() { return heat_; }
    TrunkLoad& trunk(const std::string& system) { return trunk_[system]; }
    const std::map<std::string, TrunkLoad>& trunks() const { return trunk_; }
    TalkgroupChanges& talkgroupChanges() { return changes_; }
    ScheduleWatch& schedule() { return sched_; }

private:
    static std::string slotOf(const std::string& key) {
        const size_t p = upper(key).find("/SLOT");
        return p == std::string::npos ? std::string() : key.substr(p + 1);
    }
    WhoHearsMe hears_;
    AntennaPattern pattern_;
    BustedCalls calls_;
    AprsHealth aprs_;
    PagerPatterns pager_;
    TalkgroupHeatmap heat_;
    std::map<std::string, TrunkLoad> trunk_;
    TalkgroupChanges changes_;
    ScheduleWatch sched_;
    std::vector<Insight> pending_;
    std::deque<Insight> recent_;
};

}  // namespace dect2::insight
