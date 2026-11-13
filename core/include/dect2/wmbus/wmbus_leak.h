// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Leak check for the user's own water meter (3.9, docs/SMART5-field-3.9.md item 8), from the volume register a wireless M-Bus meter
// sends (only meters the user marks as theirs; an encrypted meter needs the user's own key in the wM-Bus mode, nothing is cracked).
//  - night flow: the use between nightStartH and nightEndH local time (when a household uses no water), in litres per hour, from the
//    first and last reading of that window; the baseline is the median of the previous nights. "leak_night" when a night's flow is at
//    least minLeakLph and twice the baseline plus 0.5 L/h; severity 1 from the second such night in a row.
//  - continuous flow: a register that never stands still. Two readings at least quietSec apart with the same value prove a quiet spell;
//    "leak_continuous" when there has been none for continuousH hours although readings came at least every maxGapSec (severity 2 from
//    alarmH hours). A meter whose register steps in large units (10 L) hides a small drip from this test; the night flow still sees it
//    over several nights.
// A register that steps back starts the readings again (meter replaced or reset). Pure logic, no clock: Unix seconds (UTC) and the
// local time-zone offset for the night window. Kept between runs (save()/load()).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace dect2 {

class WaterLeakCheck {
public:
    int tzOffsetSec = 0;
    int nightStartH = 1, nightEndH = 5;
    double minLeakLph = 1.0;
    double quietSec = 1800;
    double continuousH = 24, alarmH = 72;
    double maxGapSec = 7200;

    struct Finding { std::string kind, text; int severity = 0; double value = 0; };

    // the register converted to litres ("1234.567 m3" -> 1234567); false when the text has no volume unit
    static bool litresFrom(double number, const std::string& valueText, double& litres) {
        const size_t sp = valueText.find_last_of(' ');
        const std::string unit = sp == std::string::npos ? std::string() : valueText.substr(sp + 1);
        if (unit == "m3" || unit == "m^3" || unit == "m\xC2\xB3") { litres = number * 1000; return true; }
        if (unit == "l" || unit == "L") { litres = number; return true; }
        return false;
    }

    // a reading of the cumulative register in litres; findings appended to `out`
    void add(int64_t utc, double litres, std::vector<Finding>& out) {
        if (!std::isfinite(litres) || litres < 0) return;
        if (!r_.empty() && utc <= r_.back().t) return;
        if (!r_.empty() && litres < r_.back().v - 1e-6) { r_.clear(); resets_++; }
        // keep a reading when it changed or 5 minutes passed; the last of a run of equal values is kept up to date (it ends a quiet spell)
        if (r_.size() >= 2 && r_.back().v == litres && r_[r_.size() - 2].v == litres && utc - r_[r_.size() - 2].t < 300) r_.back().t = utc;
        else r_.push_back({utc, litres});
        while (!r_.empty() && utc - r_.front().t > 9 * 86400) r_.pop_front();
        // a quiet spell ending now
        for (auto it = r_.rbegin(); it != r_.rend(); ++it) {
            if (it->v != litres) break;
            if (utc - it->t >= quietSec) { lastQuiet_ = utc; break; }
        }
        if (firstUtc_ == 0) firstUtc_ = utc;
        check(utc, out);
    }
    // once a minute or so: the night that just ended and the continuous-flow test
    void tick(int64_t now, std::vector<Finding>& out) { check(now, out); }

    double hoursSinceQuiet(int64_t now) const { const int64_t from = lastQuiet_ ? lastQuiet_ : firstUtc_; return from ? (now - from) / 3600.0 : 0; }
    bool coverageOk(int64_t from, int64_t to) const {   // readings at least every maxGapSec over [from, to]
        int64_t prev = from; bool any = false;
        for (const auto& x : r_) { if (x.t < from) continue; if (x.t > to) break; if (x.t - prev > maxGapSec) return false; prev = x.t; any = true; }
        return any && to - prev <= maxGapSec;
    }
    // night flows: local day number of the morning -> L/h
    const std::map<int64_t, double>& nights() const { return nights_; }
    double baselineLph(int64_t beforeDay) const {
        std::vector<double> v;
        for (const auto& [d, f] : nights_) if (d < beforeDay && d >= beforeDay - 14) v.push_back(f);
        if (v.size() < 3) return -1;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
    int resets() const { return resets_; }
    size_t readings() const { return r_.size(); }
    double lastLitres() const { return r_.empty() ? 0 : r_.back().v; }

    std::string save() const {
        std::ostringstream o; o.precision(12);
        o << "waterleak 1\n";
        for (const auto& x : r_) o << "R " << x.t << " " << x.v << "\n";
        for (const auto& [d, f] : nights_) o << "N " << d << " " << f << "\n";
        o << "Q " << lastQuiet_ << " " << firstUtc_ << " " << streak_ << " " << lastNightDone_ << "\n";
        return o.str();
    }
    bool load(const std::string& text) {
        std::istringstream in(text); std::string l;
        if (!std::getline(in, l) || l.rfind("waterleak 1", 0) != 0) return false;
        while (std::getline(in, l)) {
            std::istringstream ls(l); char k = 0; ls >> k;
            if (k == 'R') { Reading x; if (ls >> x.t >> x.v) r_.push_back(x); }
            else if (k == 'N') { int64_t d; double f; if (ls >> d >> f) nights_[d] = f; }
            else if (k == 'Q') ls >> lastQuiet_ >> firstUtc_ >> streak_ >> lastNightDone_;
        }
        return true;
    }

private:
    struct Reading { int64_t t = 0; double v = 0; };
    std::deque<Reading> r_;
    std::map<int64_t, double> nights_;
    int64_t lastQuiet_ = 0, firstUtc_ = 0, lastNightDone_ = 0, lastContAlert_ = 0;
    int streak_ = 0, resets_ = 0, lastContSev_ = 0;

    int64_t dayOf(int64_t utc) const { return (int64_t)std::floor((double)(utc + tzOffsetSec) / 86400.0); }

    void check(int64_t now, std::vector<Finding>& out) {
        // the night of today (local), once it is over
        const int64_t d = dayOf(now);
        const int64_t ns = d * 86400 + nightStartH * 3600 - tzOffsetSec, ne = d * 86400 + nightEndH * 3600 - tzOffsetSec;
        if (now >= ne && lastNightDone_ < d) {
            lastNightDone_ = d;
            const Reading* a = nullptr; const Reading* b = nullptr;
            for (const auto& x : r_) {
                if (!a && x.t >= ns - 1800 && x.t <= ns + 1800) a = &x;
                if (x.t >= ne - 1800 && x.t <= ne + 1800 && x.t <= now) b = &x;
            }
            if (a && b && b->t - a->t >= 2 * 3600 && coverageOk(a->t, b->t)) {
                const double lph = (b->v - a->v) / ((b->t - a->t) / 3600.0);
                const double base = baselineLph(d);
                nights_[d] = lph;
                while (nights_.size() > 60) nights_.erase(nights_.begin());
                const double limit = std::max(minLeakLph, base >= 0 ? 2 * base + 0.5 : minLeakLph);
                if (lph >= limit) {
                    streak_++;
                    char t[200];
                    if (base >= 0) snprintf(t, sizeof t, "Water at night: %.1f L/h between %02d:00 and %02d:00 (usual %.1f L/h)%s", lph, nightStartH, nightEndH, base,
                                             streak_ >= 2 ? ", second night in a row: possible leak" : "");
                    else snprintf(t, sizeof t, "Water at night: %.1f L/h between %02d:00 and %02d:00%s", lph, nightStartH, nightEndH, streak_ >= 2 ? ", second night in a row: possible leak" : "");
                    out.push_back({"leak_night", t, streak_ >= 2 ? 1 : 0, lph});
                } else streak_ = 0;
            }
        }
        // continuous flow
        const double h = hoursSinceQuiet(now);
        const int64_t from = lastQuiet_ ? lastQuiet_ : firstUtc_;
        if (h >= continuousH && from && coverageOk(from, now)) {
            const int sev = h >= alarmH ? 2 : 1;
            if (sev > lastContSev_ || now - lastContAlert_ >= 86400) {
                lastContSev_ = sev; lastContAlert_ = now;
                char t[200]; snprintf(t, sizeof t, "Water flowing without a pause for %.0f h: a running toilet, a dripping tap or a leak?", h);
                out.push_back({"leak_continuous", t, sev, h});
            }
        } else if (h < continuousH) lastContSev_ = 0;
    }
};

} // namespace dect2
