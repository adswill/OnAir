#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2::ookscatter {

using Burst = std::vector<std::pair<float, float>>;

struct Pair { float on = 0, off = 0; int burst = 0; };

enum class Sel { Current, LastN, All };

struct Cluster {
    float onUs = 0, offUs = 0;
    int n = 0;
    float onJitter = 0, offJitter = 0;
};

struct Result {
    std::vector<Cluster> clusters;
    std::vector<int> label;
    std::string guess = "unknown";
    int points = 0;
};

constexpr size_t kMaxPoints = 3000;

inline std::vector<Pair> collect(const std::vector<Burst>& bursts, Sel sel, int n, int current) {
    std::vector<Pair> out;
    if (bursts.empty()) return out;
    int first = 0, last = (int)bursts.size();
    if (sel == Sel::Current) { if (current < 0 || current >= last) return out; first = current; last = current + 1; }
    else if (sel == Sel::LastN) first = std::max(0, last - std::max(1, n));
    for (int b = first; b < last; b++) {
        const Burst& s = bursts[(size_t)b];
        for (size_t i = 0; i + 1 < s.size(); i++) {
            if (!(s[i].first > 0) || !(s[i].second > 0)) continue;
            out.push_back({s[i].first, s[i].second, b});
        }
    }
    if (out.size() > kMaxPoints) {
        std::vector<Pair> d;
        d.reserve(kMaxPoints);
        const double step = (double)out.size() / (double)kMaxPoints;
        for (size_t i = 0; i < kMaxPoints; i++) d.push_back(out[(size_t)((double)i * step)]);
        out.swap(d);
    }
    return out;
}

inline std::string csv(const std::vector<Pair>& p) {
    std::string s = "burst,pulse,on_us,off_us\n";
    int prev = -1, k = 0;
    char b[96];
    for (const Pair& q : p) {
        if (q.burst != prev) { prev = q.burst; k = 0; }
        std::snprintf(b, sizeof b, "%d,%d,%.1f,%.1f\n", q.burst, k++, q.on, q.off);
        s += b;
    }
    return s;
}

inline float axis(float us, float lo, float hi, bool logScale) {
    if (!(hi > lo) || !(lo > 0)) return 0;
    const float v = std::max(us, lo);
    const float t = logScale ? std::log(v / lo) / std::log(hi / lo) : (v - lo) / (hi - lo);
    return std::min(1.f, std::max(0.f, t));
}

inline void range(const std::vector<Pair>& p, float& lo, float& hi) {
    lo = 1e9f; hi = 0;
    for (const Pair& q : p) { lo = std::min(lo, std::min(q.on, q.off)); hi = std::max(hi, std::max(q.on, q.off)); }
    if (p.empty()) { lo = 10; hi = 10000; return; }
    lo = std::max(1.f, lo * 0.7f);
    hi = std::max(hi * 1.4f, lo * 4);
}

namespace detail {

inline float median(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
    return v[v.size() / 2];
}

inline float spread(const std::vector<float>& v, float c) {
    if (v.size() < 2 || !(c > 0)) return 0;
    double s = 0;
    for (float x : v) s += (double)(x - c) * (double)(x - c);
    return (float)(std::sqrt(s / (double)v.size()) / (double)c);
}

inline std::vector<std::pair<float, int>> groups(std::vector<float> v) {
    std::vector<std::pair<float, int>> g;
    std::sort(v.begin(), v.end());
    for (float x : v) {
        if (!g.empty() && x < g.back().first * 1.4f) {
            g.back().first = (g.back().first * (float)g.back().second + x) / (float)(g.back().second + 1);
            g.back().second++;
        } else g.push_back({x, 1});
    }
    return g;
}

}

inline std::string guessCode(const std::vector<Cluster>& c) {
    if (c.empty() || c.size() > 6) return "unknown";
    std::vector<float> ons, offs;
    for (const Cluster& k : c) { ons.push_back(k.onUs); offs.push_back(k.offUs); }
    const auto go = detail::groups(ons), gf = detail::groups(offs);
    auto ratio = [](const std::vector<std::pair<float, int>>& g) { return g.size() == 2 ? g[1].first / g[0].first : 0.f; };
    const float ro = ratio(go), rf = ratio(gf);
    float pmin = 1e12f, pmax = 0;
    for (const Cluster& k : c) { pmin = std::min(pmin, k.onUs + k.offUs); pmax = std::max(pmax, k.onUs + k.offUs); }
    const bool fixedPeriod = pmax < 1.25f * pmin;
    if (go.size() == 2 && gf.size() == 2 && std::fabs(ro - 2.f) < 0.5f && std::fabs(rf - 2.f) < 0.5f && c.size() >= 3 && !fixedPeriod
        && std::fabs(go[0].first / gf[0].first - 1.f) < 0.4f) return "Manchester";
    if (go.size() == 2 && ro >= 1.6f && (gf.size() == 1 || fixedPeriod)) return "PWM";
    if (go.size() == 1 && gf.size() == 2 && rf >= 1.6f) return "PPM";
    return "unknown";
}

inline Result analyse(const std::vector<Pair>& p, float eps = 0.2f, int minPts = 4) {
    Result r;
    r.points = (int)p.size();
    const int n = (int)p.size();
    r.label.assign((size_t)n, -1);
    if (n < 8) return r;
    std::vector<float> lx((size_t)n), ly((size_t)n);
    for (int i = 0; i < n; i++) { lx[(size_t)i] = std::log(p[(size_t)i].on); ly[(size_t)i] = std::log(p[(size_t)i].off); }
    auto adj = [&](int i, int j) { return std::fabs(lx[(size_t)i] - lx[(size_t)j]) <= eps && std::fabs(ly[(size_t)i] - ly[(size_t)j]) <= eps; };
    std::vector<int> deg((size_t)n, 0);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if (i != j && adj(i, j)) deg[(size_t)i]++;
    std::vector<int> comp((size_t)n, -1);
    int nc = 0;
    for (int i = 0; i < n; i++) {
        if (comp[(size_t)i] >= 0 || deg[(size_t)i] < minPts) continue;
        std::vector<int> st{i};
        comp[(size_t)i] = nc;
        while (!st.empty()) {
            const int a = st.back(); st.pop_back();
            for (int j = 0; j < n; j++) {
                if (comp[(size_t)j] >= 0 || !adj(a, j)) continue;
                comp[(size_t)j] = nc;
                if (deg[(size_t)j] >= minPts) st.push_back(j);
            }
        }
        nc++;
    }
    const int minSize = std::max(6, n / 20);
    for (int k = 0; k < nc; k++) {
        std::vector<float> on, off;
        for (int i = 0; i < n; i++) if (comp[(size_t)i] == k) { on.push_back(p[(size_t)i].on); off.push_back(p[(size_t)i].off); }
        if ((int)on.size() < minSize) continue;
        Cluster c;
        c.n = (int)on.size();
        c.onUs = detail::median(on); c.offUs = detail::median(off);
        c.onJitter = detail::spread(on, c.onUs); c.offJitter = detail::spread(off, c.offUs);
        if (c.onJitter > 0.2f || c.offJitter > 0.2f) continue;
        const int id = (int)r.clusters.size();
        for (int i = 0; i < n; i++) if (comp[(size_t)i] == k) r.label[(size_t)i] = id;
        r.clusters.push_back(c);
    }
    std::vector<int> order(r.clusters.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return r.clusters[(size_t)a].onUs < r.clusters[(size_t)b].onUs; });
    std::vector<int> rank(order.size());
    std::vector<Cluster> sorted;
    for (size_t i = 0; i < order.size(); i++) { rank[(size_t)order[i]] = (int)i; sorted.push_back(r.clusters[(size_t)order[i]]); }
    for (int& l : r.label) if (l >= 0) l = rank[(size_t)l];
    r.clusters.swap(sorted);
    r.guess = guessCode(r.clusters);
    return r;
}

inline std::string clusterText(const Cluster& c) {
    char b[96];
    std::snprintf(b, sizeof b, "%.0f / %.0f us (%d, jitter %.0f%%)", c.onUs, c.offUs, c.n, 100.f * std::max(c.onJitter, c.offJitter));
    return b;
}

}
