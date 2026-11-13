// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/astro/astro_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace astro {

const Line* lines(int& count) {
    static const Line t[] = {
        {"H I 1420.4058 MHz", kHiHz},
        {"OH 1612.231 MHz", 1612.231e6},
        {"OH 1665.402 MHz", 1665.402e6},
        {"OH 1667.359 MHz", 1667.359e6},
        {"OH 1720.530 MHz", 1720.530e6},
    };
    count = (int)(sizeof t / sizeof t[0]);
    return t;
}

void removeDc(std::vector<double>& p, int half) {
    const int n = (int)p.size(), c = n / 2;
    if (half < 0 || n < 2 * half + 3) return;
    const int a = c - half - 1, b = c + half + 1;
    for (int i = a + 1; i < b; i++) p[(size_t)i] = p[(size_t)a] + (p[(size_t)b] - p[(size_t)a]) * (i - a) / (double)(b - a);
}

std::vector<double> polyBaseline(const std::vector<double>& y, const std::vector<bool>& use, int degree) {
    const int n = (int)y.size();
    std::vector<double> out((size_t)n, 0.0);
    if (n == 0 || degree < 0) return out;
    const int m = std::min(degree, 9) + 1;
    // normal equations on Legendre-like scaled x (powers of x in -1 .. 1 are well enough conditioned up to degree 9 in double)
    std::vector<double> A((size_t)m * m, 0.0), B((size_t)m, 0.0), pw((size_t)m);
    int used = 0;
    for (int i = 0; i < n; i++) {
        if (!use[(size_t)i] || !std::isfinite(y[(size_t)i])) continue;
        const double x = n > 1 ? 2.0 * i / (n - 1) - 1 : 0;
        pw[0] = 1;
        for (int k = 1; k < m; k++) pw[(size_t)k] = pw[(size_t)k - 1] * x;
        for (int r = 0; r < m; r++) {
            B[(size_t)r] += pw[(size_t)r] * y[(size_t)i];
            for (int c = 0; c < m; c++) A[(size_t)r * m + c] += pw[(size_t)r] * pw[(size_t)c];
        }
        used++;
    }
    if (used < m) return out;
    // Gaussian elimination with partial pivoting
    std::vector<double> c((size_t)m, 0.0);
    for (int col = 0; col < m; col++) {
        int piv = col;
        for (int r = col + 1; r < m; r++) if (std::fabs(A[(size_t)r * m + col]) > std::fabs(A[(size_t)piv * m + col])) piv = r;
        if (std::fabs(A[(size_t)piv * m + col]) < 1e-300) return out;
        if (piv != col) { for (int k = 0; k < m; k++) std::swap(A[(size_t)col * m + k], A[(size_t)piv * m + k]); std::swap(B[(size_t)col], B[(size_t)piv]); }
        for (int r = col + 1; r < m; r++) {
            const double f = A[(size_t)r * m + col] / A[(size_t)col * m + col];
            for (int k = col; k < m; k++) A[(size_t)r * m + k] -= f * A[(size_t)col * m + k];
            B[(size_t)r] -= f * B[(size_t)col];
        }
    }
    for (int r = m - 1; r >= 0; r--) {
        double s = B[(size_t)r];
        for (int k = r + 1; k < m; k++) s -= A[(size_t)r * m + k] * c[(size_t)k];
        c[(size_t)r] = s / A[(size_t)r * m + r];
    }
    for (int i = 0; i < n; i++) {
        const double x = n > 1 ? 2.0 * i / (n - 1) - 1 : 0;
        double v = 0;
        for (int k = m - 1; k >= 0; k--) v = v * x + c[(size_t)k];
        out[(size_t)i] = v;
    }
    return out;
}

std::vector<double> smooth(const std::vector<double>& y, int w) {
    if (w <= 1 || y.empty()) return y;
    const int n = (int)y.size(), h = w / 2;
    std::vector<double> cs((size_t)n + 1, 0.0), out((size_t)n);
    for (int i = 0; i < n; i++) cs[(size_t)i + 1] = cs[(size_t)i] + y[(size_t)i];
    for (int i = 0; i < n; i++) {
        const int a = std::max(0, i - h), b = std::min(n, i - h + w);
        out[(size_t)i] = (cs[(size_t)b] - cs[(size_t)a]) / std::max(1, b - a);
    }
    return out;
}

static double median(std::vector<double> v) {
    if (v.empty()) return 1;
    std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
    return v[v.size() / 2];
}

Result process(const std::vector<double>& onIn, const std::vector<double>& offIn, const Params& p) {
    Result r;
    const int n = (int)onIn.size();
    if (n < 16 || p.rate <= 0) return r;
    const double bin = p.rate / n;
    std::vector<double> on(onIn), off(offIn.size() == onIn.size() ? offIn : std::vector<double>());
    removeDc(on, p.dcHalf);
    if (!off.empty()) removeDc(off, p.dcHalf);
    r.freqHz.resize((size_t)n); r.velKms.resize((size_t)n); r.inWindow.assign((size_t)n, false);
    for (int i = 0; i < n; i++) {
        r.freqHz[(size_t)i] = p.centerHz + (i - n / 2) * bin;
        r.velKms[(size_t)i] = velocityKms(r.freqHz[(size_t)i], p.restHz);
        const double v = r.velKms[(size_t)i];
        r.inWindow[(size_t)i] = p.window && v >= std::min(p.winLoKms, p.winHiKms) && v <= std::max(p.winLoKms, p.winHiKms);
    }
    // (on - off) / off, or relative to the median level without a reference
    std::vector<double> y((size_t)n);
    if (!off.empty()) {
        for (int i = 0; i < n; i++) y[(size_t)i] = off[(size_t)i] > 0 ? (on[(size_t)i] - off[(size_t)i]) / off[(size_t)i] : 0;
    } else {
        const double md = std::max(1e-30, median(on));
        for (int i = 0; i < n; i++) y[(size_t)i] = on[(size_t)i] / md - 1;
    }
    const int e = std::clamp((int)(p.edge * n), 0, n / 2 - 4);
    const int sh = !off.empty() && p.switchHz != 0 ? (int)std::lround(p.switchHz / bin) : 0;
    std::vector<bool> use((size_t)n, false);
    for (int i = e; i < n - e; i++) {
        bool u = !r.inWindow[(size_t)i];
        if (sh && i + sh >= 0 && i + sh < n && r.inWindow[(size_t)(i + sh)]) u = false;   // the negative image of the window
        use[(size_t)i] = u;
    }
    if (p.dcHalf >= 0) for (int i = n / 2 - p.dcHalf - 1; i <= n / 2 + p.dcHalf + 1; i++) if (i >= 0 && i < n) use[(size_t)i] = false;
    r.baseline = p.degree >= 0 ? polyBaseline(y, use, p.degree) : std::vector<double>((size_t)n, 0.0);
    for (int i = 0; i < n; i++) y[(size_t)i] -= r.baseline[(size_t)i];
    if (sh && p.fold) {   // the line is positive at b and negative at b - sh: average the two
        std::vector<double> f(y);
        for (int i = 0; i < n; i++) if (i - sh >= 0 && i - sh < n) f[(size_t)i] = (y[(size_t)i] - y[(size_t)(i - sh)]) / 2;
        y.swap(f);
    }
    r.ratio = smooth(y, p.smoothBins);
    // noise (outside the window) and the peak (inside it, or anywhere off the edges)
    double s2 = 0; int ns = 0;
    for (int i = e; i < n - e; i++) if (use[(size_t)i]) { s2 += r.ratio[(size_t)i] * r.ratio[(size_t)i]; ns++; }
    r.rms = ns ? std::sqrt(s2 / ns) : 0;
    int pk = -1;
    for (int i = std::max(e, 1); i < n - std::max(e, 1); i++) {
        if (p.window && !r.inWindow[(size_t)i]) continue;
        if (pk < 0 || r.ratio[(size_t)i] > r.ratio[(size_t)pk]) pk = i;
    }
    if (pk > 0 && pk < n - 1) {
        const double a = r.ratio[(size_t)pk - 1], b = r.ratio[(size_t)pk], c = r.ratio[(size_t)pk + 1], d = a - 2 * b + c;
        double pos = pk + (d < 0 ? std::clamp(0.5 * (a - c) / d, -0.5, 0.5) : 0);
        // the line's centre: centroid of the part above half the peak (steadier than the top of a noisy hump)
        int lo = pk, hi = pk;
        while (lo > e && r.ratio[(size_t)lo - 1] > 0.5 * b) lo--;
        while (hi < n - 1 - e && r.ratio[(size_t)hi + 1] > 0.5 * b) hi++;
        double sw = 0, sx = 0;
        for (int i = lo; i <= hi; i++) { const double w = r.ratio[(size_t)i] - 0.5 * b; sw += w; sx += w * i; }
        if (b > 0 && sw > 0 && hi - lo >= 2) pos = sx / sw;
        r.peak = pk;
        r.peakHz = p.centerHz + (pos - n / 2) * bin;
        r.peakKms = velocityKms(r.peakHz, p.restHz);
        r.peakRatio = b;
        r.snr = r.rms > 0 ? b / r.rms : 0;
    }
    return r;
}

std::string toCsv(const Result& r, const std::vector<double>& on, const Params& p, double integrationSec) {
    std::string s;
    char b[256];
    snprintf(b, sizeof b, "# DecT2 radio astronomy spectrum\n# center_hz=%.3f\n# rest_hz=%.3f\n# rate_hz=%.3f\n# bins=%zu\n# integration_s=%.3f\n",
             p.centerHz, p.restHz, p.rate, on.size(), integrationSec);
    s += b;
    s += "# velocity: radio convention, topocentric (no LSR correction)\nfreq_hz,velocity_kms,power_db,ratio,baseline\n";
    for (size_t i = 0; i < on.size() && i < r.freqHz.size(); i++) {
        snprintf(b, sizeof b, "%.3f,%.4f,%.5f,%.8g,%.8g\n", r.freqHz[i], r.velKms[i], 10 * std::log10(std::max(1e-30, on[i])), r.ratio[i], r.baseline[i]);
        s += b;
    }
    return s;
}

bool fromCsv(const std::string& text, CsvSpectrum& out) {
    out = CsvSpectrum();
    std::istringstream in(text);
    std::string l;
    bool header = false;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty()) continue;
        if (l[0] == '#') {
            auto val = [&](const char* k, double& v) { const std::string key = std::string("# ") + k + "="; if (l.rfind(key, 0) == 0) v = std::atof(l.c_str() + key.size()); };
            val("center_hz", out.centerHz); val("rest_hz", out.restHz); val("rate_hz", out.rate); val("integration_s", out.integrationSec);
            continue;
        }
        if (!header) { header = l.rfind("freq_hz", 0) == 0; if (header) continue; return false; }
        double v[5];
        if (sscanf(l.c_str(), "%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5) return false;
        out.freqHz.push_back(v[0]); out.velKms.push_back(v[1]); out.powerDb.push_back(v[2]); out.ratio.push_back(v[3]); out.baseline.push_back(v[4]);
    }
    return header && !out.freqHz.empty();
}

} // namespace astro
} // namespace dect2
