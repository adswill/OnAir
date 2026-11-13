// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/limits.h"
#include "dect2/uat/uat_track.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

void UatTracker::reset() {
    ac_.clear(); fisb_.clear(); log_.clear(); radar_.clear();
    good_ = bad_ = 0; goodWindow_ = 0; windowT0_ = 0;
}

void UatTracker::setReference(double lat, double lon) { refValid_ = true; refLat_ = lat; refLon_ = lon; }
void UatTracker::clearReference() { refValid_ = false; }

void UatTracker::apply(const UatDecoded& d, float levelDbfs, double timeSec) {
    good_++;
    if (windowT0_ <= 0) windowT0_ = timeSec;
    if (timeSec - windowT0_ <= 1.0) goodWindow_++;

    UatFrameInfo fi;
    fi.hex = d.hex;
    fi.timeSec = timeSec;
    fi.levelDbfs = levelDbfs;
    fi.summary = d.summary;
    switch (d.kind) {
    case UatDecoded::Kind::Adsb: fi.kind = "ADS-B"; break;
    case UatDecoded::Kind::Tisb: fi.kind = "TIS-B"; break;
    case UatDecoded::Kind::Fisb: fi.kind = "FIS-B"; break;
    default: fi.kind = "UAT"; break;
    }

    if (d.kind == UatDecoded::Kind::Fisb) {
        UatFisbProduct p = d.fisb;
        p.timeSec = timeSec;
        fisb_.push_back(p);
        for (const auto& b : d.nexrad) { auto& r = radar_[((uint64_t)b.productId << 32) | b.blockNum]; r.b = b; r.t = timeSec; }
        while (fisb_.size() > 64) fisb_.pop_front();
        log_.push_back(fi);
        while (log_.size() > 64) log_.pop_front();
        return;
    }

    if (d.kind == UatDecoded::Kind::Adsb || d.kind == UatDecoded::Kind::Tisb) {
        fi.address = d.ac.icao;
        Rec& r = ac_[d.ac.icao];
        UatAircraft& ac = r.ac;
        if (!ac.icao) ac.icao = d.ac.icao;
        if (!d.ac.callsign.empty()) ac.callsign = d.ac.callsign;
        if (d.ac.hasSquawk) { ac.hasSquawk = true; ac.squawk = d.ac.squawk; }
        if (d.ac.hasAlt) { ac.hasAlt = true; ac.altFt = d.ac.altFt; }
        if (d.ac.hasGeoAlt) { ac.hasGeoAlt = true; ac.geoAltFt = d.ac.geoAltFt; }
        if (d.ac.hasPos) {
            ac.hasPos = true; ac.lat = d.ac.lat; ac.lon = d.ac.lon;
            ac.track.push_back({(float)d.ac.lat, (float)d.ac.lon});
            while (ac.track.size() > 64) ac.track.erase(ac.track.begin());
        }
        if (d.ac.hasSpeed) { ac.hasSpeed = true; ac.speedKt = d.ac.speedKt; }
        if (d.ac.hasHeading) { ac.hasHeading = true; ac.headingDeg = d.ac.headingDeg; }
        if (d.ac.hasVrate) { ac.hasVrate = true; ac.vrateFpm = d.ac.vrateFpm; }
        ac.ground = d.ac.ground;
        ac.tisb = d.ac.tisb;
        if (d.ac.nic >= 0) ac.nic = d.ac.nic;
        ac.messages++;
        ac.levelDbfs = levelDbfs;
        ac.ageSec = 0;
        r.last = timeSec;
        if (refValid_ && ac.hasPos) {
            ac.hasRange = true;
            ac.distNm = (float)adsb::distanceNm(refLat_, refLon_, ac.lat, ac.lon);
            ac.bearingDeg = (float)adsb::bearingDeg(refLat_, refLon_, ac.lat, ac.lon);
        }
    }

    log_.push_back(fi);
    while (log_.size() > 64) log_.pop_front();
}

void UatTracker::tick(double now) {
    for (auto it = ac_.begin(); it != ac_.end();) {
        it->second.ac.ageSec = (float)(now - it->second.last);
        if (now - it->second.last > limits::staleSec(limits::Family::Uat, expirySec_)) it = ac_.erase(it);
        else ++it;
    }
    if (now - windowT0_ > 1.0) { goodWindow_ = 0; windowT0_ = now; }
}

void UatTracker::snapshot(UatTelemetry& out, double now) {
    tick(now);
    out.aircraft.clear();
    out.fisb.assign(fisb_.begin(), fisb_.end());
    out.nexrad.clear();
    for (auto it = radar_.begin(); it != radar_.end();) {
        if (now - it->second.t > 900.0) { it = radar_.erase(it); continue; }   // CONUS products are sent every 15 minutes
        const UatNexradBlock& b = it->second.b;
        for (int i = 0; i < 128 && out.nexrad.size() < kUatNexradMax; i++) {
            if (b.bins[(size_t)i] < 2) continue;
            const float dLon = (float)b.lonSize / 32, dLat = (float)b.latSize / 4;
            UatNexradCell c; c.lat = (float)b.latNorth - dLat * (float)(i / 32); c.lon = (float)b.lonWest + dLon * (float)(i % 32); c.dLat = dLat; c.dLon = dLon; c.value = b.bins[(size_t)i];
            out.nexrad.push_back(c);
        }
        ++it;
    }
    out.framesLog.assign(log_.begin(), log_.end());
    out.aircraftCount = (uint32_t)ac_.size();
    out.withPosition = 0;
    out.fisbCount = (uint32_t)fisb_.size();
    out.tisbCount = 0;
    out.otherCount = 0;
    for (const auto& l : log_) {
        if (l.kind == "TIS-B") out.tisbCount++;
        else if (l.kind == "UAT") out.otherCount++;
    }
    std::vector<std::pair<double, UatAircraft>> rows;
    for (auto& kv : ac_) rows.push_back({kv.second.last, kv.second.ac});
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& r : rows) {
        if (r.second.hasPos) out.withPosition++;
        out.aircraft.push_back(r.second);
        if (out.aircraft.size() >= limits::kAircraft) break;
    }
    out.refValid = refValid_;
    out.refLat = refLat_;
    out.refLon = refLon_;
    out.msgsPerSec = (float)goodWindow_;
    out.frameCount = good_;
}

std::string UatTracker::saveResults() const {
    std::vector<UatAircraft> v;
    for (auto& kv : ac_) v.push_back(kv.second.ac);
    return results::pack(0.0, v);
}
bool UatTracker::loadResults(const std::string& blob, double now) {
    double t0 = 0;
    std::vector<UatAircraft> v;
    if (!results::unpack(blob, t0, v)) return false;
    for (auto& a : v) { Rec r; r.ac = a; r.last = now; ac_[a.icao] = r; }
    return true;
}

} // namespace dect2
