// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Theoretical range outline: see adsb_horizon.h.
#include "dect2/adsb/adsb_horizon.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "dect2/common/diag.h"

namespace dect2 {

namespace js = automation::json;
namespace {
constexpr double kR = 6371.0, kK = 4.0 / 3.0, kDeg = 3.14159265358979323846 / 180.0;
}

void horizonDest(double lat, double lon, double brg, double km, double& la, double& lo) {
    const double d = km / kR, b = brg * kDeg, p1 = lat * kDeg, l1 = lon * kDeg;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    la = p2 / kDeg;
    lo = std::remainder(l2 / kDeg, 360.0);
}

bool parseUpintheair(const std::string& text, std::vector<HorizonRing>& out, std::string& err) {
    js::Value root;
    if (!js::parse(text, root, err)) return false;
    const js::Value* rings = root.get("rings");
    if (root.type != js::Value::Obj || !rings || rings->type != js::Value::Arr) { DIAG(400571, "The horizon file is not an upintheair.json file because it has no rings, so it was not loaded."); err = "not an upintheair.json file (no rings)"; return false; }
    std::vector<HorizonRing> v;
    for (const js::Value& r : rings->a) {
        HorizonRing h;
        const js::Value* alt = r.get("alt");
        if (!alt) continue;
        h.altM = alt->type == js::Value::Num ? alt->n : std::atof(alt->s.c_str());
        const js::Value* pts = r.get("points");
        if (!pts || pts->type != js::Value::Arr) continue;
        for (const js::Value& p : pts->a)
            if (p.type == js::Value::Arr && p.a.size() >= 2 && p.a[0].type == js::Value::Num && p.a[1].type == js::Value::Num && std::fabs(p.a[0].n) <= 90 && std::fabs(p.a[1].n) <= 180)
                h.pts.push_back({p.a[0].n, p.a[1].n});
        if (h.pts.size() >= 3) v.push_back(std::move(h));
    }
    if (v.empty()) { DIAG(288956, "The horizon file has no usable rings, so it was not loaded."); err = "the file has no usable rings"; return false; }
    out = std::move(v);
    return true;
}

std::vector<HorizonRing> terrainHorizon(const prop::Terrain& t, double lat, double lon, double aglM, const std::vector<double>& alts, int bearings, double maxKm, double stepKm) {
    std::vector<HorizonRing> out(alts.size());
    for (size_t i = 0; i < alts.size(); i++) out[i].altM = alts[i];
    double g0 = 0;
    if (!t.heightAt(lat, lon, g0)) g0 = 0;
    const double h0 = g0 + aglM;
    const double kRm = kK * kR * 1000;
    for (int b = 0; b < bearings; b++) {
        const double brg = 360.0 * b / bearings;
        double thMax = -1e9;                       // the highest terrain angle so far (radians, small-angle with the earth's bulge)
        std::vector<double> reach(alts.size(), maxKm);
        std::vector<bool> done(alts.size(), false);
        for (double d = stepKm; d <= maxKm + 1e-9; d += stepKm) {
            const double dm = d * 1000;
            for (size_t i = 0; i < alts.size(); i++) {
                if (done[i]) continue;
                const double phi = (alts[i] - h0) / dm - dm / (2 * kRm);   // the aircraft's angle at this distance
                if (phi < thMax) { reach[i] = d - stepKm; done[i] = true; }
            }
            double la, lo, g = 0;
            horizonDest(lat, lon, brg, d, la, lo);
            if (!t.heightAt(la, lo, g)) g = 0;
            thMax = std::max(thMax, (g - h0) / dm - dm / (2 * kRm));
        }
        for (size_t i = 0; i < alts.size(); i++) {
            double la, lo;
            horizonDest(lat, lon, brg, std::max(0.0, reach[i]), la, lo);
            out[i].pts.push_back({la, lo});
        }
    }
    return out;
}

}  // namespace dect2
