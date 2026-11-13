// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B atmosphere sounding and altimetry cross-check (3.9, docs/SMART5-avia-3.9.md). Pure logic, no clock: every call gets the time.
//
// Sounding: every aircraft that reports or lets the tracker derive the wind (Comm-B 4,4 / 5,0 + 6,0) and the static air temperature adds one
// sample per 1000 ft layer per minute. From the last windowSec of samples: the vector-mean wind and the mean temperature per layer, the
// deviation from the ICAO standard atmosphere, the tropopause by the WMO lapse-rate rule (the lowest level where the lapse rate falls to
// 2 K/km or less and the mean lapse rate to every level within the next 2 km stays at or below 2 K/km), the freezing level and the jet core.
//
// Altimetry: the GNSS-minus-barometric height difference (TC 19) of one aircraft against the other aircraft in the same 4000 ft band at the
// same time. The difference depends on the day's temperature and pressure, but it is the same for every aircraft at a level, so an aircraft
// that differs from the others by more than limitFt, again and again, has an altitude source that disagrees with the fleet. When the whole
// band moves together within minutes it is the GNSS side (interference or spoofing), not one altimeter.
#pragma once
#include "adsb_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

// ICAO standard atmosphere temperature (C) at a pressure altitude (ft)
inline double isaTempC(double altFt) { const double m = altFt * 0.3048; return m < 11000 ? 15.0 - 0.0065 * m : -56.5; }

struct AtmosLayer {
    int baseFt = 0;              // the layer is baseFt .. baseFt + 1000
    int nWind = 0, nTemp = 0;
    double windFromDeg = 0, windKt = 0;   // vector mean, the direction it blows from
    double tempC = 0, isaDevC = 0;
};

struct AtmosProfile {
    std::vector<AtmosLayer> layers;        // with at least one sample, lowest first
    int aircraft = 0;                      // distinct aircraft behind it
    bool hasTropo = false; int tropoFt = 0; double tropoTempC = 0;
    bool hasFreezing = false; int freezingFt = 0;
    bool hasJet = false; int jetFt = 0; double jetKt = 0, jetFromDeg = 0;   // the strongest layer wind, 60 kt or more
};

class AdsbSounding {
public:
    double windowSec = 1800;               // samples older than this are dropped
    double maxRangeNm = 80;                // only aircraft this close (when the range is known)
    void add(const AdsbAircraft& a, double now) {
        if (!a.hasAlt || a.ground || a.altFt < 0 || a.altFt > 50000) return;
        if (a.hasRange && a.distNm > maxRangeNm) return;
        const bool w = a.hasWindRep || a.hasWindDer, t = a.hasSat;
        if (!w && !t) return;
        const int layer = a.altFt / 1000;
        const uint64_t key = ((uint64_t)a.icao << 8) | (uint64_t)layer;
        auto it = last_.find(key);
        if (it != last_.end() && now - it->second < 60) return;   // one sample per aircraft per layer per minute
        last_[key] = now;
        Sample s; s.t = now; s.icao = a.icao; s.altFt = a.altFt;
        if (w) {
            const double dir = a.hasWindRep ? a.windRepDir : a.windDerDir, kt = a.hasWindRep ? a.windRepKt : a.windDerKt;
            const double r = dir * M_PI / 180;   // wind FROM dir: the air moves towards dir + 180
            s.hasWind = true; s.u = -kt * std::sin(r); s.v = -kt * std::cos(r);
        }
        if (t) { s.hasTemp = true; s.tempC = a.satC; }
        samples_.push_back(s);
        prune(now);
    }
    void addSample(uint32_t icao, int altFt, bool hasWind, double fromDeg, double kt, bool hasTemp, double tempC, double now) {   // tests, feeds
        AdsbAircraft a; a.icao = icao; a.hasAlt = true; a.altFt = altFt;
        a.hasWindRep = hasWind; a.windRepDir = (float)fromDeg; a.windRepKt = (float)kt; a.hasSat = hasTemp; a.satC = (float)tempC;
        add(a, now);
    }
    void clear() { samples_.clear(); last_.clear(); }
    size_t samples() const { return samples_.size(); }

    AtmosProfile profile(double now) const {
        AtmosProfile p;
        std::map<int, Acc> acc; std::map<uint32_t, int> ac;
        for (const auto& s : samples_) {
            if (now - s.t > windowSec) continue;
            Acc& a = acc[s.altFt / 1000];
            if (s.hasWind) { a.u += s.u; a.v += s.v; a.nw++; }
            if (s.hasTemp) { a.t += s.tempC; a.nt++; }
            ac[s.icao]++;
        }
        p.aircraft = (int)ac.size();
        for (const auto& [k, a] : acc) {
            AtmosLayer L; L.baseFt = k * 1000; L.nWind = a.nw; L.nTemp = a.nt;
            if (a.nw) {
                const double u = a.u / a.nw, v = a.v / a.nw;
                L.windKt = std::hypot(u, v);
                double d = std::atan2(-u, -v) * 180 / M_PI; if (d < 0) d += 360; L.windFromDeg = d;
            }
            if (a.nt) { L.tempC = a.t / a.nt; L.isaDevC = L.tempC - isaTempC(L.baseFt + 500); }
            p.layers.push_back(L);
        }
        // temperature profile (layer mid-points with a temperature), km and C
        std::vector<std::pair<double, double>> tp;
        for (const auto& L : p.layers) if (L.nTemp) tp.push_back({(L.baseFt + 500) * 0.3048 / 1000, L.tempC});
        for (size_t i = 0; i + 1 < tp.size() && !p.hasFreezing; i++)
            if (tp[i].second >= 0 && tp[i + 1].second < 0) {
                const double f = tp[i].second / (tp[i].second - tp[i + 1].second);
                p.hasFreezing = true; p.freezingFt = (int)std::lround((tp[i].first + f * (tp[i + 1].first - tp[i].first)) * 1000 / 0.3048);
            }
        if (!p.hasFreezing && !tp.empty() && tp[0].second < 0 && tp[0].first < 1.5) { p.hasFreezing = true; p.freezingFt = 0; }   // below freezing from the lowest layer
        for (size_t i = 0; i + 1 < tp.size() && !p.hasTropo; i++) {
            if (tp[i].first < 5) continue;   // the WMO rule is meant above the boundary layer and the mid-troposphere inversions
            const double dz = tp[i + 1].first - tp[i].first; if (dz <= 0 || dz > 1.6) continue;
            const double g = -(tp[i + 1].second - tp[i].second) / dz;   // K/km, positive when it cools with height
            if (g > 2) continue;
            bool ok = true; int above = 0;
            for (size_t j = i + 1; j < tp.size() && tp[j].first - tp[i].first <= 2.0 + 1e-9; j++) {
                const double gj = -(tp[j].second - tp[i].second) / (tp[j].first - tp[i].first);
                if (gj > 2) { ok = false; break; }
                above++;
            }
            if (ok && above >= 1) { p.hasTropo = true; p.tropoFt = (int)std::lround(tp[i].first * 1000 / 0.3048); p.tropoTempC = tp[i].second; }
        }
        for (const auto& L : p.layers)
            if (L.nWind && L.windKt >= 60 && (!p.hasJet || L.windKt > p.jetKt)) { p.hasJet = true; p.jetFt = L.baseFt + 500; p.jetKt = L.windKt; p.jetFromDeg = L.windFromDeg; }
        return p;
    }

private:
    struct Sample { double t = 0; uint32_t icao = 0; int altFt = 0; bool hasWind = false, hasTemp = false; double u = 0, v = 0, tempC = 0; };
    struct Acc { double u = 0, v = 0, t = 0; int nw = 0, nt = 0; };
    std::deque<Sample> samples_;
    std::map<uint64_t, double> last_;
    void prune(double now) {
        while (!samples_.empty() && now - samples_.front().t > windowSec) samples_.pop_front();
        if (last_.size() > 20000) for (auto it = last_.begin(); it != last_.end();) it = now - it->second > windowSec ? last_.erase(it) : std::next(it);
    }
};

// ---- altimetry cross-check -----------------------------------------------------------------------------------------------------

struct AltFlag {
    uint32_t icao = 0;
    std::string callsign;
    int altFt = 0;
    double offsetFt = 0;          // its GNSS-baro difference minus the fleet median at that level
    int others = 0;               // aircraft in the comparison
    double t = 0;
    std::string text;
};
struct AltShift {                 // the whole band moved together: the GNSS heights, not one altimeter
    int bandFt = 0; double shiftFt = 0; int aircraft = 0; double t = 0; std::string text;
};

class AdsbAltCheck {
public:
    double limitFt = 250;         // disagreement that counts
    int needHits = 3;             // this many disagreeing samples, at least spanSec apart from first to last
    double spanSec = 30;
    double windowSec = 600;       // the fleet reference is built from this much history
    double shiftFt = 300;         // a band moving this far within shiftSec
    double shiftSec = 180;
    double cooldownSec = 1800;

    // call for every aircraft update; flags returned once per cooldown
    void add(const AdsbAircraft& a, double now, std::vector<AltFlag>* flags = nullptr, std::vector<AltShift>* shifts = nullptr) {
        if (!a.hasAlt || !a.hasGnssDiff || a.ground || a.altFt < 1000) return;
        auto la = lastAdd_.try_emplace(a.icao, -1e9).first;
        double& last = la->second;
        if (now - last < 2) return;   // at most one sample per aircraft every 2 s
        last = now;
        const int band = a.altFt / 4000;
        Obs o; o.t = now; o.icao = a.icao; o.diff = a.gnssDiffFt; o.altFt = a.altFt;
        auto& q = bands_[band];
        q.push_back(o);
        while (!q.empty() && now - q.front().t > windowSec) q.pop_front();
        // the fleet: the latest value of every other aircraft in the band within the last 120 s
        // (the difference grows with height when the air is warmer or colder than standard, so the fleet's own slope is fitted)
        std::map<uint32_t, std::pair<double, double>> latest;   // altitude, difference
        for (const auto& x : q) if (x.icao != a.icao && now - x.t <= 120) latest[x.icao] = {(double)x.altFt, x.diff};
        if ((int)latest.size() >= 3) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0; const double n = (double)latest.size();
            for (auto& [k, p] : latest) { sx += p.first; sy += p.second; sxx += p.first * p.first; sxy += p.first * p.second; }
            const double den = n * sxx - sx * sx;
            double slope = std::fabs(den) > 1e-6 && den / (n * n) > 250000 ? (n * sxy - sx * sy) / den : 0;   // a slope only when the heights spread over 500 ft or more
            slope = std::max(-0.08, std::min(0.08, slope));   // +-8 % of the height: far beyond any real temperature deviation
            const double icpt = (sy - slope * sx) / n;
            std::vector<double> res; for (auto& [k, p] : latest) res.push_back(p.second - (icpt + slope * p.first));
            std::vector<double> dev; const double rm = median(res); for (double d : res) dev.push_back(std::fabs(d - rm));
            const double mad = median(dev);
            const double off = a.gnssDiffFt - (icpt + slope * a.altFt) - rm;
            auto& h = hits_[a.icao];
            if (std::fabs(off) > std::max(limitFt, 4 * mad)) {
                if (h.n == 0 || now - h.last > 120) { h.n = 0; h.first = now; }
                h.n++; h.last = now;
                if (h.n >= needHits && now - h.first >= spanSec && now - h.flagged > cooldownSec) {
                    h.flagged = now;
                    if (flags) {
                        AltFlag f; f.icao = a.icao; f.callsign = a.callsign; f.altFt = a.altFt; f.offsetFt = off; f.others = (int)latest.size(); f.t = now;
                        char b[200]; snprintf(b, sizeof b, "altitude disagrees with %d aircraft at the same level by %+.0f ft (GNSS minus barometric)", f.others, off);
                        f.text = b; flags->push_back(f);
                    }
                }
            } else if (now - h.last > 120) h.n = 0;
        }
        // band shift: median of the last 60 s against the median of the samples shiftSec .. windowSec ago, from at least 3 aircraft each
        std::map<uint32_t, double> recent, older;
        for (const auto& x : q) { if (now - x.t <= 60) recent[x.icao] = x.diff; else if (now - x.t >= shiftSec) older[x.icao] = x.diff; }
        if (recent.size() >= 3 && older.size() >= 3) {
            std::vector<double> r, o2; for (auto& [k, d] : recent) r.push_back(d); for (auto& [k, d] : older) o2.push_back(d);
            const double s = median(r) - median(o2);
            double& lf = shiftFlagged_.try_emplace(band, -1e9).first->second;
            if (std::fabs(s) > shiftFt && now - lf > cooldownSec) {
                lf = now;
                if (shifts) {
                    AltShift sh; sh.bandFt = band * 4000; sh.shiftFt = s; sh.aircraft = (int)recent.size(); sh.t = now;
                    char b[200]; snprintf(b, sizeof b, "GNSS heights of %d aircraft at %d-%d ft moved %+.0f ft together: a GNSS problem, not an altimeter", sh.aircraft, sh.bandFt, sh.bandFt + 4000, s);
                    sh.text = b; shifts->push_back(sh);
                }
            }
        }
    }
    void clear() { bands_.clear(); hits_.clear(); lastAdd_.clear(); shiftFlagged_.clear(); }

private:
    struct Obs { double t = 0; uint32_t icao = 0; double diff = 0; int altFt = 0; };
    struct Hits { int n = 0; double first = 0, last = -1e9, flagged = -1e9; };
    std::map<int, std::deque<Obs>> bands_;
    std::map<uint32_t, Hits> hits_;
    std::map<uint32_t, double> lastAdd_;
    std::map<int, double> shiftFlagged_;
    static double median(std::vector<double> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end()); return v[v.size() / 2]; }
};

} // namespace dect2
