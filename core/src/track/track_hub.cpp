// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/track/track_hub.h"
#include "dect2/acars/acars_tel.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/hfdl/hfdl_tel.h"
#include "dect2/vdl2/vdl2_tel.h"
#include <algorithm>
#include <cmath>

namespace dect2::track {

Key keyFromIcao(uint32_t icao) { return (Key)(icao & 0x1FFFFFFu); }
Key keyFromText(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return (h & 0x0000FFFFFFFFFFFFull) | (1ull << 56);
}

bool fixFromAdsb(const AdsbAircraft& a, double now, Fix& f) {
    f = Fix();
    f.src = Src::Adsb;
    const bool uat = a.srcId == 255;      // UAT feeds are marked by the caller through srcId; the default is 1090 MHz
    if (uat) f.src = Src::Uat;
    f.t = now - (a.hasPos ? std::max(0.0, (double)a.posAgeSec) : std::max(0.0, (double)a.ageSec));
    if (a.hasPos && a.posAgeSec < 30) {
        f.hasPos = true; f.lat = a.lat; f.lon = a.lon; f.nacp = a.nacp;
        if (a.posKind == 1) f.posSigmaM = 400;              // local decode from the receiver position, not confirmed: wide
    }
    if (a.hasAlt) { f.hasAlt = true; f.altM = a.altFt * kFt; }
    if (a.hasVrate) { f.hasVrate = true; f.vrateMs = a.vrateFpm * kFpm; }
    if (a.hasSpeed && a.hasHeading && a.speedKind == 0 && a.headingIsTrack) { f.hasVel = true; f.speedMs = a.speedKt * kKt; f.trackDeg = a.headingDeg; }
    else if (a.hasSpeed && a.hasHeading && a.speedKind == 2 && !a.headingIsTrack) { f.hasVel = true; f.velIsAir = true; f.speedMs = a.speedKt * kKt; f.trackDeg = a.headingDeg; }
    if (a.hasRoll) { f.hasRoll = true; f.rollDeg = a.rollDeg; }
    if (a.hasWindDer) { f.hasWind = true; f.windFromDeg = a.windDerDir; f.windMs = a.windDerKt * kKt; }
    else if (a.hasWindRep) { f.hasWind = true; f.windFromDeg = a.windRepDir; f.windMs = a.windRepKt * kKt; }
    f.onGround = a.ground;
    return f.hasPos || f.hasAlt || f.hasVel;
}

bool fixFromAcars(const AcarsAircraft& a, Fix& f) {
    f = Fix(); f.src = Src::Acars;
    if (!a.hasPos) return false;
    f.t = a.posTime; f.hasPos = true; f.lat = a.lat; f.lon = a.lon;
    if (a.posSource == 1) f.timeSigmaSec = 6;                                // ADS-C carries its own time stamp
    else f.resolutionDeg = 0.1 / 60.0;                                       // text reports: tenths of a minute is the common layout
    if (a.hasAlt) { f.hasAlt = true; f.altM = a.altFt * kFt; f.altSigmaM = 40; }   // flight levels are written in hundreds of feet
    if (a.hasTrack && a.hasSpeed && a.posSource == 1) { f.hasVel = true; f.trackDeg = a.trackDeg; f.speedMs = a.speedKt * kKt; f.speedSigmaMs = 5; f.trackSigmaDeg = 5; }
    return true;
}

bool fixFromVdl2(const Vdl2Aircraft& a, Fix& f) {
    f = Fix(); f.src = Src::Vdl2;
    if (!a.hasPos) return false;
    f.t = a.posTime; f.hasPos = true; f.lat = a.lat; f.lon = a.lon;
    f.resolutionDeg = a.posKind.find("ADS-C") != std::string::npos ? 0.0 : 0.1 / 60.0;
    if (a.hasAlt) { f.hasAlt = true; f.altM = a.altFt * kFt; f.altSigmaM = 15; }
    if (a.hasKin) {
        f.hasVel = true; f.trackDeg = a.trackDeg; f.speedMs = a.groundKt * kKt; f.speedSigmaMs = 3; f.trackSigmaDeg = 3;
        f.hasVrate = true; f.vrateMs = a.vsFpm * kFpm; f.vrateSigmaMs = 2;
    }
    f.onGround = a.onGround;
    return true;
}

bool fixFromHfdl(const HfdlAircraft& a, Fix& f) {
    f = Fix(); f.src = Src::Hfdl;
    if (!a.hasPos) return false;
    f.t = a.posTime; f.hasPos = true; f.lat = a.lat; f.lon = a.lon; f.resolutionDeg = 1.0 / 600.0;
    if (a.hasAlt) { f.hasAlt = true; f.altM = a.altFt * kFt; f.altSigmaM = 40; }
    return true;
}

UpdateResult TrackHub::feed(Key k, const Fix& f0) {
    auto it = e_.find(k);
    if (it == e_.end()) it = e_.emplace(k, Entry(opt_)).first;
    Entry& e = it->second;
    Fix f = f0;
    if (f.hasPos) { e.lastLat = f.lat; e.lastLon = f.lon; e.hasPos = true; }
    if (f.hasAlt) { e.lastAltM = f.altM; e.hasAlt = true; }
    e.lastT = std::max(e.lastT, f.t);
    if (useWindField) {
        if (f.hasWind && e.hasPos) {
            const double we = -f.windMs * std::sin(f.windFromDeg * kDeg), wn = -f.windMs * std::cos(f.windFromDeg * kDeg);
            wind_.add(f.t, e.lastLat, e.lastLon, e.hasAlt ? e.lastAltM : 10000, we, wn);
        } else if (e.hasPos && (f.velIsAir || f.src == Src::Acars || f.src == Src::Vdl2 || f.src == Src::Hfdl)) {
            double we, wn, sg;
            if (wind_.at(f.t, e.lastLat, e.lastLon, e.hasAlt ? e.lastAltM : 10000, we, wn, sg)) e.est.setWindPrior(we, wn, sg);
        }
    }
    return e.est.update(f);
}

bool TrackHub::feedText(Key k, const std::string& label, const std::string& text, double now, double utcSecOfDay) {
    RouteInfo ri;
    if (!parseRouteText(label, text, ri)) return false;
    auto it = e_.find(k);
    if (it == e_.end()) it = e_.emplace(k, Entry(opt_)).first;
    RouteHint h = it->second.est.route();
    if (const Airport* a = findAirport(ri.dest)) { h.hasDest = true; h.destLat = a->lat; h.destLon = a->lon; }
    if (const Airport* a = findAirport(ri.origin)) { h.hasOrigin = true; h.origLat = a->lat; h.origLon = a->lon; }
    if (ri.etaSec >= 0 && utcSecOfDay >= 0) {
        double d = ri.etaSec - utcSecOfDay;
        if (d < -3600) d += 86400;                       // an ETA after midnight
        h.etaT = now + d;
    }
    it->second.est.setRoute(h);
    return true;
}

void TrackHub::setRoute(Key k, const RouteHint& r) {
    auto it = e_.find(k);
    if (it == e_.end()) it = e_.emplace(k, Entry(opt_)).first;
    it->second.est.setRoute(r);
}

void TrackHub::setStation(uint32_t id, double lat, double lon, double rangeNm) { st_[id] = {lat, lon, rangeNm}; }

void TrackHub::noteStation(Key k, uint32_t id, double t) {
    auto it = e_.find(k);
    if (it == e_.end()) it = e_.emplace(k, Entry(opt_)).first;
    it->second.station = id; it->second.stationT = t;
}

bool TrackHub::estimate(Key k, double now, PosEstimate& o, bool quick) const {
    auto it = e_.find(k);
    if (it == e_.end()) { o = PosEstimate(); return false; }
    const Entry& e = it->second;
    if (e.est.started()) return e.est.estimate(now, o, quick);
    auto s = st_.find(e.station);
    o = PosEstimate();
    if (e.stationT < -1e17 || s == st_.end()) return false;
    const double alt = e.hasAlt ? e.lastAltM / kFt : 35000.0;
    const double rangeM = (s->second.rangeNm > 0 ? s->second.rangeNm : losRangeNm(alt)) * kNm;
    o.valid = true; o.coarse = true; o.predicted = true;
    o.lat = s->second.lat; o.lon = s->second.lon;
    o.ellipse.majorM = o.ellipse.minorM = rangeM * 0.97;      // a uniform disc of radius R has its 95 % circle at 0.97 R
    o.sigmaH = rangeM / std::sqrt(2.0);
    o.cEE = o.cNN = o.sigmaH * o.sigmaH * 0.5;
    o.ageSec = std::max(0.0, now - e.stationT); o.posAgeSec = o.ageSec; o.staleness = std::min(1.0, o.ageSec / 3600.0);
    o.confidence = 0.05; o.mainSrc = Src::Vdl2;
    return true;
}

void TrackHub::purge(double now, double maxAgeSec) {
    for (auto it = e_.begin(); it != e_.end();) {
        const double last = std::max(it->second.lastT, it->second.stationT);
        if (now - last > maxAgeSec) it = e_.erase(it); else ++it;
    }
}

namespace {
std::vector<const TrackHub*>& hubs() { static auto* v = new std::vector<const TrackHub*>; return *v; }   // never destroyed: the screens' hubs unregister at program exit
}

void registerHub(const TrackHub* h) {
    auto& v = hubs();
    if (std::find(v.begin(), v.end(), h) == v.end()) v.push_back(h);
}

void unregisterHub(const TrackHub* h) {
    auto& v = hubs();
    v.erase(std::remove(v.begin(), v.end(), h), v.end());
}

bool liveFix(Key k, double& lat, double& lon) {
    double best = -1e18;
    bool got = false;
    for (const TrackHub* h : hubs()) {
        const TrackEstimator* e = h->find(k);
        if (!e) continue;
        const double t = e->lastFixT();
        if (t <= best) continue;
        PosEstimate p;
        if (!h->estimate(k, t, p, true) || !p.valid) continue;
        lat = p.lat; lon = p.lon; best = t; got = true;
    }
    return got;
}

}  // namespace dect2::track
