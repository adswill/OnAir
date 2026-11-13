// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B manoeuvre alerts (3.9, docs/SMART5-field-3.9.md item 1), from what each aircraft broadcasts (the "adsb.aircraft" updates, about
// one every 2 s per aircraft):
//  - "emergency":  squawk 7500 (unlawful interference), 7600 (radio failure), 7700 (general emergency), or an emergency state in the
//                  aircraft status message (TC 28); once per aircraft and code;
//  - "descent":    descending faster than descentFpm for sustainSec (the barometric or GNSS vertical rate the aircraft sends, or the rate
//                  from its altitude when it sends none); an alarm from alarmDescentFpm or with an emergency squawk;
//  - "climb":      climbing faster than climbFpm for sustainSec (military jets do this; airliners rarely exceed 4000 fpm);
//  - "turn":       turning faster than turnDegPerSec (a rate-one turn is 3 deg/s) above turnMinKt for sustainSec;
//  - "go_around":  an approach (descending below goAroundMaxFt) that turns into a climb of goAroundClimbFt or more within 3 minutes of the
//                  lowest point without the aircraft reporting itself on the ground; "touch_and_go" when it did report ground in between.
// Each alert is repeated for the same aircraft and kind only after cooldownSec. Pure logic, no clock: times are the caller's seconds.
#pragma once
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AdsbManState {
    std::string hex, callsign;
    double t = 0;                       // seconds
    bool hasAlt = false; double altFt = 0;
    bool hasVrate = false; double vrateFpm = 0;
    bool hasHeading = false; double headingDeg = 0;
    bool hasSpeed = false; double speedKt = 0;
    bool hasPos = false; double lat = 0, lon = 0;
    bool ground = false;
    int squawk = -1;                    // four octal digits written as decimal (7700), -1 none
    int emergency = 0;                  // TC 28 emergency state 0 - 7
};

struct AdsbManAlert {
    std::string kind, hex, callsign, text;
    double t = 0;
    int severity = 0;                   // 0 information, 1 warning, 2 alarm
    double value = 0;                   // fpm, deg/s, ft climbed ...
    bool hasPos = false; double lat = 0, lon = 0;
};

class AdsbManoeuvreWatch {
public:
    double descentFpm = 5000, alarmDescentFpm = 8000;
    double climbFpm = 6000;
    double turnDegPerSec = 5, turnMinKt = 140;
    double sustainSec = 8;
    double minAltFt = 1000;             // descent, climb and turn alerts above this
    double goAroundMaxFt = 3000, goAroundClimbFt = 400;
    double cooldownSec = 600;
    // optional: km to the nearest airfield; a go-around is then only reported within 25 km of one
    std::function<double(double lat, double lon)> nearestAirfieldKm;

    static const char* squawkMeaning(int sq) {
        return sq == 7500 ? "unlawful interference" : sq == 7600 ? "radio failure" : sq == 7700 ? "general emergency" : "";
    }
    static const char* emergencyName(int e) {
        static const char* const n[8] = {"", "general emergency", "medical emergency", "minimum fuel", "no communications", "unlawful interference", "downed aircraft", "reserved"};
        return e >= 0 && e < 8 ? n[e] : "";
    }

    // one update; alerts are appended to `out`; returns how many
    int add(const AdsbManState& s, std::vector<AdsbManAlert>& out) {
        if (s.hex.empty()) return 0;
        Track& k = tr_[s.hex];
        if (!k.samples.empty() && s.t < k.samples.back().t) return 0;   // out of order
        const size_t before = out.size();
        if (!s.callsign.empty()) k.callsign = s.callsign;
        // emergencies
        const bool sqEm = s.squawk == 7500 || s.squawk == 7600 || s.squawk == 7700;
        if (sqEm && k.lastEmSquawk != s.squawk) {
            k.lastEmSquawk = s.squawk;
            char b[160]; snprintf(b, sizeof b, "%s squawks %d (%s)%s", name(s.hex, k).c_str(), s.squawk, squawkMeaning(s.squawk), altText(s).c_str());
            push(out, s, k, "emergency", 2, s.squawk, b, true);
        } else if (!sqEm) k.lastEmSquawk = -1;
        if (s.emergency > 0 && s.emergency != k.lastEmState && !sqEm) {
            k.lastEmState = s.emergency;
            char b[160]; snprintf(b, sizeof b, "%s reports an emergency: %s%s", name(s.hex, k).c_str(), emergencyName(s.emergency), altText(s).c_str());
            push(out, s, k, "emergency", 2, s.emergency, b, true);
        } else if (s.emergency == 0) k.lastEmState = 0;

        Sample x; x.t = s.t; x.hasAlt = s.hasAlt; x.alt = s.altFt; x.ground = s.ground; x.hasHdg = s.hasHeading; x.hdg = s.headingDeg;
        x.hasSpd = s.hasSpeed; x.spd = s.speedKt;
        x.hasVr = s.hasVrate; x.vr = s.vrateFpm;
        if (!x.hasVr && x.hasAlt) {   // the rate from the altitude: the oldest altitude at least 8 s back
            for (auto it = k.samples.rbegin(); it != k.samples.rend(); ++it)
                if (it->hasAlt && s.t - it->t >= 8) { x.hasVr = true; x.vr = (x.alt - it->alt) / (s.t - it->t) * 60.0; x.derived = true; break; }
        }
        k.samples.push_back(x);
        while (!k.samples.empty() && s.t - k.samples.front().t > 300) k.samples.pop_front();

        const bool airborne = !s.ground && s.hasAlt && s.altFt >= minAltFt;
        if (airborne) {
            double worst = 0;
            if (sustained(k, s.t, [&](const Sample& a) { return a.hasVr && a.vr <= -descentFpm; }, &worst, true)) {
                const bool alarm = worst <= -alarmDescentFpm || k.lastEmSquawk == 7700 || s.emergency > 0;
                char b[160]; snprintf(b, sizeof b, "%s descending at %.0f ft/min%s", name(s.hex, k).c_str(), -worst, altText(s).c_str());
                push(out, s, k, "descent", alarm ? 2 : 1, worst, b);
            }
            if (sustained(k, s.t, [&](const Sample& a) { return a.hasVr && a.vr >= climbFpm; }, &worst, false)) {
                char b[160]; snprintf(b, sizeof b, "%s climbing at %.0f ft/min%s", name(s.hex, k).c_str(), worst, altText(s).c_str());
                push(out, s, k, "climb", 0, worst, b);
            }
            double rate = 0;
            if (s.hasSpeed && s.speedKt >= turnMinKt && turnRate(k, s.t, rate) && std::fabs(rate) >= turnDegPerSec) {
                char b[160]; snprintf(b, sizeof b, "%s turning %s at %.1f deg/s at %.0f kt%s", name(s.hex, k).c_str(), rate > 0 ? "right" : "left", std::fabs(rate), s.speedKt, altText(s).c_str());
                push(out, s, k, "turn", 0, std::fabs(rate), b);
            }
        }
        goAround(s, k, out);
        return (int)(out.size() - before);
    }

    // forget aircraft not heard for maxAgeSec
    void prune(double now, double maxAgeSec = 900) {
        for (auto it = tr_.begin(); it != tr_.end();) {
            if (it->second.samples.empty() || now - it->second.samples.back().t > maxAgeSec) it = tr_.erase(it); else ++it;
        }
    }
    size_t tracked() const { return tr_.size(); }

private:
    struct Sample { double t = 0; bool hasAlt = false; double alt = 0; bool ground = false; bool hasHdg = false; double hdg = 0; bool hasSpd = false; double spd = 0;
                    bool hasVr = false; double vr = 0; bool derived = false; };
    struct Track {
        std::deque<Sample> samples;
        std::string callsign;
        std::map<std::string, double> lastAlert;   // kind -> time
        int lastEmSquawk = -1, lastEmState = 0;
        // go-around state
        bool approach = false; double descentSince = -1, minAlt = 1e9, minT = 0, groundFirst = 0; bool groundSeen = false;
    };
    std::map<std::string, Track> tr_;

    static std::string name(const std::string& hex, const Track& k) { return k.callsign.empty() ? hex : k.callsign + " (" + hex + ")"; }
    static std::string altText(const AdsbManState& s) {
        if (!s.hasAlt) return "";
        char b[32]; snprintf(b, sizeof b, ", %.0f ft", s.altFt); return b;
    }
    void push(std::vector<AdsbManAlert>& out, const AdsbManState& s, Track& k, const char* kind, int sev, double v, const char* text, bool noCooldown = false) {
        std::string key = kind;
        if (noCooldown) key += std::to_string((int)v);
        auto it = k.lastAlert.find(key);
        if (it != k.lastAlert.end() && s.t - it->second < cooldownSec) return;
        k.lastAlert[key] = s.t;
        AdsbManAlert a; a.kind = kind; a.hex = s.hex; a.callsign = k.callsign; a.t = s.t; a.severity = sev; a.value = v; a.text = text;
        a.hasPos = s.hasPos; a.lat = s.lat; a.lon = s.lon;
        out.push_back(a);
    }
    // every sample of the last sustainSec meets the test (at least two samples spanning sustainSec); `extreme` gets the smallest (min) or largest value
    template <class F> bool sustained(const Track& k, double now, F test, double* extreme, bool min) {
        int n = 0; double first = now; double ex = min ? 1e18 : -1e18;
        for (auto it = k.samples.rbegin(); it != k.samples.rend(); ++it) {
            if (!it->hasVr) continue;
            if (!test(*it)) return false;
            n++; first = it->t;
            ex = min ? std::min(ex, it->vr) : std::max(ex, it->vr);
            if (now - first >= sustainSec) break;
        }
        if (n < 2 || now - first < sustainSec) return false;
        *extreme = ex;
        return true;
    }
    // the turn rate over the last sustainSec, deg/s (+ right); false without enough headings or when the turn changes direction
    bool turnRate(const Track& k, double now, double& rate) const {
        const Sample* last = nullptr; double sum = 0, first = now; int n = 0, sign = 0;
        for (auto it = k.samples.rbegin(); it != k.samples.rend(); ++it) {
            if (!it->hasHdg) continue;
            if (last) {
                double d = last->hdg - it->hdg;
                while (d > 180) d -= 360;
                while (d < -180) d += 360;
                const int sg = d > 0.5 ? 1 : d < -0.5 ? -1 : 0;
                if (sg == 0 || (sign != 0 && sg != sign)) return false;
                sign = sg; sum += d;
            }
            last = &*it; first = it->t; n++;
            if (now - first >= sustainSec) break;
        }
        if (n < 3 || now - first < sustainSec) return false;
        rate = sum / (now - first);
        return true;
    }
    void goAround(const AdsbManState& s, Track& k, std::vector<AdsbManAlert>& out) {
        auto reset = [&] { k.approach = false; k.descentSince = -1; k.minAlt = 1e9; k.groundSeen = false; };
        if (s.ground) {   // on the runway: a touch-and-go if it lifts off again within a minute, else a full-stop landing
            if (!k.approach) return;
            if (!k.groundSeen) k.groundFirst = s.t;
            k.groundSeen = true; k.minT = s.t;
            if (s.hasAlt) k.minAlt = std::min(k.minAlt, s.altFt);
            if (s.t - k.groundFirst > 60) reset();
            return;
        }
        if (!s.hasAlt) return;
        const Sample& x = k.samples.back();
        if (x.hasVr && x.vr <= -300 && s.altFt < goAroundMaxFt + 2000) {
            if (k.descentSince < 0) k.descentSince = s.t;
            if (s.altFt < goAroundMaxFt && s.t - k.descentSince >= 30) k.approach = true;
        }
        if (k.approach && s.altFt < k.minAlt) { k.minAlt = s.altFt; k.minT = s.t; }
        if (k.approach && s.t - k.minT <= 180 && s.altFt >= k.minAlt + goAroundClimbFt && x.hasVr && x.vr >= 500) {
            bool nearField = true;
            if (nearestAirfieldKm && s.hasPos) { const double km = nearestAirfieldKm(s.lat, s.lon); nearField = km >= 0 && km <= 25; }
            if (nearField) {
                const bool tg = k.groundSeen;
                char b[200];
                if (tg) snprintf(b, sizeof b, "%s touch-and-go: climbing again from the ground, now %.0f ft", name(s.hex, k).c_str(), s.altFt);
                else snprintf(b, sizeof b, "%s go-around: approach down to %.0f ft, now climbing through %.0f ft", name(s.hex, k).c_str(), k.minAlt, s.altFt);
                push(out, s, k, tg ? "touch_and_go" : "go_around", tg ? 0 : 1, s.altFt - k.minAlt, b);
            }
            reset();
        }
        if (k.approach && s.t - k.minT > 180 && s.altFt > k.minAlt + goAroundClimbFt) reset();
        if (!(x.hasVr && x.vr <= -300) && !k.approach) k.descentSince = -1;
    }
};

} // namespace dect2
