// Two-way and trunked voice insights (docs/SMART5-voice-3.9.md), per talkgroup or per system, never per radio:
//  TalkgroupHeatmap - weekday x hour airtime per talkgroup, decayed over weeks; next busy hour; "busier than usual now"
//  TrunkLoad        - concurrent calls, offered traffic in Erlangs per hour, Erlang-B blocking for the voice channels seen
//  TalkgroupChanges - clear <-> encrypted, a dormant talkgroup waking up, emergency calls clustering on one talkgroup
// Fed by the end-of-call summaries (automation event "session.call"): talkgroup key, end time, duration, verdict, emergency.
#pragma once
#include "dect2/insight/insight.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::insight {

// "TG 1234/slot1" -> "TG 1234"; group keys of EDACS / MPT1327 / analog channels as they are; "" for a private call or a radio id
inline std::string talkgroupOf(const std::string& key) {
    const std::string u = upper(key);
    if (u.find("PRIVATE") != std::string::npos || u.find("UNIT") != std::string::npos || u.find("RADIO") != std::string::npos ||
        u.find("INDIVIDUAL") != std::string::npos || u.find("ISSI") != std::string::npos)
        return {};
    std::string k = key;
    const size_t s = upper(k).find("/SLOT");
    if (s != std::string::npos) k = k.substr(0, s);
    if (u.rfind("TG", 0) == 0 || u.find("GROUP") != std::string::npos || u.find("GSSI") != std::string::npos || u.find("CHANNEL") != std::string::npos ||
        u.find("REFLECTOR") != std::string::npos || u.find("ROOM") != std::string::npos || u.find("CH ") != std::string::npos)
        return k;
    return {};
}

// Unix seconds -> weekday (0 Monday .. 6 Sunday) and hour, with the user's offset from UTC
inline int weekHour(double t, int tzOffsetSec) {
    const double lt = t + tzOffsetSec;
    const long long day = (long long)std::floor(lt / 86400.0);
    const int wd = (int)(((day + 3) % 7 + 7) % 7);   // 1970-01-01 was a Thursday (3 when Monday is 0)
    const int hr = (int)std::floor(std::fmod(std::fmod(lt, 86400.0) + 86400.0, 86400.0) / 3600.0);
    return wd * 24 + hr;
}

inline const char* weekdayName(int wd) {
    static const char* const n[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    return n[((wd % 7) + 7) % 7];
}

// ------------------------------------------------------------------------------------------------------------------
class TalkgroupHeatmap {
public:
    static constexpr int kSlots = 7 * 24;
    struct Limits {
        double halfLifeWeeks = 4;     // older weeks count half after this
        double busyFactor = 3;        // this many times the usual airtime of the hour ...
        double busyMinSec = 120;      // ... and at least this much
        double minExposure = 2;       // the hour of the week must have been listened to this many (decayed) weeks
    };
    explicit TalkgroupHeatmap(int tzOffsetSec = 0) : tz_(tzOffsetSec) {}
    TalkgroupHeatmap(int tzOffsetSec, Limits l) : L(l), tz_(tzOffsetSec) {}
    void setTzOffset(int sec) { tz_ = sec; }

    // a call that ran start .. start + dur; the airtime is split over the hours it spans
    void addCall(double start, double dur, const std::string& tg, std::vector<Insight>* out = nullptr) {
        if (tg.empty() || !(dur >= 0) || !std::isfinite(start)) return;
        decayTo(start + dur);
        markHeard(start, start + dur);
        auto& h = tg_[tg];
        double t = start;
        const double end = start + std::max(dur, 0.5);
        while (t < end) {
            const double hourEnd = (std::floor((t + tz_) / 3600.0) + 1) * 3600.0 - tz_;
            const double piece = std::min(end, hourEnd) - t;
            h.air[(size_t)weekHour(t, tz_)] += piece;
            curAdd(h, t, piece);
            t = std::min(end, hourEnd);
        }
        h.calls++;
        h.last = end;
        if (out) checkBusy(tg, h, end, *out);
    }
    // the receiver was listening during start .. end (a call on any talkgroup of the system also says so)
    void markHeard(double start, double end) {
        for (double t = std::floor((start + tz_) / 3600.0) * 3600.0 - tz_; t < end; t += 3600.0) {
            const long long hourIdx = (long long)std::floor((t + tz_) / 3600.0);
            if (heardHours_.insert(hourIdx).second) {
                exposure_[(size_t)weekHour(t + 1, tz_)] += 1;
                if (heardHours_.size() > 24 * 7 * 60) heardHours_.erase(heardHours_.begin());
            }
        }
    }
    // the usual airtime of this talkgroup in the hour of the week of t (seconds per hour; -1 when that hour was hardly listened to)
    double expected(const std::string& tg, double t) const {
        auto it = tg_.find(tg);
        const int s = weekHour(t, tz_);
        if (it == tg_.end() || exposure_[(size_t)s] < L.minExposure) return -1;
        return it->second.air[(size_t)s] / exposure_[(size_t)s];
    }
    // the start of the next hour (after now) this talkgroup is usually busy: at least half its busiest hour and 60 s; 0 if none
    double nextBusy(const std::string& tg, double now) const {
        auto it = tg_.find(tg);
        if (it == tg_.end()) return 0;
        double peak = 0;
        for (int s = 0; s < kSlots; s++) if (exposure_[(size_t)s] >= 1) peak = std::max(peak, it->second.air[(size_t)s] / exposure_[(size_t)s]);
        const double lim = std::max(60.0, peak * 0.5);
        const double h0 = (std::floor((now + tz_) / 3600.0) + 1) * 3600.0 - tz_;
        for (int k = 0; k < kSlots; k++) {
            const double t = h0 + k * 3600.0;
            const int s = weekHour(t + 1, tz_);
            if (exposure_[(size_t)s] >= 1 && it->second.air[(size_t)s] / exposure_[(size_t)s] >= lim) return t;
        }
        return 0;
    }
    // the hours of the week, busiest first (for a heatmap legend)
    std::vector<std::pair<int, double>> busiest(const std::string& tg, int n = 3) const {
        std::vector<std::pair<int, double>> v;
        auto it = tg_.find(tg);
        if (it == tg_.end()) return v;
        for (int s = 0; s < kSlots; s++) if (exposure_[(size_t)s] >= 1) v.push_back({s, it->second.air[(size_t)s] / exposure_[(size_t)s]});
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if ((int)v.size() > n) v.resize((size_t)n);
        return v;
    }
    // seconds per hour of the week, normalised by how often each hour was listened to (the heatmap itself)
    std::array<double, kSlots> grid(const std::string& tg) const {
        std::array<double, kSlots> g{};
        auto it = tg_.find(tg);
        if (it == tg_.end()) return g;
        for (int s = 0; s < kSlots; s++) g[(size_t)s] = exposure_[(size_t)s] > 0 ? it->second.air[(size_t)s] / exposure_[(size_t)s] : 0;
        return g;
    }
    std::vector<std::string> talkgroups() const { std::vector<std::string> v; for (auto& kv : tg_) v.push_back(kv.first); return v; }

    // kept between sessions: "E slot value", "T name|slot|value", "W week"
    std::string save() const {
        std::ostringstream o;
        o.precision(10);
        o << "W " << week_ << "\n";
        for (int s = 0; s < kSlots; s++) if (exposure_[(size_t)s] > 0) o << "E " << s << " " << exposure_[(size_t)s] << "\n";
        for (auto& kv : tg_)
            for (int s = 0; s < kSlots; s++) if (kv.second.air[(size_t)s] > 0) o << "T " << kv.first << "|" << s << "|" << kv.second.air[(size_t)s] << "\n";
        return o.str();
    }
    bool load(const std::string& text) {
        std::istringstream is(text);
        std::string line;
        while (std::getline(is, line)) {
            if (line.size() < 3) continue;
            if (line[0] == 'W') week_ = std::atoll(line.c_str() + 2);
            else if (line[0] == 'E') { int s = 0; double v = 0; if (sscanf(line.c_str() + 2, "%d %lf", &s, &v) == 2 && s >= 0 && s < kSlots) exposure_[(size_t)s] = v; }
            else if (line[0] == 'T') {
                const size_t a = line.rfind('|'), b = a == std::string::npos ? a : line.rfind('|', a - 1);
                if (b == std::string::npos || b < 2) continue;
                const int s = std::atoi(line.c_str() + b + 1);
                if (s >= 0 && s < kSlots) tg_[line.substr(2, b - 2)].air[(size_t)s] = std::atof(line.c_str() + a + 1);
            }
        }
        return true;
    }

private:
    struct Tg {
        std::array<double, kSlots> air{};
        long long curHour = -1;
        double curAir = 0;
        bool saidThisHour = false;
        int calls = 0;
        double last = 0;
    };
    void curAdd(Tg& h, double t, double piece) {
        const long long hi = (long long)std::floor((t + tz_) / 3600.0);
        if (hi != h.curHour) { h.curHour = hi; h.curAir = 0; h.saidThisHour = false; }
        h.curAir += piece;
    }
    void checkBusy(const std::string& tg, Tg& h, double t, std::vector<Insight>& out) {
        if (h.saidThisHour) return;
        const int s = weekHour(t - 1, tz_);
        if (exposure_[(size_t)s] < L.minExposure + 1) return;   // this hour counts itself once already
        // the usual airtime without this hour's own share
        const double usual = std::max(0.0, h.air[(size_t)s] - h.curAir) / std::max(1.0, exposure_[(size_t)s] - 1);
        if (h.curAir >= L.busyMinSec && h.curAir > L.busyFactor * usual) {
            h.saidThisHour = true;
            Insight i;
            i.time = t; i.area = "voice"; i.kind = "voice_busy"; i.severity = 0; i.key = tg;
            char b[160];
            snprintf(b, sizeof b, "%s busier than usual: %.0f s of calls this hour (usually %.0f s, %s %02d:00)", tg.c_str(), h.curAir, usual, weekdayName(s / 24), s % 24);
            i.text = b;
            out.push_back(i);
        }
    }
    void decayTo(double t) {
        const long long w = (long long)std::floor((t + tz_) / (7 * 86400.0));
        if (week_ == 0) { week_ = w; return; }
        if (w <= week_) return;
        const double f = std::pow(0.5, (double)(w - week_) / L.halfLifeWeeks);
        for (auto& e : exposure_) e *= f;
        for (auto& kv : tg_) for (auto& a : kv.second.air) a *= f;
        week_ = w;
    }

    Limits L;
    int tz_ = 0;
    long long week_ = 0;
    std::array<double, kSlots> exposure_{};
    std::set<long long> heardHours_;
    std::map<std::string, Tg> tg_;
};

// ------------------------------------------------------------------------------------------------------------------
// Erlang B: the blocking probability of n channels offered a Erlangs
inline double erlangB(int n, double a) {
    if (n <= 0) return 1;
    if (a <= 0) return 0;
    double b = 1;
    for (int k = 1; k <= n; k++) b = a * b / (k + a * b);
    return b;
}

// the offered traffic that n channels carry as `carried` Erlangs (blocked calls are never heard, so carried = a (1 - B(n, a)))
inline double offeredFromCarried(int n, double carried) {
    if (n <= 0 || carried <= 0) return 0;
    if (carried >= n * 0.999) return n * 10.0;   // saturated: offered traffic is unknown, report it as very high
    double lo = carried, hi = std::max(carried * 2, 1.0);
    while (hi * (1 - erlangB(n, hi)) < carried && hi < n * 1000) hi *= 2;
    for (int i = 0; i < 80; i++) {
        const double m = (lo + hi) / 2;
        (m * (1 - erlangB(n, m)) < carried ? lo : hi) = m;
    }
    return (lo + hi) / 2;
}

struct TrunkHour {
    double start = 0;             // the hour's start (Unix seconds)
    int calls = 0;
    double carriedErl = 0;        // airtime / 3600
    double offeredErl = 0;        // estimated with Erlang B for the channels seen
    double blocking = 0;          // Erlang-B blocking at the offered traffic
    int peakConcurrent = 0;
    int channels = 0;
};

class TrunkLoad {
public:
    struct Limits { double nearCapacity = 0.05; double settleSec = 300; };
    TrunkLoad() {}
    explicit TrunkLoad(Limits l) : L(l) {}
    void setChannels(int n) { fixedChannels_ = n; }   // the voice channels of the system when known; else the ones seen
    // one call; channel is any stable id of the voice channel (frequency and slot)
    void addCall(double start, double end, const std::string& channel) {
        if (!(end >= start)) return;
        calls_.push_back({start, end});
        if (!channel.empty()) channels_.insert(channel);
        if (calls_.size() > 200000) calls_.pop_front();
    }
    int channels() const { return fixedChannels_ > 0 ? fixedChannels_ : (int)channels_.size(); }
    // the hour starting at h0, from the calls known
    TrunkHour hour(double h0) const {
        TrunkHour r;
        r.start = h0; r.channels = channels();
        const double h1 = h0 + 3600;
        std::vector<std::pair<double, int>> ev;
        double air = 0;
        for (const auto& c : calls_) {
            const double a = std::max(c.first, h0), b = std::min(c.second, h1);
            if (b <= a) continue;
            air += b - a;
            if (c.first >= h0 && c.first < h1) r.calls++;
            ev.push_back({a, +1});
            ev.push_back({b, -1});
        }
        std::sort(ev.begin(), ev.end());
        int cur = 0;
        for (auto& e : ev) { cur += e.second; r.peakConcurrent = std::max(r.peakConcurrent, cur); }
        r.carriedErl = air / 3600.0;
        r.offeredErl = offeredFromCarried(std::max(r.channels, r.peakConcurrent), r.carriedErl);
        r.blocking = erlangB(std::max(r.channels, r.peakConcurrent), r.offeredErl);
        return r;
    }
    // closes the hours that ended at least settleSec ago; reports "near capacity" once per hour
    void tick(double now, const std::string& system, std::vector<Insight>& out) {
        const double lastClosed = std::floor((now - L.settleSec) / 3600.0) * 3600.0 - 3600.0;
        if (lastClosed <= lastReported_) return;
        if (lastReported_ == 0 && !calls_.empty()) lastReported_ = std::floor(calls_.front().first / 3600.0) * 3600.0 - 3600.0;
        for (double h = lastReported_ + 3600; h <= lastClosed; h += 3600) {
            TrunkHour r = hour(h);
            hours_.push_back(r);
            if (hours_.size() > 24 * 60) hours_.pop_front();
            if (r.calls > 0 && r.blocking > L.nearCapacity) {
                Insight i;
                i.time = now; i.area = "voice"; i.kind = "trunk_load"; i.severity = 1; i.key = system;
                char b[200];
                snprintf(b, sizeof b, "%s near capacity: %.1f Erlang offered on %d channels, %.0f %% of calls likely blocked (peak %d at once)",
                         system.c_str(), r.offeredErl, std::max(r.channels, r.peakConcurrent), r.blocking * 100, r.peakConcurrent);
                i.text = b;
                out.push_back(i);
            }
        }
        lastReported_ = lastClosed;
    }
    const std::deque<TrunkHour>& hours() const { return hours_; }

private:
    Limits L;
    int fixedChannels_ = 0;
    std::set<std::string> channels_;
    std::deque<std::pair<double, double>> calls_;
    std::deque<TrunkHour> hours_;
    double lastReported_ = 0;
};

// ------------------------------------------------------------------------------------------------------------------
class TalkgroupChanges {
public:
    struct Limits {
        int stableCalls = 3;              // calls in one state before a change of it is news
        double dormantSec = 7 * 86400;    // silent this long and then heard: woke up
        int dormantMinCalls = 3;          // ... if it was used before
        double emergencyWindowSec = 600;
        int emergencyCalls = 2;
    };
    TalkgroupChanges() {}
    explicit TalkgroupChanges(Limits l) : L(l) {}
    void addCall(double end, const std::string& tg, bool encrypted, bool emergency, std::vector<Insight>& out) {
        if (tg.empty()) return;
        St& s = st_[tg];
        char b[200];
        // dormant talkgroup waking up
        if (s.calls >= L.dormantMinCalls && s.last > 0 && end - s.last > L.dormantSec) {
            snprintf(b, sizeof b, "%s active again after %.0f days", tg.c_str(), (end - s.last) / 86400.0);
            out.push_back(mk(end, "tg_awake", 0, tg, b));
        }
        // encryption changes
        if (s.calls > 0 && encrypted != s.enc) {
            if (s.run >= L.stableCalls) {
                snprintf(b, sizeof b, encrypted ? "%s went encrypted (it was clear for %d calls)" : "%s is clear again (it was encrypted for %d calls)", tg.c_str(), s.run);
                out.push_back(mk(end, encrypted ? "tg_encrypted" : "tg_clear", encrypted ? 1 : 0, tg, b));
            }
            s.enc = encrypted; s.run = 1;
        } else {
            s.enc = encrypted; s.run++;
        }
        // emergencies clustering
        if (emergency) {
            s.emerg.push_back(end);
            while (!s.emerg.empty() && end - s.emerg.front() > L.emergencyWindowSec) s.emerg.pop_front();
            if ((int)s.emerg.size() >= L.emergencyCalls && end - s.emergSaid > L.emergencyWindowSec) {
                s.emergSaid = end;
                snprintf(b, sizeof b, "%s: %d emergency calls in %.0f min", tg.c_str(), (int)s.emerg.size(), std::max(1.0, (end - s.emerg.front()) / 60.0));
                out.push_back(mk(end, "tg_emergency", 2, tg, b));
            }
        }
        s.calls++;
        s.last = end;
    }

private:
    struct St { int calls = 0, run = 0; bool enc = false; double last = 0, emergSaid = -1e18; std::deque<double> emerg; };
    static Insight mk(double t, const char* kind, int sev, const std::string& key, const std::string& text) {
        Insight i;
        i.time = t; i.area = "voice"; i.kind = kind; i.severity = sev; i.key = key; i.text = text;
        return i;
    }
    Limits L;
    std::map<std::string, St> st_;
};

}  // namespace dect2::insight
