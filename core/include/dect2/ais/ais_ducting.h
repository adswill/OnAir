// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS ducting detector (3.9, docs/SMART5-field-3.9.md item 4). VHF reaches a little beyond the radio horizon,
// d = 4.12 (sqrt(h_rx) + sqrt(h_ship)) km with heights in metres (standard atmosphere, k = 4/3). In a tropospheric duct (a temperature
// inversion over the sea) ships are heard two to ten times farther. This learns the station's normal range (the 90th percentile of the
// ship distances, one sample per vessel per 10 minutes, outside ducting) and starts a "ducting" event when minVessels different ships are
// heard beyond factor x that range within windowSec (or two beyond twice it); it ends after endSec without any. Base stations, aids to
// navigation, SAR aircraft (they fly high) and the long-range satellite channels are left out; positions over maxKm away are ignored
// (a wrong position). Uses only the receiver position the user set and the positions ships broadcast. Pure logic, no clock.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AisDuctEvent {
    bool start = true;                   // false: the event ended
    double t = 0;
    int vessels = 0;                     // beyond the threshold in the window
    double farthestKm = 0, thresholdKm = 0, normalKm = 0;
    double bearingDeg = 0;               // mean direction of the far vessels
    double farLat = 0, farLon = 0;
    std::string text;
};

class AisDucting {
public:
    double antennaM = 10;                // receiver antenna height above the sea
    double shipM = 25;                   // a typical ship's AIS antenna
    double factor = 1.5;                 // "beyond normal"
    int minVessels = 3;
    double windowSec = 900, endSec = 1800;
    double maxKm = 1500;
    int minSamples = 200;                // learned range used from this many samples

    void setHome(double lat, double lon) { home_ = true; hLat_ = lat; hLon_ = lon; }
    bool hasHome() const { return home_; }
    double horizonKm() const { return 4.12 * (std::sqrt(std::max(antennaM, 0.0)) + std::sqrt(std::max(shipM, 0.0))); }
    // the normal range: learned (recomputed every 50 new samples), but never under the radio horizon
    double normalKm() const {
        if ((int)samples_.size() < minSamples) return horizonKm();
        if (added_ != cachedAt_ && (cachedAt_ == 0 || added_ >= cachedAt_ + 50)) {
            std::vector<double> v(samples_.begin(), samples_.end());
            std::nth_element(v.begin(), v.begin() + (long)(v.size() * 9 / 10), v.end());
            learnedKm_ = v[v.size() * 9 / 10];
            cachedAt_ = added_;
        }
        return std::max(learnedKm_, horizonKm());
    }
    double thresholdKm() const { return factor * normalKm(); }
    bool active() const { return active_; }
    double activeSince() const { return since_; }
    double recordKm() const { return record_; }
    size_t learned() const { return samples_.size(); }

    static bool excluded(uint32_t mmsi, const std::string& cls, double freqHz) {
        if (cls == "base" || cls == "aid" || cls == "SAR") return true;
        if (mmsi / 1000000 == 111 || mmsi / 10000000 == 0 || mmsi / 10000000 == 99 || mmsi / 10000000 == 97) return true;   // SAR aircraft, coast stations, aids, SART / MOB / EPIRB
        if (std::fabs(freqHz - 156.775e6) < 5e3 || std::fabs(freqHz - 156.825e6) < 5e3) return true;   // channels 75 / 76: message 27 for satellites
        return false;
    }

    // a position report; an event start or end is appended to `out`
    void add(double t, uint32_t mmsi, double lat, double lon, const std::string& cls, double freqHz, std::vector<AisDuctEvent>& out) {
        if (!home_ || excluded(mmsi, cls, freqHz)) return;
        if (std::fabs(lat) > 90 || std::fabs(lon) > 180 || (lat == 0 && lon == 0)) return;
        const double km = dist(hLat_, hLon_, lat, lon);
        if (km > maxKm) return;
        const double thr = thresholdKm();
        if (!active_) {   // learning: one sample per vessel per 10 minutes, not while ducting
            auto& last = sampled_[mmsi];
            if (t - last >= 600) { last = t; samples_.push_back(km); added_++; if (samples_.size() > 5000) samples_.pop_front(); }
        }
        if (km > thr) {
            Far& f = far_[mmsi];
            f.t = t; f.km = km; f.lat = lat; f.lon = lon;
            if (km > record_) record_ = km;
        }
        evaluate(t, out);
    }
    // call now and then (once a minute) so an event can end without new reports
    void tick(double t, std::vector<AisDuctEvent>& out) { evaluate(t, out); }

    struct FarVessel { uint32_t mmsi; double km, lat, lon, t; };
    std::vector<FarVessel> farVessels(double now) const {
        std::vector<FarVessel> v;
        for (const auto& [m, f] : far_) if (now - f.t <= windowSec) v.push_back({m, f.km, f.lat, f.lon, f.t});
        std::sort(v.begin(), v.end(), [](const FarVessel& a, const FarVessel& b) { return a.km > b.km; });
        return v;
    }

private:
    struct Far { double t = 0, km = 0, lat = 0, lon = 0; };
    bool home_ = false; double hLat_ = 0, hLon_ = 0;
    std::deque<double> samples_;
    mutable double learnedKm_ = 0; mutable uint64_t cachedAt_ = 0; uint64_t added_ = 0;
    std::map<uint32_t, double> sampled_;
    std::map<uint32_t, Far> far_;
    bool active_ = false; double since_ = 0, lastFar_ = 0, record_ = 0;

    static double dist(double la1, double lo1, double la2, double lo2) {
        const double r = 3.14159265358979 / 180, dl = (la2 - la1) * r, dn = (lo2 - lo1) * r;
        const double a = std::sin(dl / 2) * std::sin(dl / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dn / 2) * std::sin(dn / 2);
        return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
    }
    static double bearing(double la1, double lo1, double la2, double lo2) {
        const double r = 3.14159265358979 / 180;
        const double y = std::sin((lo2 - lo1) * r) * std::cos(la2 * r), x = std::cos(la1 * r) * std::sin(la2 * r) - std::sin(la1 * r) * std::cos(la2 * r) * std::cos((lo2 - lo1) * r);
        double b = std::atan2(y, x) / r;
        return b < 0 ? b + 360 : b;
    }
    void evaluate(double t, std::vector<AisDuctEvent>& out) {
        for (auto it = far_.begin(); it != far_.end();) { if (t - it->second.t > windowSec) it = far_.erase(it); else ++it; }
        const double thr = thresholdKm(), nrm = normalKm();
        int n = 0, n2 = 0; double fk = 0, flat = 0, flon = 0, sx = 0, sy = 0;
        for (const auto& [m, f] : far_) {
            n++; if (f.km > 2 * nrm) n2++;
            if (f.km > fk) { fk = f.km; flat = f.lat; flon = f.lon; }
            const double b = bearing(hLat_, hLon_, f.lat, f.lon) * 3.14159265358979 / 180;
            sx += std::sin(b); sy += std::cos(b);
        }
        if (n) lastFar_ = t;
        static const char* const pts[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        if (!active_ && (n >= minVessels || n2 >= 2)) {
            active_ = true; since_ = t;
            AisDuctEvent e; e.start = true; e.t = t; e.vessels = n; e.farthestKm = fk; e.thresholdKm = thr; e.normalKm = nrm; e.farLat = flat; e.farLon = flon;
            double b = std::atan2(sx, sy) * 180 / 3.14159265358979; if (b < 0) b += 360;
            e.bearingDeg = b;
            char s[200];
            snprintf(s, sizeof s, "Tropospheric ducting: %d ships heard beyond %.0f km (normal %.0f km), farthest %.0f km, towards %s", n, thr, nrm, fk,
                     pts[(int)std::floor(std::fmod(b + 22.5, 360.0) / 45.0) & 7]);
            e.text = s;
            out.push_back(e);
        } else if (active_ && n == 0 && t - lastFar_ >= endSec - windowSec) {
            AisDuctEvent e; e.start = false; e.t = t; e.normalKm = nrm; e.thresholdKm = thr;
            char s[160];
            snprintf(s, sizeof s, "Ducting over after %.0f min: no ship beyond %.0f km for %.0f min", (t - since_) / 60, thr, endSec / 60);
            e.text = s;
            active_ = false;
            out.push_back(e);
        }
    }
};

} // namespace dect2
