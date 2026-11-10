// DAB single-frequency network view: the paths in the channel impulse response, and which transmitter (from TII and the list of positions) each one probably is, from the differences
// of distance from the receiver. Transmitters of one network may have static delays that are not known here: a path that fits nothing is shown as such.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

constexpr double kDabGuardUs = 246.0, kDabCirUsPerBin = 1.0 / 2.048;

struct DabPath { double delayUs = 0; float levelDb = 0; };    // relative to the strongest path (negative: arrives before it)
struct DabSfnTx { int mainId = 0, subId = 0; double km = -1, bearingDeg = 0; float tiiDb = 0; };
struct DabSfnRow { DabSfnTx tx; bool matched = false; DabPath path; double expectedUs = 0; };
struct DabSfnResult { std::vector<DabSfnRow> rows; std::vector<DabPath> paths; int refTx = -1; bool outsideGuard = false; };

// paths of a cyclic impulse response (magnitude, 2048 bins of 1 / 2.048 MHz), strongest first
inline std::vector<DabPath> dabCirPaths(const std::vector<float>& cir, float minDb = -18) {
    std::vector<DabPath> out;
    const int n = (int)cir.size();
    if (n < 16) return out;
    int pk = 0; for (int i = 1; i < n; i++) if (cir[(size_t)i] > cir[(size_t)pk]) pk = i;
    const float top = std::max(1e-9f, cir[(size_t)pk]);
    for (int i = 0; i < n; i++) {
        const float v = cir[(size_t)i], l = cir[(size_t)((i + n - 1) % n)], r = cir[(size_t)((i + 1) % n)];
        if (v < l || v <= r) continue;
        const float db = 20 * std::log10(std::max(1e-9f, v / top));
        if (db < minDb) continue;
        int d = i - pk; if (d > n / 2) d -= n; if (d < -n / 2) d += n;
        out.push_back({d * kDabCirUsPerBin * (2048.0 / n), db});
    }
    std::sort(out.begin(), out.end(), [](const DabPath& a, const DabPath& b) { return a.levelDb > b.levelDb; });
    return out;
}

// Each located transmitter is tried as the one on the strongest path; the assignment that explains the most paths (within tolUs, then the smallest error) wins.
inline DabSfnResult dabSfnSolve(const std::vector<DabPath>& paths, const std::vector<DabSfnTx>& txs, double tolUs = 3.0) {
    DabSfnResult r; r.paths = paths;
    for (const auto& p : paths) if (std::fabs(p.delayUs) > kDabGuardUs) r.outsideGuard = true;
    int bestN = -1; double bestErr = 1e18;
    std::vector<DabSfnRow> best;
    for (size_t ref = 0; ref < txs.size(); ref++) {
        if (txs[ref].km < 0) continue;
        std::vector<DabSfnRow> rows; int n = 0; double err = 0;
        std::vector<char> used(paths.size(), 0);
        for (const auto& t : txs) {
            DabSfnRow row; row.tx = t;
            if (t.km >= 0) {
                row.expectedUs = (t.km - txs[ref].km) / 0.2998;
                int at = -1; double e = tolUs;
                for (size_t i = 0; i < paths.size(); i++) if (!used[i] && std::fabs(paths[i].delayUs - row.expectedUs) <= e) { e = std::fabs(paths[i].delayUs - row.expectedUs); at = (int)i; }
                if (at >= 0) { used[(size_t)at] = 1; row.matched = true; row.path = paths[(size_t)at]; n++; err += e; }
            }
            rows.push_back(row);
        }
        if (n > bestN || (n == bestN && err < bestErr)) { bestN = n; bestErr = err; best = rows; r.refTx = (int)ref; }
    }
    if (bestN < 0) for (const auto& t : txs) { DabSfnRow q; q.tx = t; best.push_back(q); }
    r.rows = best;
    return r;
}

} // namespace dect2
