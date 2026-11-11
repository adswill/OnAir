// See coverage.h.
#include "dect2/trunkpool/coverage.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace tpool {

double radioMaxRateHz(const std::string& name) {
    std::string s;
    for (char c : name) s += (char)std::tolower((unsigned char)c);
    auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
    if (has("rtl")) return 2.4e6;                      // 2.56 is the top; above 2.4 most dongles drop samples
    if (has("hackrf")) return 20e6;
    if (has("airspyhf") || has("airspy hf")) return 0.768e6;
    if (has("airspy")) return 10e6;
    if (has("sdrplay") || has("rsp")) return 10e6;
    if (has("pluto")) return 4e6;                      // USB 2: about 4 Msps of 16-bit samples without loss
    if (has("bladerf")) return 40e6;
    if (has("lime")) return 30.72e6;
    if (has("usrp") || has("b200") || has("b210")) return 56e6;
    if (has("funcube")) return 0.192e6;
    return 0;
}

bool insideCapture(double hz, double centreHz, double rateHz, double edgeGuardHz) {
    if (!(rateHz > 0) || !(hz > 0)) return false;
    return std::fabs(hz - centreHz) <= rateHz * kUsableFraction / 2 - edgeGuardHz;
}

double spanToCoverHz(const std::vector<double>& hz, double edgeGuardHz) {
    double lo = 0, hi = 0;
    bool any = false;
    for (double f : hz) {
        if (!(f > 0)) continue;
        if (!any) { lo = hi = f; any = true; }
        lo = std::min(lo, f); hi = std::max(hi, f);
    }
    return any ? hi - lo + 2 * edgeGuardHz : 0;
}

double rateToCoverHz(const std::vector<double>& hz, double edgeGuardHz) { return spanToCoverHz(hz, edgeGuardHz) / kUsableFraction; }

std::string outsideText(double hz, double controlHz, double centreHz, double edgeGuardHz, double maxRateHz) {
    double need;
    if (controlHz > 0) need = rateToCoverHz({controlHz, hz}, edgeGuardHz);
    else need = 2 * (std::fabs(hz - centreHz) + edgeGuardHz) / kUsableFraction;
    char b[160];
    if (maxRateHz > 0 && need > maxRateHz) snprintf(b, sizeof b, "outside the capture (needs %.1f MHz; this radio reaches %.1f MHz)", need / 1e6, maxRateHz / 1e6);
    else snprintf(b, sizeof b, "outside the capture (needs %.1f MHz)", need / 1e6);
    return b;
}

void ChannelUsage::add(double hz, double nowSec) {
    if (!(hz > 0)) return;
    auto it = std::find_if(v_.begin(), v_.end(), [&](const ChannelUse& c) { return std::fabs(c.hz - hz) < 100; });
    if (it == v_.end()) { v_.push_back({hz, 1, nowSec}); it = v_.end() - 1; }
    else { it->grants++; it->lastSec = nowSec; }
    while (it != v_.begin() && (it - 1)->grants < it->grants) { std::iter_swap(it, it - 1); --it; }   // most used first
    if (v_.size() > 512) v_.pop_back();
}

CoveragePlan planCoverage(const CoverageInput& in) {
    CoveragePlan p;
    struct Pt { double hz; uint64_t w; bool cc; };
    std::vector<Pt> pts;
    if (in.controlHz > 0) pts.push_back({in.controlHz, 0, true});
    for (const ChannelUse& c : in.channels) {
        if (!(c.hz > 0)) continue;
        if (in.controlHz > 0 && std::fabs(c.hz - in.controlHz) < 100) continue;   // the control channel itself
        pts.push_back({c.hz, c.grants, false});
        p.total++; p.grantsTotal += c.grants;
    }
    if (pts.empty()) { p.summary = "no channel known yet: the plan is made from the grants the control channel sends"; return p; }
    std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) { return a.hz < b.hz; });
    const double edge = std::max(0.0, in.edgeGuardHz), dc = std::max(0.0, in.dcGuardHz);
    std::vector<double> all;
    for (const Pt& q : pts) all.push_back(q.hz);
    p.spanAllHz = spanToCoverHz(all, edge);
    p.rateAllHz = p.spanAllHz / kUsableFraction;
    const double maxRate = in.maxRateHz > 0 ? in.maxRateHz : std::max(p.rateAllHz + 2 * dc / kUsableFraction, 1e6) * 1.05;
    const double minRate = std::min(maxRate, in.minRateHz > 0 ? in.minRateHz : 1e6);
    const double inner = maxRate * kUsableFraction - 2 * edge;   // the widest extent of channels one capture holds
    // the window [pts[i], pts[j]] of extent <= inner that holds the control channel and the most grants (then the most channels, then the narrowest)
    size_t bi = 0, bj = 0;
    uint64_t bw = 0; int bn = -1; double be = 1e300;
    for (size_t i = 0; i < pts.size(); i++) {
        uint64_t w = 0; int nch = 0; bool cc = in.controlHz <= 0;
        for (size_t j = i; j < pts.size(); j++) {
            if (pts[j].hz - pts[i].hz > inner) break;
            if (pts[j].cc) cc = true; else { w += pts[j].w; nch++; }
            if (!cc) continue;
            const double e = pts[j].hz - pts[i].hz;
            if (w > bw || (w == bw && nch > bn) || (w == bw && nch == bn && e < be)) { bw = w; bn = nch; be = e; bi = i; bj = j; }
        }
    }
    if (bn < 0) {   // the control channel alone (no voice channel fits beside it)
        for (size_t i = 0; i < pts.size(); i++) if (pts[i].cc) { bi = bj = i; bn = 0; be = 0; }
    }
    const double lo = pts[bi].hz, hi = pts[bj].hz;
    // the rate: what holds the window with room to move the centre off every channel, rounded up to 100 kHz, inside the radio's limits
    double rate = (hi - lo + 2 * edge + 2 * dc) / kUsableFraction;
    rate = std::ceil(rate / 1e5) * 1e5;
    rate = std::min(maxRate, std::max(minRate, rate));
    const double half = rate * kUsableFraction / 2 - edge;
    double cLo = hi - half, cHi = lo + half;
    if (cLo > cHi) cLo = cHi = 0.5 * (lo + hi);
    const double mid = std::min(cHi, std::max(cLo, 0.5 * (lo + hi)));
    // the centre: the point of [cLo, cHi] farthest from every channel inside the window (the DC spike sits on the centre)
    auto clearance = [&](double c) {
        double m = 1e300;
        for (size_t k = bi; k <= bj; k++) m = std::min(m, std::fabs(pts[k].hz - c));
        return m;
    };
    std::vector<double> cand{mid, cLo, cHi};
    for (size_t k = bi; k + 1 <= bj; k++) cand.push_back(0.5 * (pts[k].hz + pts[k + 1].hz));
    for (size_t k = bi; k <= bj; k++) { cand.push_back(pts[k].hz - dc); cand.push_back(pts[k].hz + dc); }
    double best = mid, bestClr = -1;
    for (double c : cand) {
        if (c < cLo - 1 || c > cHi + 1) continue;
        c = std::min(cHi, std::max(cLo, c));
        const double clr = std::min(clearance(c), dc * 4);   // beyond a few guards the distance no longer matters: stay near the middle
        if (clr > bestClr + 1 || (std::fabs(clr - bestClr) <= 1 && std::fabs(c - mid) < std::fabs(best - mid))) { bestClr = clr; best = c; }
    }
    p.centreHz = std::round(best / 100) * 100;
    p.rateHz = rate;
    p.dcClear = clearance(p.centreHz) >= dc;
    p.ok = true;
    for (const Pt& q : pts) {
        const bool in1 = insideCapture(q.hz, p.centreHz, p.rateHz, edge);
        if (q.cc) { p.controlInside = in1; continue; }
        if (in1) { p.covered++; p.grantsCovered += q.w; }
        else p.outside.push_back(q.hz);
    }
    if (in.controlHz <= 0) p.controlInside = true;
    // the outside channels, most used first
    std::sort(p.outside.begin(), p.outside.end(), [&](double a, double b) {
        uint64_t wa = 0, wb = 0;
        for (const Pt& q : pts) { if (q.hz == a) wa = q.w; if (q.hz == b) wb = q.w; }
        return wa > wb;
    });
    char b[320];
    const int pct = p.grantsTotal ? (int)std::lround(100.0 * (double)p.grantsCovered / (double)p.grantsTotal) : 0;
    int n = snprintf(b, sizeof b, "Centre %.4f MHz at %.1f Msps: the control channel and %d of %d voice channels (%d %% of the calls)", p.centreHz / 1e6, p.rateHz / 1e6, p.covered, p.total, pct);
    if (n > 0 && (size_t)n < sizeof b && p.covered < p.total) {
        if (in.maxRateHz > 0 && p.rateAllHz > in.maxRateHz)
            snprintf(b + n, sizeof b - (size_t)n, "; all of them need %.1f MHz, this radio reaches %.1f MHz", p.rateAllHz / 1e6, in.maxRateHz / 1e6);
        else snprintf(b + n, sizeof b - (size_t)n, "; all of them need %.1f MHz", p.rateAllHz / 1e6);
    }
    p.summary = b;
    return p;
}

} // namespace tpool
} // namespace dect2
