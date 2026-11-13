// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/geo/maplayers.h"
#include "dect2/tetra/tetra_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace {
constexpr double kPi = dect2::math::kPi, kEarthKm = 6371.0;
std::string hex(uint32_t v, int w) { char b[16]; snprintf(b, sizeof b, "%0*X", w, v); return b; }
}

std::string ageText(double s) {
    char b[48];
    if (s < 0) s = 0;
    if (s < 60) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 3600) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%d h %d min", (int)(s / 3600), (int)std::fmod(s / 60, 60));
    return b;
}

std::vector<MapMarker> markersFromAdsb(const AdsbTelemetry& t) {
    std::vector<MapMarker> v;
    for (const AdsbAircraft& a : t.aircraft) {
        if (!a.hasPos) continue;
        MapMarker m; m.layer = LayerAdsb; m.id = hex(a.icao & 0xFFFFFF, 6);
        m.label = a.callsign.empty() ? m.id : a.callsign;
        m.lat = a.lat; m.lon = a.lon;
        if (a.hasAlt) { m.hasAlt = true; m.altM = a.altFt * 0.3048; }
        if (a.hasSpeed) { m.hasSpeed = true; m.speedKt = a.speedKt; }
        if (a.hasHeading) m.headingDeg = a.headingDeg;
        m.ageSec = a.posAgeSec > 0 ? a.posAgeSec : a.ageSec;
        m.detail = a.category.empty() ? (a.ground ? "on the ground" : "") : "category " + a.category;
        for (const auto& p : a.track) m.track.push_back({p.lat, p.lon});
        v.push_back(std::move(m));
    }
    return v;
}

std::vector<MapMarker> markersFromAis(const AisTelemetry& t) {
    std::vector<MapMarker> v;
    for (const AisVessel& s : t.vessels) {
        if (!s.hasPos) continue;
        MapMarker m; m.layer = LayerAis; m.id = std::to_string(s.mmsi);
        m.label = s.name.empty() ? m.id : s.name;
        m.lat = s.lat; m.lon = s.lon;
        if (s.sog >= 0) { m.hasSpeed = true; m.speedKt = s.sog; }
        m.headingDeg = s.heading >= 0 ? s.heading : (s.cog >= 0 ? s.cog : -1);
        m.ageSec = s.ageSec;
        m.detail = s.destination.empty() ? s.callsign : "to " + s.destination;
        if (s.longRange) m.detail = "long-range AIS (message 27)" + (m.detail.empty() ? std::string() : ", " + m.detail);
        if (s.altitudeM >= 0) { m.hasAlt = true; m.altM = s.altitudeM; }
        for (const auto& p : s.track) m.track.push_back({p.lat, p.lon});
        v.push_back(std::move(m));
    }
    return v;
}

std::vector<MapMarker> markersFromTetra(const TetraTelemetry& t) {
    std::vector<MapMarker> v;
    for (const TetraSdsRec& s : t.sds) {
        if (!s.hasPos) continue;
        MapMarker m; m.layer = LayerDmr; m.id = "tetra-" + std::to_string(s.fromIssi); m.label = "TETRA " + std::to_string(s.fromIssi);
        m.lat = s.lat; m.lon = s.lon; m.ageSec = 0; m.detail = "LIP position";
        v.push_back(std::move(m));
    }
    return v;
}

std::vector<MapMarker> markersFromVdes(const VdesTelemetry& t) {
    std::vector<MapMarker> v;
    std::vector<uint32_t> seen;
    for (const VdesAsm& a : t.asms) {   // newest first: the first frame of an MMSI is its latest position
        if (!a.hasPos || !a.aisMmsi) continue;
        if (std::find(seen.begin(), seen.end(), a.aisMmsi) != seen.end()) continue;
        seen.push_back(a.aisMmsi);
        MapMarker m; m.layer = LayerAis; m.id = std::to_string(a.aisMmsi);
        m.label = "VDES " + m.id;
        m.lat = a.lat; m.lon = a.lon;
        if (a.sogKt >= 0 && a.sogKt < 102.3f) { m.hasSpeed = true; m.speedKt = a.sogKt; }
        m.headingDeg = a.cogDeg >= 0 && a.cogDeg < 360 ? a.cogDeg : -1;
        m.ageSec = std::max(0.0, t.timeSec - a.timeSec);
        m.detail = "VDES ASM, AIS message " + std::to_string(a.aisType);
        v.push_back(std::move(m));
    }
    return v;
}

std::vector<MapMarker> markersFromAprs(const PacketTelemetry& t) {
    std::vector<MapMarker> v;
    for (const PacketStation& s : t.stations) {
        if (!s.hasPos) continue;
        MapMarker m; m.layer = LayerAprs; m.id = s.call; m.label = s.call;
        m.lat = s.lat; m.lon = s.lon;
        if (s.hasAlt) { m.hasAlt = true; m.altM = s.altM; }
        if (s.hasSpeed) { m.hasSpeed = true; m.speedKt = s.speedKnots; }
        if (s.hasCourse) m.headingDeg = s.courseDeg;
        m.ageSec = std::max(0.0, t.timeSec - s.lastHeardSec);
        m.detail = s.comment;
        v.push_back(std::move(m));
    }
    return v;
}

std::vector<MapMarker> markersFromSonde(const SondeTelemetry& t) {
    std::vector<MapMarker> v;
    for (const SondeInfo& s : t.sondes) {
        if (!s.hasPos) continue;
        MapMarker m; m.layer = LayerSonde; m.id = s.serial; m.label = s.serial;
        m.lat = s.lat; m.lon = s.lon; m.hasAlt = true; m.altM = s.altM;
        if (s.hasVel) { m.hasSpeed = true; m.speedKt = s.hSpeed * 1.943844; m.headingDeg = s.headingDeg; }
        m.ageSec = s.lastHeardS;
        m.detail = s.type + (s.hasVel && s.vSpeed != 0 ? (s.vSpeed < 0 ? ", falling" : ", rising") : "");
        for (const auto& p : s.track) m.track.push_back({p.lat, p.lon});
        v.push_back(std::move(m));
    }
    return v;
}

std::vector<MapMarker> markersFromDmr(const DmrTelemetry& t) {
    std::vector<MapMarker> v;
    const double now = (double)std::time(nullptr);
    // newest first, so the first fix of each radio in this backwards walk is its latest position
    for (size_t i = t.locations.size(); i-- > 0;) {
        const DmrLocation& loc = t.locations[i];
        if (!loc.hasFix) continue;
        const std::string id = std::to_string(loc.src);
        bool have = false;
        for (const MapMarker& m : v) if (m.id == id) { have = true; break; }
        if (have) continue;
        MapMarker m; m.layer = LayerDmr; m.id = id; m.label = id;
        m.lat = loc.lat; m.lon = loc.lon;
        if (loc.hasSpeed) { m.hasSpeed = true; m.speedKt = loc.speedMs * 1.943844; }
        m.headingDeg = loc.hasHeading ? loc.headingDeg : -1;
        m.ageSec = std::max(0.0, now - loc.unixSec);
        char b[64]; snprintf(b, sizeof b, "TG %u, slot %d", loc.dst, loc.slot);
        m.detail = b;
        v.push_back(std::move(m));
    }
    for (MapMarker& m : v) {   // the movement track of each radio, oldest first
        const uint32_t src = (uint32_t)std::stoul(m.id);
        for (const DmrLocation& loc : t.locations)
            if (loc.src == src && loc.hasFix) m.track.push_back({(float)loc.lat, (float)loc.lon});
    }
    return v;
}

double footprintRadiusKm(double altKm, double minElDeg) {
    if (altKm <= 0) return 0;
    const double e = minElDeg * kPi / 180.0;
    const double c = kEarthKm / (kEarthKm + altKm) * std::cos(e);
    return kEarthKm * (std::acos(std::min(1.0, c)) - e);
}

bool satelliteMapPoint(const Sgp4& s, double unixSec, double pastMin, double futureMin, double stepMin, SatMapPoint& out, double minElDeg) {
    out = SatMapPoint();
    if (!s.ok() || stepMin <= 0) return false;
    out.name = s.tle().name; out.norad = s.tle().norad;
    if (!subPoint(s, unixSec, out.lat, out.lon, out.altKm)) return false;
    out.footprintKm = footprintRadiusKm(out.altKm, minElDeg);
    const int a = (int)std::floor(pastMin / stepMin), b = (int)std::ceil(futureMin / stepMin);
    for (int i = -a; i <= b; i++) {
        double la, lo, al;
        if (!subPoint(s, unixSec + i * stepMin * 60.0, la, lo, al)) continue;
        if (i <= 0) out.nowIndex = out.track.size();
        out.track.push_back({la, lo});
    }
    return true;
}

std::vector<std::pair<double, double>> circlePoints(double lat, double lon, double radiusKm, int n) {
    std::vector<std::pair<double, double>> v;
    const double d = radiusKm / kEarthKm, la = lat * kPi / 180, lo = lon * kPi / 180;
    for (int i = 0; i < n; i++) {
        const double b = 2 * kPi * i / n;
        const double la2 = std::asin(std::sin(la) * std::cos(d) + std::cos(la) * std::sin(d) * std::cos(b));
        const double lo2 = lo + std::atan2(std::sin(b) * std::sin(d) * std::cos(la), std::cos(d) - std::sin(la) * std::sin(la2));
        v.push_back({la2 * 180 / kPi, std::fmod(std::fmod(lo2 * 180 / kPi + 180, 360) + 360, 360) - 180});
    }
    return v;
}

}
