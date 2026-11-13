// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Derived ground speed, track and vertical rate for the aircraft that report positions only (ACARS, VDL2, HFDL): the maths is in
// core/include/dect2/geo/kinematics.h. Here: the fixes out of the track points, the choice between measured / estimated / derived, and the
// text for the screens and the map upload. Header only.
#pragma once
#include "dect2/geo/kinematics.h"
#include "dect2/track/track_hub.h"
#include "i18n/i18n.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace derivedkin {

using dect2::geo::DerivedKin;
using dect2::geo::KinSource;
using dect2::geo::KinValue;

struct Info {
    KinValue v;
    DerivedKin d;      // the two-point figures, whether or not they were the ones shown
};

template <class P> inline dect2::geo::KinFix fixOf(const P& p) {
    dect2::geo::KinFix f;
    f.lat = p.lat; f.lon = p.lon; f.t = p.time;
    if constexpr (requires { p.altFt; }) { f.hasAlt = true; f.altFt = p.altFt; }
    if constexpr (requires { p.repSecOfDay; }) f.repSecOfDay = p.repSecOfDay;
    return f;
}

// The estimator's filtered state, when it has seen enough fixes (the first pair stays with the two-point value).
inline dect2::geo::KinEstimated estimated(const dect2::track::TrackHub& hub, bool predictOn, dect2::track::Key k, double now, size_t nFixes) {
    dect2::geo::KinEstimated e;
    if (!predictOn || nFixes < 3) return e;
    dect2::track::PosEstimate pe;
    if (!hub.estimate(k, now, pe, true) || !pe.valid || pe.coarse) return e;
    if (!(pe.speedKt > 0) || pe.speedKt > 1000 || pe.ageSec > 2400) return e;
    e.valid = true; e.gsKt = pe.speedKt; e.trackDeg = pe.trackDeg; e.vsFpm = pe.vrateFpm;
    return e;
}

template <class P>
inline Info compute(const std::vector<P>& track, const dect2::geo::KinMeasured& m, const dect2::geo::KinEstimated& e) {
    Info i;
    i.d = dect2::geo::deriveFromTrack((int)track.size(), [&](int n) { return fixOf(track[(size_t)n]); });
    i.v = dect2::geo::chooseKin(m, e, i.d);
    return i;
}

inline std::string durText(double sec) {
    char b[24];
    if (sec < 90) snprintf(b, sizeof b, "%.0f s", sec); else if (sec < 5400) snprintf(b, sizeof b, "%.0f min", sec / 60); else snprintf(b, sizeof b, "%.1f h", sec / 3600);
    return b;
}

inline const char* marker(const KinValue& v) { return v.src == KinSource::Measured ? "" : "~"; }

// "~452 kt +-12", "450 kt" (measured)
inline std::string gsText(const KinValue& v) {
    if (!v.hasGs) return "-";
    char b[64];
    if (v.gsSigmaKt > 0) snprintf(b, sizeof b, "~%.0f kt \xC2\xB1%.0f", v.gsKt, std::max(1.0, std::round(v.gsSigmaKt)));
    else snprintf(b, sizeof b, "%s%.0f kt", marker(v), v.gsKt);
    return b;
}
inline std::string trackText(const KinValue& v) {
    if (!v.hasTrack) return "-";
    char b[64];
    if (v.averaged && v.src == KinSource::Derived) snprintf(b, sizeof b, tr("%s%.0f deg (average)"), marker(v), v.trackDeg);
    else snprintf(b, sizeof b, "%s%.0f deg", marker(v), v.trackDeg);
    return b;
}
inline std::string vsText(const KinValue& v) {
    if (!v.hasVs) return "-";
    char b[40];
    snprintf(b, sizeof b, "%s%+.0f fpm", marker(v), v.vsFpm);
    return b;
}

// What the "~" means.
inline std::string explain(const Info& i) {
    char b[320];
    switch (i.v.src) {
        case KinSource::Measured: return tr("Reported by the aircraft.");
        case KinSource::Estimated: return tr("Estimated by the position filter from the position reports (not broadcast by the aircraft).");
        case KinSource::Derived:
            snprintf(b, sizeof b, i.v.averaged ? tr("Derived from 2 positions %s apart: an average over that time; a curved path is longer than the straight line, so the real speed may be higher. The track is the average bearing.")
                                               : tr("Derived from 2 positions %s apart (distance on the WGS-84 ellipsoid divided by the time). The aircraft did not report a speed."),
                     durText(i.v.dtSec).c_str());
            return b;
        default: return "";
    }
}

// Key/value lines for the detail panes: returns false when there is nothing to show.
struct Lines { std::string gs, track, vs, note; };
inline bool lines(const Info& i, Lines& L) {
    if (!(i.v.hasGs || i.v.hasTrack || i.v.hasVs)) return false;
    L.gs = gsText(i.v); L.track = trackText(i.v); L.vs = vsText(i.v); L.note = explain(i);
    return true;
}

// fields for the map upload / CSV: only the derived figures
inline bool isDerived(const Info& i) { return i.v.src == KinSource::Derived; }

}  // namespace derivedkin
