// HF utility insight (docs/SMART5-hfutil-3.9.md): overdue broadcasts.
// Learns, per station and frequency, the times of day (UTC) it is decoded (WEFAX charts, RTTY weather, SITOR, STANAG, ALE
// soundings, HF beacons, time signals). When a learned slot passes without a decode while the receiver was on that
// frequency for most of the slot, "DWD 10100.8 kHz overdue (usually at 10:30 UTC)".
#pragma once
#include "dect2/insight/insight.h"
#include <algorithm>
#include <bitset>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace dect2::insight {

class ScheduleWatch {
public:
    struct Limits {
        int tolMin = 6;              // a decode within this many minutes of a slot belongs to it
        int minDays = 3;             // a slot is learned after decodes on this many days ...
        double minRatio = 0.6;       // ... in at least this share of the days the receiver listened at that time
        double listenedShare = 0.8;  // the receiver must have been on the frequency for this share of the slot window
        int graceMin = 10;           // say "overdue" this long after the slot
        double freqTolHz = 1500;     // "on the frequency"
    };
    ScheduleWatch() {}
    explicit ScheduleWatch(Limits l) : L(l) {}

    struct Slot { int minute = 0; int hitDays = 0, listenedDays = 0; bool hitToday = false; bool saidToday = false; };

    // a decode of `station` on freqHz at t (Unix seconds)
    void decoded(double t, const std::string& station, double freqHz) {
        Key& k = key(station, freqHz);
        roll(k, t);
        const int m = minuteOf(t);
        k.listened.set((size_t)m);   // a decode means the receiver was there
        for (auto& s : k.slots) {
            if (circDist(s.minute, m) <= L.tolMin) {
                if (!s.hitToday) { s.hitToday = true; s.minute = (int)std::lround(s.minute * 0.8 + nearestTo(s.minute, m) * 0.2 + 1440) % 1440; }
                return;
            }
        }
        if (k.slots.size() >= 300) return;   // a mode decoded at random times: nothing to learn
        Slot s;
        s.minute = m; s.hitToday = true;
        k.slots.push_back(s);
    }

    // call about once a minute with the frequency the receiver is on (or several: a wide capture). Gives "overdue" findings.
    void tick(double now, const std::vector<double>& tunedHz, std::vector<Insight>& out) {
        const int m = minuteOf(now);
        for (auto& kv : keys_) {
            Key& k = kv.second;
            roll(k, now);
            bool on = false;
            for (double f : tunedHz) if (std::fabs(f - k.freqHz) <= L.freqTolHz) on = true;
            if (on) k.listened.set((size_t)m);
            for (auto& s : k.slots) {
                if (s.hitToday || s.saidToday || !learned(s)) continue;
                const int due = s.minute + L.tolMin + L.graceMin;
                if (due >= 1440 || m < due || m > due + 60) continue;   // slots near midnight: not judged (the day boundary)
                if (listenedShare(k, s.minute) < L.listenedShare) continue;
                s.saidToday = true;
                char b[200];
                snprintf(b, sizeof b, "%s %.1f kHz overdue (usually at %02d:%02d UTC, heard on %d of %d days)", k.station.c_str(), k.freqHz / 1e3,
                         s.minute / 60, s.minute % 60, s.hitDays, s.listenedDays);
                Insight i;
                i.time = now; i.area = "hfutil"; i.kind = "overdue"; i.severity = 0; i.key = kv.first; i.text = b;
                out.push_back(i);
            }
        }
    }

    // the learned slots of a station, minutes of the UTC day
    std::vector<int> schedule(const std::string& station, double freqHz) const {
        std::vector<int> v;
        auto it = keys_.find(name(station, freqHz));
        if (it == keys_.end()) return v;
        for (const auto& s : it->second.slots) if (learned(s)) v.push_back(s.minute);
        std::sort(v.begin(), v.end());
        return v;
    }

    struct Learned { std::string station; double freqHz = 0; std::vector<int> minutes; };
    // every station with a learned schedule
    std::vector<Learned> all() const {
        std::vector<Learned> v;
        for (const auto& kv : keys_) {
            Learned l;
            l.station = kv.second.station; l.freqHz = kv.second.freqHz;
            for (const auto& s : kv.second.slots) if (learned(s)) l.minutes.push_back(s.minute);
            std::sort(l.minutes.begin(), l.minutes.end());
            if (!l.minutes.empty()) v.push_back(l);
        }
        return v;
    }

private:
    struct Key {
        std::string station;
        double freqHz = 0;
        long long day = -1;
        std::bitset<1440> listened;
        std::vector<Slot> slots;
    };
    static std::string name(const std::string& st, double hz) { char b[32]; snprintf(b, sizeof b, "|%.1f", hz / 1e3); return st + b; }
    Key& key(const std::string& st, double hz) {
        // the same station within the frequency tolerance is one key
        for (auto& kv : keys_) if (kv.second.station == st && std::fabs(kv.second.freqHz - hz) <= L.freqTolHz) return kv.second;
        Key& k = keys_[name(st, hz)];
        k.station = st; k.freqHz = hz;
        return k;
    }
    static int minuteOf(double t) { return (int)std::floor(std::fmod(std::fmod(t, 86400.0) + 86400.0, 86400.0) / 60.0); }
    static int circDist(int a, int b) { const int d = std::abs(a - b) % 1440; return std::min(d, 1440 - d); }
    static int nearestTo(int ref, int m) { if (m - ref > 720) return m - 1440; if (ref - m > 720) return m + 1440; return m; }
    bool learned(const Slot& s) const { return s.hitDays >= L.minDays && s.hitDays >= L.minRatio * s.listenedDays; }
    double listenedShare(const Key& k, int minute) const {
        int n = 0, on = 0;
        for (int d = -L.tolMin; d <= L.tolMin; d++) { n++; on += k.listened.test((size_t)((minute + d + 1440) % 1440)); }
        return (double)on / n;
    }
    // a new UTC day: count yesterday's slots
    void roll(Key& k, double t) {
        const long long day = (long long)std::floor(t / 86400.0);
        if (k.day == day) return;
        if (k.day >= 0) {
            for (auto& s : k.slots) {
                if (s.hitToday) { s.hitDays++; s.listenedDays++; }
                else if (listenedShare(k, s.minute) >= L.listenedShare) s.listenedDays++;
                s.hitToday = s.saidToday = false;
            }
            // slots heard once and never again after a week of listening are noise
            k.slots.erase(std::remove_if(k.slots.begin(), k.slots.end(), [&](const Slot& s) { return s.listenedDays >= 7 && s.hitDays * 4 < s.listenedDays; }), k.slots.end());
        }
        k.day = day;
        k.listened.reset();
    }

    Limits L;
    std::map<std::string, Key> keys_;
};

}  // namespace dect2::insight
