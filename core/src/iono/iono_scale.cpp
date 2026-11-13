// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/iono/iono_scale.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace iono {

namespace {

constexpr double kRowKm = kC * kDelayBin;     // group range of one row (15 km)
constexpr int kMaxPeaks = 8;                  // per column
constexpr double kPeakDb = 8;                 // a peak is at least this far over the column's median ...
constexpr double kOverQ90 = 4;                // ... and this far over its 90th percentile (keeps interference and noise out)
constexpr int kGap = 4;                       // columns a trace may skip
constexpr int kMinLen = 7;                    // points of a trace (350 kHz)
constexpr double kGroundKm = 80;              // a trace under this group range is the ground wave
constexpr double kHint = 200;                 // km: the mirror height assumed for a distance from the delay
constexpr double kHe = 110, kHf = 250;        // km: nominal virtual heights of the E and F modes (oblique)

double colMhz(int c) { return (c + 0.5) * kColHz / 1e6; }

struct Peak { int col; double row; double db; bool used = false; };

// the swept data runs on for at least two columns past c (so a trace ending at c really ends there)
bool sweptAfter(const std::vector<float>& db, int c) {
    for (int k = c + 1; k <= c + 2; k++) {
        if (k >= kCols) return false;
        bool any = false;
        for (int j = 0; j < kRows && !any; j++) any = !std::isnan(db[(size_t)j * kCols + (size_t)k]);
        if (!any) return false;
    }
    return true;
}

double topMhz(const IonoTrace& t) { return t.pts.empty() ? 0 : t.pts.back().fMhz; }
double lowMhz(const IonoTrace& t) { return t.pts.empty() ? 0 : t.pts.front().fMhz; }
int colOf(double fMhz) { return (int)std::floor(fMhz * 1e6 / kColHz); }
double minRange(const IonoTrace& t) { double m = 1e300; for (const auto& p : t.pts) m = std::min(m, (double)p.rangeKm); return m; }
double medRange(const IonoTrace& t) {
    std::vector<float> v; for (const auto& p : t.pts) v.push_back(p.rangeKm);
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
// range of trace t at frequency f (nearest point within a column), NAN if t has none there
double rangeAt(const IonoTrace& t, double f) {
    double best = NAN, bd = 0.6 * kColHz / 1e6;
    for (const auto& p : t.pts) if (std::fabs(p.fMhz - f) < bd) { bd = std::fabs(p.fMhz - f); best = p.rangeKm; }
    return best;
}
// linear interpolation of the range of t at f inside its span (NAN outside)
double rangeInterp(const IonoTrace& t, double f) {
    if (t.pts.empty() || f < t.pts.front().fMhz || f > t.pts.back().fMhz) return NAN;
    for (size_t i = 1; i < t.pts.size(); i++)
        if (t.pts[i].fMhz >= f) {
            const auto& a = t.pts[i - 1]; const auto& b = t.pts[i];
            const double u = b.fMhz > a.fMhz ? (f - a.fMhz) / (b.fMhz - a.fMhz) : 0;
            return a.rangeKm + u * (b.rangeKm - a.rangeKm);
        }
    return t.pts.back().rangeKm;
}

double circDiff(double a, double b, double p) { double d = std::fmod(std::fabs(a - b), p); return std::min(d, p - d); }

} // namespace

// ---- known sounders. None of these is checked against a live recording here: the rates and sites are from public chirp-sounder
// reports (GNU Radio chirpsounder, amateur logs) and the schedules are not known (0), so every entry is marked unverified.
const std::vector<KnownSounder>& knownSounders() {
    static const std::vector<KnownSounder> k = {
        {"Sodankyla (SGO), Finland", 500e3, 0, 0, 67.367, 26.629, false, "chirp ionosonde of the Sodankyla Geophysical Observatory; schedule not known"},
        {"Inskip, UK", 100e3, 0, 0, 53.830, -2.840, false, "oblique sounder; rate and schedule from listener reports"},
        {"Akrotiri, Cyprus", 100e3, 0, 0, 34.585, 32.987, false, "oblique sounder; rate and schedule from listener reports"},
        {"Chesapeake, Virginia, USA (ROTHR)", 100e3, 0, 0, 36.560, -76.300, false, "over-the-horizon radar sounder; site approximate"},
        {"Kingsville, Texas, USA (ROTHR)", 100e3, 0, 0, 27.500, -97.800, false, "over-the-horizon radar sounder; site approximate"},
        {"Juana Diaz, Puerto Rico (ROTHR)", 100e3, 0, 0, 18.020, -66.500, false, "over-the-horizon radar sounder; site approximate"},
    };
    return k;
}

double greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
    const double d2r = M_PI / 180;
    const double p1 = lat1 * d2r, p2 = lat2 * d2r, dp = p2 - p1, dl = (lon2 - lon1) * d2r;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kEarthKm * std::asin(std::min(1.0, std::sqrt(a)));
}

double obliqueGroupPathKm(double d, double h, int n) {
    const double R = kEarthKm, th = d / (2.0 * n * R);
    const double s = std::sqrt(R * R + (R + h) * (R + h) - 2 * R * (R + h) * std::cos(th));
    return 2.0 * n * s;
}

double mirrorHeightKm(double P, double d, int n) {
    const double R = kEarthKm, s = P / (2.0 * n), th = d / (2.0 * n * R), c = std::cos(th);
    const double disc = R * R * c * c - R * R + s * s;
    if (disc < 0) return NAN;
    return R * c + std::sqrt(disc) - R;
}

double obliqueDistanceKm(double P, double h, int n) {
    const double R = kEarthKm, s = P / (2.0 * n);
    if (s < h) return NAN;
    const double c = (R * R + (R + h) * (R + h) - s * s) / (2 * R * (R + h));
    return 2.0 * n * R * std::acos(std::clamp(c, -1.0, 1.0));
}

int identify(const IonoSounder& s, const Observer& obs, double hint, int& match, int& candidates, const std::vector<KnownSounder>& list) {
    match = 0; candidates = 0;
    int best = -1, ties = 0; double bestScore = -1e9; bool bestSched = false;
    for (size_t i = 0; i < list.size(); i++) {
        const KnownSounder& k = list[i];
        if (std::fabs(k.rateHz - s.rateHz) > 1) continue;
        candidates++;
        double score = 1; bool sched = false;
        if (k.periodSec > 0 && s.periodSec > 0) {
            if (std::fabs(k.periodSec - s.periodSec) < 0.5 && circDiff(k.offsetSec, s.offsetSec, k.periodSec) < 1.0) { score += 10; sched = true; }
            else if (k.verified) continue;
            else score -= 0.5;
        }
        if (obs.have && std::isfinite(hint)) {
            const double gc = greatCircleKm(obs.lat, obs.lon, k.lat, k.lon);
            score += std::fabs(gc - hint) < std::max(150.0, 0.25 * gc) ? 3 : -2;
        }
        if (score > bestScore + 1e-9) { bestScore = score; best = (int)i; ties = 1; bestSched = sched; }
        else if (std::fabs(score - bestScore) < 1e-9) ties++;
    }
    if (best < 0 || bestScore <= 0) return -1;
    if (bestSched) { match = 2; return best; }
    if (ties > 1) return -1;   // the rate alone does not tell them apart
    match = 1;
    return best;
}

// ---- traces

std::vector<IonoTrace> extractTraces(const std::vector<float>& db) {
    std::vector<IonoTrace> out;
    if (db.size() != (size_t)kRows * kCols) return out;
    // 1. peaks per column
    std::vector<std::vector<Peak>> cols(kCols);
    std::vector<float> v(kRows), s;
    for (int c = 0; c < kCols; c++) {
        int n = 0;
        for (int j = 0; j < kRows; j++) { v[(size_t)j] = db[(size_t)j * kCols + (size_t)c]; if (!std::isnan(v[(size_t)j])) n++; }
        if (n < kRows / 2) continue;
        s.clear();
        for (float x : v) if (!std::isnan(x)) s.push_back(x);
        std::nth_element(s.begin(), s.begin() + (long)(s.size() * 9 / 10), s.end());
        const double q90 = s[s.size() * 9 / 10];
        std::nth_element(s.begin(), s.begin() + (long)(s.size() / 2), s.end());
        const double med = s[s.size() / 2];
        const double thr = std::max(med + kPeakDb, q90 + kOverQ90);
        std::vector<Peak> pk;
        for (int j = 0; j < kRows; j++) {
            const float x = v[(size_t)j];
            if (std::isnan(x) || x < thr) continue;
            bool mx = true;
            for (int d = -2; d <= 2 && mx; d++) {
                const int k = j + d;
                if (d == 0 || k < 0 || k >= kRows || std::isnan(v[(size_t)k])) continue;
                if (v[(size_t)k] > x || (v[(size_t)k] == x && d < 0)) mx = false;
            }
            if (!mx) continue;
            double r = j;
            if (j > 0 && j + 1 < kRows && !std::isnan(v[(size_t)j - 1]) && !std::isnan(v[(size_t)j + 1])) {
                const double a = std::pow(10.0, v[(size_t)j - 1] / 10), b = std::pow(10.0, x / 10), e = std::pow(10.0, v[(size_t)j + 1] / 10);
                const double den = a - 2 * b + e;
                if (den < 0) r += std::clamp(0.5 * (a - e) / den, -0.5, 0.5);
            }
            pk.push_back({c, r, x});
        }
        std::sort(pk.begin(), pk.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
        if (pk.size() > (size_t)kMaxPeaks) pk.resize(kMaxPeaks);
        cols[(size_t)c] = std::move(pk);
    }
    // 2. linking, low to high frequency
    struct Tr { std::vector<Peak> p; double slope = 0; bool open = true; };
    std::vector<Tr> tr;
    for (int c = 0; c < kCols; c++) {
        for (auto& t : tr) if (t.open && c - t.p.back().col > kGap) t.open = false;
        auto& pk = cols[(size_t)c];
        if (pk.empty()) continue;
        struct Pair { double cost; size_t ti, pi; };
        std::vector<Pair> pairs;
        for (size_t ti = 0; ti < tr.size(); ti++) {
            if (!tr[ti].open) continue;
            const Peak& last = tr[ti].p.back();
            const int gap = c - last.col;
            const double pred = last.row + tr[ti].slope * gap;
            const double tol = 2.5 + 1.0 * gap + 0.6 * std::fabs(tr[ti].slope) * gap;
            for (size_t pi = 0; pi < pk.size(); pi++) {
                const double d = std::fabs(pk[pi].row - pred);
                if (d <= tol) pairs.push_back({d + 0.5 * (gap - 1), ti, pi});
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
        std::vector<char> tUsed(tr.size(), 0);
        for (const auto& q : pairs) {
            if (tUsed[q.ti] || pk[q.pi].used) continue;
            tUsed[q.ti] = 1; pk[q.pi].used = true;
            Tr& t = tr[q.ti];
            const Peak& last = t.p.back();
            const double sl = (pk[q.pi].row - last.row) / (c - last.col);
            t.slope = t.p.size() == 1 ? sl : 0.5 * t.slope + 0.5 * sl;
            t.p.push_back(pk[q.pi]);
        }
        for (auto& p : pk) if (!p.used) { Tr t; t.p.push_back(p); tr.push_back(std::move(t)); }
    }
    // 3. fragments (the steep end of a trace near a critical frequency breaks off): append to the trace they continue
    std::sort(tr.begin(), tr.end(), [](const Tr& a, const Tr& b) { return a.p.front().col < b.p.front().col; });
    for (size_t i = 0; i < tr.size(); i++) {
        Tr& f = tr[i];
        if (f.p.empty() || f.p.size() >= (size_t)kMinLen) continue;
        int best = -1; double bd = 1e300;
        for (size_t k = 0; k < tr.size(); k++) {
            if (k == i || tr[k].p.size() < (size_t)kMinLen) continue;
            const Peak& e = tr[k].p.back();
            const int gap = f.p.front().col - e.col;
            if (gap < 1 || gap > 2 || f.p.front().row < e.row - 2) continue;
            const double d = f.p.front().row - e.row;
            if (d < bd) { bd = d; best = (int)k; }
        }
        if (best >= 0) { for (auto& p : f.p) tr[(size_t)best].p.push_back(p); f.p.clear(); }
    }
    // 4. cusps: where a trace climbs to a critical frequency (E, F1) and the next layer's trace comes down from above, the linking runs
    // over the top; split there (a local maximum standing well over both sides)
    std::vector<Tr> split;
    for (auto& t : tr) {
        std::vector<Peak> p = std::move(t.p);
        while (p.size() >= 5) {
            size_t cut = 0;
            for (size_t i = 2; i + 1 < p.size() && !cut; i++) {
                if (p[i].row < p[i - 1].row || p[i].row <= p[i + 1].row) continue;
                double lo = p[i].row, ro = p[i].row;
                for (size_t k = 0; k < i; k++) lo = std::min(lo, p[k].row);
                for (size_t k = i + 1; k < p.size(); k++) ro = std::min(ro, p[k].row);
                if ((p[i].row - lo) * kRowKm >= 45 && (p[i].row - ro) * kRowKm >= 45 && (p[i].row - p[i - 2].row) * kRowKm >= 20) cut = i + 1;
            }
            if (!cut) break;
            Tr a; a.p.assign(p.begin(), p.begin() + (long)cut);
            split.push_back(std::move(a));
            p.erase(p.begin(), p.begin() + (long)cut);
        }
        Tr r; r.p = std::move(p);
        split.push_back(std::move(r));
    }
    for (const auto& t : split) {
        if (t.p.size() < (size_t)kMinLen || t.p.back().col - t.p.front().col < kMinLen - 1) continue;
        int smooth = 0;   // an echo trace is smooth most of the way (steep only close to a critical frequency); noise jumps about
        for (size_t i = 1; i < t.p.size(); i++) if (std::fabs(t.p[i].row - t.p[i - 1].row) <= 3.0 * (t.p[i].col - t.p[i - 1].col)) smooth++;
        if (smooth < 0.6 * (double)(t.p.size() - 1)) continue;
        IonoTrace o;
        for (const auto& p : t.p) o.pts.push_back({(float)colMhz(p.col), (float)(p.row * kRowKm), (float)p.db});
        out.push_back(std::move(o));
    }
    std::sort(out.begin(), out.end(), [](const IonoTrace& a, const IonoTrace& b) { return minRange(a) < minRange(b); });
    return out;
}

// ---- scaling

namespace {

// O and X of a layer: the trace reaching highest is X if an O trace ends 0.3 - 1.0 MHz (about fH / 2) below it over the same frequencies
int pickO(std::vector<IonoTrace>& tr, const std::vector<int>& L, int& x) {
    int top = L[0];
    for (int i : L) if (topMhz(tr[(size_t)i]) > topMhz(tr[(size_t)top])) top = i;
    int cand = -1;
    for (int i : L) {
        if (i == top) continue;
        const double d = topMhz(tr[(size_t)top]) - topMhz(tr[(size_t)i]);
        if (d < 0.3 || d > 1.0 || tr[(size_t)i].pts.size() < 0.3 * tr[(size_t)top].pts.size()) continue;
        if (lowMhz(tr[(size_t)top]) > topMhz(tr[(size_t)i])) continue;
        if (cand < 0 || topMhz(tr[(size_t)i]) > topMhz(tr[(size_t)cand])) cand = i;
    }
    x = -1;
    if (cand < 0) return top;
    tr[(size_t)top].x = true; x = top;
    return cand;
}

void scaleVertical(const std::vector<float>& db, std::vector<IonoTrace>& tr, IonoScaling& sc, bool locked) {
    std::vector<int> prim;
    for (size_t i = 0; i < tr.size(); i++) {
        if (locked && medRange(tr[i]) < kGroundKm) { tr[i].layer = IonoTrace::Ground; continue; }
        prim.push_back((int)i);
    }
    if (prim.empty()) return;
    sc.geometry = 1;
    sc.traces = (int)prim.size();
    if (!locked) {   // relative delays: only the end frequency of the highest trace and the lowest echo
        double top = 0, lo = 1e300; int ti = -1;
        for (int i : prim) { if (topMhz(tr[(size_t)i]) > top) { top = topMhz(tr[(size_t)i]); ti = i; } lo = std::min(lo, lowMhz(tr[(size_t)i])); }
        sc.foF2 = top + 0.025; sc.fmin = lo;
        sc.foF2Above = !sweptAfter(db, colOf(top));
        tr[(size_t)ti].layer = IonoTrace::F2;
        return;
    }
    // multiple reflections: twice (three times) the height of another trace
    for (int b : prim) {
        IonoTrace& B = tr[(size_t)b];
        for (int a : prim) {
            if (a == b || tr[(size_t)a].hops > 1) continue;
            const IonoTrace& A = tr[(size_t)a];
            for (int m = 2; m <= 3 && B.hops == 1; m++) {
                int common = 0, ok = 0;
                for (const auto& p : B.pts) {
                    const double ra = rangeAt(A, p.fMhz);
                    if (std::isnan(ra)) continue;
                    common++;
                    if (std::fabs(p.rangeKm - m * ra) < std::max(30.0, 0.06 * m * ra)) ok++;
                }
                if (common >= 4 && ok >= 0.6 * common && ok >= 0.4 * (double)B.pts.size()) B.hops = (uint8_t)m;
            }
        }
    }
    std::vector<int> E, F;
    for (int i : prim) {
        IonoTrace& t = tr[(size_t)i];
        if (t.hops > 1) continue;
        const double hmin = minRange(t) / 2, hmed = medRange(t) / 2;
        if (hmed >= 85 && hmed <= 160 && hmin >= 75) { t.layer = IonoTrace::E; E.push_back(i); }
        else if (hmin > 140) { t.layer = IonoTrace::F2; F.push_back(i); }
    }
    double fmin = 1e300;
    for (int i : E) fmin = std::min(fmin, lowMhz(tr[(size_t)i]));
    for (int i : F) fmin = std::min(fmin, lowMhz(tr[(size_t)i]));
    if (fmin < 1e300) sc.fmin = fmin;
    if (!E.empty()) {
        int xe;
        const int ti = pickO(tr, E, xe);
        double h = 1e300;
        for (int i : E) if (!tr[(size_t)i].x) h = std::min(h, minRange(tr[(size_t)i]) / 2);
        sc.foE = topMhz(tr[(size_t)ti]) + 0.025;
        sc.foEAbove = !sweptAfter(db, colOf(topMhz(tr[(size_t)ti])));
        sc.hE = h;
    }
    if (F.empty()) return;
    int xf;
    const int o = pickO(tr, F, xf);
    if (xf >= 0) sc.fxF2 = topMhz(tr[(size_t)xf]) + 0.025;
    IonoTrace& O = tr[(size_t)o];
    sc.foF2 = topMhz(O) + 0.025;
    sc.foF2Above = !sweptAfter(db, colOf(topMhz(O)));
    double hF = 1e300;
    for (int i : F) if (!tr[(size_t)i].x) hF = std::min(hF, minRange(tr[(size_t)i]) / 2);
    sc.hF = hF;
    // F1: an O trace of F ending well below foF2 where another F trace takes over, or a cusp inside the F2 trace
    // (the F1 candidates climb at their end, toward the cusp; their own O and X are told apart like F2's)
    double f1 = NAN;
    std::vector<int> F1;
    for (int i : F) {
        const IonoTrace& t = tr[(size_t)i];
        if (i == o || t.x || topMhz(t) > sc.foF2 - 0.4 || t.pts.back().rangeKm < minRange(t) + 30) continue;
        bool cont = false;
        for (int k : F) if (k != i && !tr[(size_t)k].x && lowMhz(tr[(size_t)k]) <= topMhz(t) + 0.4 && topMhz(tr[(size_t)k]) > topMhz(t) + 0.4) cont = true;
        if (cont) F1.push_back(i);
    }
    if (!F1.empty()) { int x1; f1 = topMhz(tr[(size_t)pickO(tr, F1, x1)]) + 0.025; }
    if (std::isnan(f1) && O.pts.size() >= 9) {
        // a cusp: h' rises at least 30 km to a local maximum and falls at least 30 km after it
        std::vector<double> h(O.pts.size());
        for (size_t i = 0; i < h.size(); i++) {
            double a = 0; int n = 0;
            for (size_t k = i > 0 ? i - 1 : 0; k <= std::min(h.size() - 1, i + 1); k++) { a += O.pts[k].rangeKm / 2; n++; }
            h[i] = a / n;
        }
        for (size_t i = 2; i + 2 < h.size(); i++) {
            if (O.pts[i].fMhz > sc.foF2 - 0.4 || h[i] < h[i - 1] || h[i] < h[i + 1]) continue;
            double before = h[i], after = h[i];
            for (size_t k = 0; k < i; k++) before = std::min(before, h[k]);
            for (size_t k = i + 1; k < h.size() && O.pts[k].fMhz < sc.foF2 - 0.2; k++) after = std::min(after, h[k]);
            if (h[i] - before >= 30 && h[i] - after >= 30) { f1 = O.pts[i].fMhz + 0.025; break; }
        }
    }
    if (!std::isnan(f1) && (std::isnan(sc.foE) || f1 > sc.foE + 0.3)) {
        sc.foF1 = f1;
        for (int i : F) if (!tr[(size_t)i].x && i != o && topMhz(tr[(size_t)i]) < f1 + 0.03) tr[(size_t)i].layer = IonoTrace::F1;
        double h2 = 1e300;
        for (const auto& p : O.pts) if (p.fMhz > f1) h2 = std::min(h2, (double)p.rangeKm / 2);
        if (h2 < 1e300) sc.hF2 = h2;
    } else sc.hF2 = minRange(O) / 2;
    if (!sc.foF2Above) {
        const double r = rangeInterp(O, 0.834 * sc.foF2);
        if (std::isfinite(r)) {
            sc.hmF2 = r / 2;
            sc.m3000 = 1490.0 / (sc.hmF2 + 176.0);
            sc.muf3000 = sc.foF2 * sc.m3000;
        }
    }
}

void scaleOblique(const std::vector<float>& db, std::vector<IonoTrace>& tr, IonoScaling& sc, bool locked) {
    if (tr.empty()) return;
    sc.geometry = 2;
    sc.traces = (int)tr.size();
    if (!locked) {
        double top = 0;
        for (const auto& t : tr) top = std::max(top, topMhz(t));
        sc.pathMuf = top + 0.025;
        sc.pathMufAbove = !sweptAfter(db, colOf(top));
        return;
    }
    // the high ray: a shorter trace ending at the same nose as a longer one, above it
    for (auto& b : tr) {
        for (const auto& a : tr) {
            if (&a == &b || a.highRay || a.pts.size() <= b.pts.size()) continue;
            if (std::fabs(topMhz(a) - topMhz(b)) > 0.25 || lowMhz(b) <= lowMhz(a)) continue;
            int above = 0, common = 0;
            for (const auto& p : b.pts) { const double ra = rangeAt(a, p.fMhz); if (std::isnan(ra)) continue; common++; if (p.rangeKm > ra) above++; }
            if (common >= 2 && above >= 0.8 * common) { b.highRay = true; break; }
        }
    }
    // the lowest trace: with the distance from the delay it is the 1-hop mode, E when flat (a thin layer: the group path within 3 % up to
    // 90 % of its end frequency), else F
    int low = -1;
    for (size_t i = 0; i < tr.size(); i++) if (!tr[i].highRay && (low < 0 || minRange(tr[i]) < minRange(tr[(size_t)low]))) low = (int)i;
    if (low < 0) return;
    if (std::isnan(sc.distKm)) {
        const IonoTrace& L = tr[(size_t)low];
        const double pmin = minRange(L), f90 = lowMhz(L) + 0.9 * (topMhz(L) - lowMhz(L));
        double p90 = pmin;
        for (const auto& p : L.pts) if (p.fMhz <= f90) p90 = std::max(p90, (double)p.rangeKm);
        const bool flat = p90 < 1.03 * pmin;
        sc.distKm = obliqueDistanceKm(pmin, flat ? kHe : kHf, 1);
        if (std::isfinite(sc.distKm)) { sc.distFrom = 1; tr[(size_t)low].layer = flat ? IonoTrace::E : IonoTrace::F2; tr[(size_t)low].hops = 1; }
    }
    if (!std::isfinite(sc.distKm)) return;
    // each trace (lowest group path first): the fewest hops whose mirror height (from the group path at the distance) is that of E
    // (85 - 150 km) or F (150 - 500 km); an F mode is ruled out for a trace that stays below the MUF of the E mode of as many hops
    // (the E layer turns those rays back)
    std::vector<size_t> order;
    for (size_t i = 0; i < tr.size(); i++) if (!tr[i].highRay) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return minRange(tr[a]) < minRange(tr[b]); });
    double mufE[4] = {0, 0, 0, 0};
    for (size_t i : order) {
        IonoTrace& t = tr[i];
        if (!t.layer) {
            for (int n = 1; n <= 3 && !t.layer; n++) {
                const double h = mirrorHeightKm(minRange(t), sc.distKm, n);
                if (!std::isfinite(h)) continue;
                if (h >= 85 && h < 150) { t.layer = IonoTrace::E; t.hops = (uint8_t)n; }
                else if (h >= 150 && h <= 500 && !(mufE[n] > 0 && topMhz(t) < mufE[n])) { t.layer = IonoTrace::F2; t.hops = (uint8_t)n; }
            }
        }
        if (t.layer == IonoTrace::E) mufE[t.hops] = std::max(mufE[t.hops], topMhz(t));
    }
    // a high ray belongs to the mode of the trace whose nose it shares
    for (auto& b : tr) {
        if (!b.highRay) continue;
        for (const auto& a : tr) if (!a.highRay && a.layer && std::fabs(topMhz(a) - topMhz(b)) <= 0.25) { b.layer = a.layer; b.hops = a.hops; break; }
    }
    for (const auto& t : tr) {
        if (!t.layer) continue;
        IonoMode* m = nullptr;
        for (auto& x : sc.modes) if (x.layer == t.layer && x.hops == t.hops) m = &x;
        if (!m) { sc.modes.push_back(IonoMode{}); m = &sc.modes.back(); m->layer = t.layer; m->hops = t.hops; m->mufMhz = 0; }
        const double top = topMhz(t) + 0.025;
        if (top > m->mufMhz) { m->mufMhz = top; m->mufAbove = !sweptAfter(db, colOf(topMhz(t))); }
        if (!t.highRay) m->minRangeKm = std::isnan(m->minRangeKm) ? minRange(t) : std::min(m->minRangeKm, minRange(t));
    }
    std::sort(sc.modes.begin(), sc.modes.end(), [](const IonoMode& a, const IonoMode& b) { return a.layer != b.layer ? a.layer < b.layer : a.hops < b.hops; });
    for (const auto& m : sc.modes)
        if (std::isnan(sc.pathMuf) || m.mufMhz > sc.pathMuf) { sc.pathMuf = m.mufMhz; sc.pathMufAbove = m.mufAbove; }
    double fmin = 1e300;
    for (const auto& t : tr) if (t.layer) fmin = std::min(fmin, lowMhz(t));
    if (fmin < 1e300) sc.fmin = fmin;
}

} // namespace

IonoScaling scaleIonogram(const std::vector<float>& db, const IonoSounder& s, const ScaleOptions& o, std::vector<IonoTrace>* traces,
                          const std::vector<KnownSounder>& list) {
    IonoScaling sc;
    sc.t = s.lastT0;
    std::vector<IonoTrace> tr = extractTraces(db);
    double pmin = 1e300; bool ground = false;
    for (const auto& t : tr) { if (o.locked && medRange(t) < kGroundKm) ground = true; else pmin = std::min(pmin, minRange(t)); }
    const double hint = !o.locked || pmin >= 1e300 ? NAN : pmin < 500 ? 0.0 : obliqueDistanceKm(pmin, kHint, 1);
    sc.known = identify(s, o.obs, hint, sc.knownMatch, sc.candidates, list);
    int geo = o.geometry;
    double knownDist = NAN;
    if (sc.known >= 0 && o.obs.have) knownDist = greatCircleKm(o.obs.lat, o.obs.lon, list[(size_t)sc.known].lat, list[(size_t)sc.known].lon);
    if (geo == 0) {
        if (std::isfinite(knownDist)) geo = knownDist < 150 ? 1 : 2;
        else if (!o.locked) geo = 2;
        else geo = ground || pmin < 500 ? 1 : 2;
    }
    if (pmin < 1e300 || ground) {
        if (geo == 1) scaleVertical(db, tr, sc, o.locked);
        else {
            if (std::isfinite(knownDist)) { sc.distKm = knownDist; sc.distFrom = 2; }
            std::vector<IonoTrace> sky;
            for (auto& t : tr) if (!o.locked || medRange(t) >= kGroundKm) sky.push_back(t);
            scaleOblique(db, sky, sc, o.locked);
            tr = std::move(sky);
        }
    }
    if (traces) *traces = std::move(tr);
    return sc;
}

// ---- bands

const std::vector<HfBand>& hfBands() {
    static const std::vector<HfBand> b = {
        {"160 m", 1.810, 2.000, false}, {"120 m", 2.300, 2.495, true}, {"90 m", 3.200, 3.400, true}, {"80 m", 3.500, 3.800, false},
        {"75 m", 3.900, 4.000, true}, {"60 m bc", 4.750, 5.060, true}, {"60 m", 5.3515, 5.3665, false}, {"49 m", 5.900, 6.200, true},
        {"40 m", 7.000, 7.200, false}, {"41 m", 7.200, 7.450, true}, {"31 m", 9.400, 9.900, true}, {"30 m", 10.100, 10.150, false},
        {"25 m", 11.600, 12.100, true}, {"22 m", 13.570, 13.870, true}, {"20 m", 14.000, 14.350, false}, {"19 m", 15.100, 15.800, true},
        {"16 m", 17.480, 17.900, true}, {"17 m", 18.068, 18.168, false}, {"15 m bc", 18.900, 19.020, true}, {"15 m", 21.000, 21.450, false},
        {"13 m", 21.450, 21.850, true}, {"12 m", 24.890, 24.990, false}, {"11 m", 25.670, 26.100, true}, {"10 m", 28.000, 29.700, false},
    };
    return b;
}

int bandState(const HfBand& b, double muf, double fmin) {
    if (!std::isfinite(muf)) return -2;
    if (b.loMhz > muf) return 0;
    if (std::isfinite(fmin) && b.hiMhz < fmin) return -1;
    return 1;
}

double scalingMuf(const IonoScaling& s) { return s.geometry == 2 ? s.pathMuf : s.geometry == 1 ? s.muf3000 : NAN; }

std::string historyCsv(const std::vector<IonoHistPt>& h, const IonoSounder& s, const char* knownName) {
    std::string out = "time_utc,sounder,rate_khz_s,known,geometry,foE_mhz,foF1_mhz,foF2_mhz,hE_km,hF_km,m3000F2,muf3000F2_mhz,path_muf_mhz,distance_km\n";
    auto num = [](float v, const char* f) { char b[32]; if (std::isnan(v)) return std::string(); snprintf(b, sizeof b, f, v); return std::string(b); };
    for (const auto& p : h) {
        char t[48];
        if (p.t > 1e8) {
            const std::time_t sec = (std::time_t)std::floor(p.t);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &sec);
#else
            gmtime_r(&sec, &tm);
#endif
            std::strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%SZ", &tm);
        } else snprintf(t, sizeof t, "%.1f", p.t);
        std::string name = knownName ? knownName : "";
        for (char& c : name) if (c == ',' || c == '"') c = ' ';
        char b[160];
        snprintf(b, sizeof b, "%s,%d,%.0f,%s,%s,", t, s.id, s.rateHz / 1e3, name.c_str(), p.geometry == 1 ? "vertical" : p.geometry == 2 ? "oblique" : "");
        out += b;
        out += num(p.foE, "%.2f") + "," + num(p.foF1, "%.2f") + "," + num(p.foF2, "%.2f") + "," + num(p.hE, "%.0f") + "," + num(p.hF, "%.0f") + "," +
               num(p.m3000, "%.3f") + "," + num(p.muf3000, "%.2f") + "," + num(p.pathMuf, "%.2f") + "," + num(p.distKm, "%.0f") + "\n";
    }
    return out;
}

} // namespace iono
} // namespace dect2
