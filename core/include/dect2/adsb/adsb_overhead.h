// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B overhead log (planefence): every aircraft that came within a radius of the receiver, one row per pass with the closest moment (time,
// distance, altitude, callsign, type, registration), a "now overhead" list, a CSV per day. docs/ADSB-OVERHEAD-3.9.md.
#pragma once
#include "dect2/adsb/adsb_tel.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AdsbOverheadPass {
    uint32_t icao = 0;
    std::string callsign, type, registration;
    int64_t firstUtc = 0, closestUtc = 0, lastUtc = 0;   // inside the radius
    double closestKm = 1e9;
    bool hasAlt = false; int altFt = 0;                  // at the closest moment
    bool ground = false;
};

// What the log needs of the registration database for a row (empty when unknown)
struct AdsbOverheadInfo { std::string type, registration; };

class AdsbOverheadLog {
public:
    void setRadiusKm(double km) { radiusKm_ = km > 0.1 ? km : 0.1; }
    double radiusKm() const { return radiusKm_; }
    void setMaxAltFt(int ft) { maxAltFt_ = ft; }   // <= 0: any altitude
    // one update with the aircraft as the receiver has them now. lookup gives type and registration. Returns the passes that ended in this update
    // (left the radius, or not heard inside it for endSec).
    template <class Lookup> std::vector<AdsbOverheadPass> update(const std::vector<AdsbAircraft>& aircraft, int64_t nowUtc, Lookup lookup);
    std::vector<AdsbOverheadPass> update(const std::vector<AdsbAircraft>& aircraft, int64_t nowUtc);
    // passes going on now (inside the radius), closest first
    std::vector<AdsbOverheadPass> now() const;
    // the finished passes kept (newest last); add() puts one back (from the day's file at start)
    const std::vector<AdsbOverheadPass>& done() const { return done_; }
    void add(const AdsbOverheadPass& p);
    void clear() { open_.clear(); done_.clear(); }
    static constexpr int kEndSec = 120;
    static constexpr size_t kKeep = 5000;
private:
    struct Open { AdsbOverheadPass p; int64_t lastIn = 0; };
    void note(const AdsbAircraft& ac, double km, int64_t now, const AdsbOverheadInfo& info);
    std::vector<AdsbOverheadPass> closeOld(const std::vector<AdsbAircraft>& aircraft, int64_t now);
    double radiusKm_ = 3.0;
    int maxAltFt_ = 0;
    std::map<uint32_t, Open> open_;
    std::vector<AdsbOverheadPass> done_;
};

template <class Lookup> std::vector<AdsbOverheadPass> AdsbOverheadLog::update(const std::vector<AdsbAircraft>& aircraft, int64_t nowUtc, Lookup lookup) {
    for (const AdsbAircraft& ac : aircraft) {
        if (!ac.hasRange || ac.ageSec > 30) continue;
        const double km = ac.distNm * 1.852;
        if (km > radiusKm_) continue;
        if (maxAltFt_ > 0 && ac.hasAlt && !ac.ground && ac.altFt > maxAltFt_) continue;
        AdsbOverheadInfo info;
        if (!open_.count(ac.icao) || open_[ac.icao].p.type.empty()) info = lookup(ac);
        note(ac, km, nowUtc, info);
    }
    return closeOld(aircraft, nowUtc);
}

// CSV of passes: utc_closest (ISO), icao, callsign, registration, type, closest_km, alt_ft, first, last, ground
std::string adsbOverheadCsv(const std::vector<AdsbOverheadPass>& v);
// reads what adsbOverheadCsv wrote (bad lines skipped)
std::vector<AdsbOverheadPass> adsbOverheadFromCsv(const std::string& text);
// "2026-11-10" of a UTC time in the given offset from UTC (seconds): the day a pass belongs to
std::string adsbOverheadDay(int64_t utc, int tzOffsetSec);

}  // namespace dect2
