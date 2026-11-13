// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fastscan/qualify.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace fastscan {

const char* sigClassName(SigClass c) {
    switch (c) {
        case SigClass::Carrier: return "Carrier";
        case SigClass::Voice: return "Voice";
        case SigClass::Narrowband: return "Narrowband";
        case SigClass::Wideband: return "Wideband";
        case SigClass::Broadcast: return "Broadcast";
        case SigClass::Digital: return "Digital";
        default: return "?";
    }
}

SigClass classify(double f, double bw, double burst) {
    if (bw < 2500) return SigClass::Carrier;
    if (bw <= 16000) return SigClass::Voice;
    if (bw <= 40000) return burst < 0.6 ? SigClass::Voice : SigClass::Narrowband;
    if (bw <= 110000) return SigClass::Digital;
    if (bw <= 300000) return (f >= 87.5e6 && f <= 108.5e6) ? SigClass::Broadcast : SigClass::Wideband;
    return SigClass::Digital;
}

std::string classHint(double f, double bw, double burst) {
    const std::string mode = guessMode(f, bw);
    const bool am = mode == "am";
    switch (classify(f, bw, burst)) {
        case SigClass::Carrier: return "carrier";
        case SigClass::Voice: return burst < 0.6 ? (am ? "AM voice (keyed)" : "NFM voice (keyed)") : (am ? "AM" : "NFM");
        case SigClass::Narrowband: return "narrow digital / FM";
        case SigClass::Digital: return bw > 300000 ? "wide digital (DAB/TV/LTE)" : "digital";
        case SigClass::Broadcast: return "WFM broadcast";
        case SigClass::Wideband: return "wideband FM / data";
        default: return "";
    }
}

bool isImageOf(double f, float pk, double of, float opk, double centre, double tol, float rej) {
    if (std::fabs((f - centre) + (of - centre)) > tol) return false;      // mirrored offsets sum to zero
    if (std::fabs(f - centre) < tol) return false;                        // on the centre it is the DC spike, not an image
    return opk - pk >= rej;
}

void Qualifier::look(const FastPlan& plan, int hop, int sweep, double t, const std::vector<Hit>& hits) {
    if (hop < 0 || hop >= (int)plan.hops.size()) return;
    const Hop& h = plan.hops[hop];
    binHz_ = plan.binHz;
    const double tol = plan.binHz * p_.imageTolBins;
    const double dcw = p_.dcWidthHz > 0 ? p_.dcWidthHz : 2 * plan.binHz;
    std::vector<const Hit*> in;
    for (const Hit& x : hits) if (x.freqHz >= h.loHz - plan.binHz && x.freqHz <= h.hiHz + plan.binHz) in.push_back(&x);
    for (const Hit* x : in) {
        St& s = st_[x->id];
        s.looks++;
        const bool present = x->lastSeen >= t - 1e-6;
        if (present) {
            s.seen++;
            if (s.lastSweep != sweep) { s.sweeps++; s.lastSweep = sweep; }
            const double d = x->peakDb - s.mean;
            s.mean += d / s.seen;
            s.m2 += d * (x->peakDb - s.mean);
            s.bwMax = std::max(s.bwMax, x->bwHz);
        }
        s.dc = std::fabs(x->freqHz - h.centerHz) <= dcw && x->bwHz <= 3 * plan.binHz;
        s.image = false;
        for (const Hit* o : in) if (o != x && isImageOf(x->freqHz, x->peakDb, o->freqHz, o->peakDb, h.centerHz, tol, p_.imageRejectDb)) { s.image = true; break; }
    }
}

std::vector<ChannelInfo> Qualifier::channels(const std::vector<Hit>& hits, bool reliableOnly) const {
    std::vector<ChannelInfo> out;
    out.reserve(hits.size());
    for (const Hit& h : hits) {
        ChannelInfo c;
        c.hit = h;
        auto it = st_.find(h.id);
        if (it != st_.end()) {
            const St& s = it->second;
            c.sweeps = s.sweeps; c.looks = s.looks;
            c.presence = s.looks > 0 ? (float)s.seen / (float)s.looks : 0.f;
            c.peakMeanDb = (float)s.mean;
            c.peakStdDb = s.seen > 1 ? (float)std::sqrt(s.m2 / (s.seen - 1)) : 0.f;
            c.confirmed = s.sweeps >= p_.confirmSweeps;
            c.image = s.image; c.dc = s.dc;
            c.spur = s.looks >= p_.steadyLooks && c.presence >= p_.steadyMinPresence && c.peakStdDb < p_.steadyStdDb && h.bwHz < 12 * binHz_;
        }
        c.cls = classify(h.freqHz, h.bwHz, c.presence > 0 ? c.presence : 1.f);
        c.hint = classHint(h.freqHz, h.bwHz, c.presence > 0 ? c.presence : 1.f);
        if (!reliableOnly || c.reliable()) out.push_back(std::move(c));
    }
    return out;
}

}  // namespace fastscan
}  // namespace dect2
