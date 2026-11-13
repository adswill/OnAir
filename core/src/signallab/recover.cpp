// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 3b: symbol recovery. A matched filter for the family, a Gardner timing loop (cubic interpolation) at the
// estimated rate, then for PSK/QAM a carrier loop (x^M coarse lock + decision-directed second-order PLL), and a slicer.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>

namespace dect2::signallab {

namespace {
template <class T> T interp(const std::vector<T>& v, double t) {
    const long i = (long)std::floor(t);
    const float u = (float)(t - (double)i);
    auto at = [&](long k) { return v[(size_t)std::clamp(k, 0L, (long)v.size() - 1)]; };
    const T y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    // Catmull-Rom
    const T a = y1 * 2.f, b = y2 - y0, c = y0 * 2.f - y1 * 5.f + y2 * 4.f - y3, d = y1 * 3.f - y0 - y2 * 3.f + y3;
    return (a + b * u + c * (u * u) + d * (u * u * u)) * 0.5f;
}
float mag2(float v) { return v * v; }
float mag2(cf v) { return std::norm(v); }
float ted(float mid, float prev, float cur) { return mid * (prev - cur); }
float ted(cf mid, cf prev, cf cur) { return (std::conj(mid) * (prev - cur)).real(); }

template <class T> std::vector<T> gardner(const std::vector<T>& s, double sps, double bn, double* jitter) {
    std::vector<T> out;
    if (sps < 1.5 || s.size() < (size_t)(sps * 8)) return out;
    double pw = 0;
    for (const T& v : s) pw += mag2(v);
    pw /= (double)s.size();
    if (pw <= 0) return out;
    // initial phase: the one with the most energy at the symbol instants over the first symbols
    const int nPh = 16;
    double bestE = -1, ph0 = 0;
    const int nSym = (int)std::min(300.0, (double)s.size() / sps - 3);
    for (int k = 0; k < nPh; k++) {
        const double ph = sps * k / nPh;
        double e = 0;
        for (int j = 1; j < nSym; j++) e += mag2(interp(s, ph + j * sps));
        if (e > bestE) { bestE = e; ph0 = ph; }
    }
    const double zeta = 0.707, th = bn / (zeta + 0.25 / zeta), Kd = 3.0;
    const double k1 = 4 * zeta * th / (1 + 2 * zeta * th + th * th) / Kd, k2 = 4 * th * th / (1 + 2 * zeta * th + th * th) / Kd;
    double p = ph0 + sps, w = 0, ee = 0;
    size_t ne = 0;
    T prev = interp(s, ph0);
    out.push_back(prev);
    while (p + 3 < (double)s.size()) {
        const double T_ = sps * (1 + w);
        const T cur = interp(s, p), mid = interp(s, p - T_ / 2);
        double e = ted(mid, prev, cur) / pw;
        e = std::clamp(e, -1.0, 1.0);
        w = std::clamp(w + k2 * e, -0.05, 0.05);
        out.push_back(cur);
        prev = cur;
        ee += e * e; ne++;
        p += T_ + sps * k1 * e;
    }
    if (jitter) *jitter = ne ? std::sqrt(ee / (double)ne) / Kd : 0;
    return out;
}

std::vector<float> movingAverage(const std::vector<float>& s, int L) {
    if (L <= 1) return s;
    std::vector<float> o(s.size());
    double acc = 0;
    for (size_t i = 0; i < s.size(); i++) {
        acc += s[i];
        if (i >= (size_t)L) acc -= s[i - L];
        o[i] = (float)(acc / std::min<double>((double)i + 1, L));
    }
    return o;
}

double rrc(double t, double a) {   // t in symbols
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
    if (a > 0 && std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-6)
        return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
    return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

std::vector<cf> rrcFilter(const std::vector<cf>& x, double sps, double a) {
    const int half = (int)std::ceil(4 * sps);
    std::vector<float> h(2 * half + 1);
    double e = 0;
    for (int i = -half; i <= half; i++) { h[i + half] = (float)rrc(i / sps, a); e += (double)h[i + half] * h[i + half]; }
    for (auto& v : h) v = (float)(v / std::sqrt(e));
    std::vector<cf> y(x.size());
    for (size_t n = 0; n < x.size(); n++) {
        cf acc(0, 0);
        for (int k = -half; k <= half; k++) { const long j = (long)n - k; if (j >= 0 && j < (long)x.size()) acc += x[j] * h[k + half]; }
        y[n] = acc;
    }
    return y;
}

double refRot(int m, bool qam) { return qam ? M_PI / 4 : m == 4 ? M_PI / 4 : 0.0; }
cf decide(cf z, int m, bool qam, int* idx) {
    if (qam) {
        const int i = std::clamp((int)std::lround((z.real() + 3) / 2), 0, 3), q = std::clamp((int)std::lround((z.imag() + 3) / 2), 0, 3);
        if (idx) *idx = i * 4 + q;
        return cf((float)(2 * i - 3), (float)(2 * q - 3));
    }
    const double r = refRot(m, false), st = 2 * M_PI / m;
    int k = (int)std::lround((std::arg(z) - r) / st);
    k = ((k % m) + m) % m;
    if (idx) *idx = k;
    return std::polar(1.f, (float)(r + st * k));
}
int gray(int k) { return k ^ (k >> 1); }
}

std::vector<float> timingRecoverReal(const std::vector<float>& s, double sps, double bn, double* jitter) { return gardner(s, sps, bn, jitter); }
std::vector<cf> timingRecoverComplex(const std::vector<cf>& x, double sps, double bn, double* jitter) { return gardner(x, sps, bn, jitter); }

std::vector<cf> carrierRecover(const std::vector<cf>& sym, int m, bool qam, double* freqPerSym) {
    std::vector<cf> z = sym;
    if (z.empty()) return z;
    double pw = 0;
    for (const cf& v : z) pw += std::norm(v);
    pw /= (double)z.size();
    const float g = (float)(pw > 0 ? std::sqrt((qam ? 10.0 : 1.0) / pw) : 1.0);
    for (cf& v : z) v *= g;
    const int M = qam ? 4 : std::max(1, m);
    // coarse frequency from the x^M line of the symbols
    const size_t N = std::min<size_t>(65536, nextPow2(z.size()) * 4);
    std::vector<std::complex<double>> b(N, 0.0);
    for (size_t i = 0; i < z.size() && i < N; i++) { std::complex<double> p = 1, q = z[i]; for (int k = 0; k < M; k++) p *= q; b[i] = p / std::max(1e-12, std::pow(std::abs(q), M - 1)); }
    fft(b);
    size_t pk = 0;
    for (size_t i = 1; i < N; i++) if (std::norm(b[i]) > std::norm(b[pk])) pk = i;
    double fb = (double)pk;
    if (fb >= (double)N / 2) fb -= (double)N;
    const double f0 = fb / (double)N / M;   // cycles per symbol
    // initial phase from the first symbols
    std::complex<double> acc = 0;
    for (size_t i = 0; i < std::min<size_t>(z.size(), 64); i++) {
        std::complex<double> q = std::complex<double>(z[i]) * std::polar(1.0, -2 * M_PI * f0 * (double)i), p = 1;
        for (int k = 0; k < M; k++) p *= q;
        acc += p / std::max(1e-12, std::pow(std::abs(q), M - 1));
    }
    double ph = (std::arg(acc) - M * refRot(m, qam)) / M, w = 2 * M_PI * f0;
    const double al = 0.08, be = 0.0016;
    std::vector<cf> o(z.size());
    for (size_t i = 0; i < z.size(); i++) {
        const cf r = z[i] * std::polar(1.f, (float)-ph);
        const cf d = decide(r, m, qam, nullptr);
        const double e = std::arg(r * std::conj(d));
        o[i] = r;
        w += be * e;
        ph += w + al * e;
    }
    if (freqPerSym) *freqPerSym = w / (2 * M_PI);
    return o;
}

Symbols recoverSymbols(const std::vector<cf>& x, double fs, double baud, Mod m, int order) {
    Symbols s;
    s.order = std::max(2, order);
    if (baud <= 0 || fs / baud < 1.5 || x.size() < 32) return s;
    const double sps = fs / baud;
    if (m == Mod::PSK || m == Mod::QAM) {
        const bool qam = m == Mod::QAM;
        if (qam) s.order = 16;
        const std::vector<cf> y = rrcFilter(x, sps, 0.35);
        const std::vector<cf> t = timingRecoverComplex(y, sps, 0.01, &s.timingJitter);
        s.points = carrierRecover(t, s.order, qam, &s.residualHz);
        s.residualHz *= baud;
        double ee = 0, rr = 0;
        for (size_t i = 0; i < s.points.size(); i++) {
            int k = 0;
            const cf d = decide(s.points[i], s.order, qam, &k);
            s.sym.push_back(k);
            if (i >= s.points.size() / 10) { ee += std::norm(s.points[i] - d); rr += std::norm(d); }   // after the loops settle
        }
        s.evm = rr > 0 ? std::sqrt(ee / rr) : 0;
        return s;
    }
    std::vector<float> v = symbolSignal(x, fs, m);
    v = movingAverage(v, std::max(1, (int)std::lround(sps * (m == Mod::OOK || m == Mod::ASK ? 0.8 : 0.7))));
    const std::vector<float> c2 = kmeans1d(v, 2);
    const float mid = 0.5f * (c2[0] + c2[1]);
    for (auto& q : v) q -= mid;
    s.soft = timingRecoverReal(v, sps, 0.01, &s.timingJitter);
    s.levels = kmeans1d(s.soft, s.order);
    s.residualHz = mid;
    double ee = 0, rr = 0;
    const float span = s.levels.back() - s.levels.front();
    for (float q : s.soft) {
        int k = 0;
        for (int i = 1; i < s.order; i++) if (std::fabs(q - s.levels[i]) < std::fabs(q - s.levels[k])) k = i;
        s.sym.push_back(k);
        ee += (double)(q - s.levels[k]) * (q - s.levels[k]);
    }
    rr = (double)span * span / 4 * (double)s.soft.size();
    s.evm = rr > 0 ? std::sqrt(ee / rr) : 0;
    return s;
}

Bits symbolsToBits(const Symbols& s, Mod m, bool g) {
    Bits b;
    if (m == Mod::QAM) {
        for (int k : s.sym) {
            const int i = gray(k / 4), q = gray(k % 4);
            b.push_back((uint8_t)((i >> 1) & 1)); b.push_back((uint8_t)(i & 1));
            b.push_back((uint8_t)((q >> 1) & 1)); b.push_back((uint8_t)(q & 1));
        }
        return b;
    }
    int nb = 0;
    while ((1 << nb) < s.order) nb++;
    nb = std::max(1, nb);
    for (int k : s.sym) {
        const int v = (g || m == Mod::PSK) ? gray(k) : k;
        for (int i = nb - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1));
    }
    return b;
}

} // namespace dect2::signallab
