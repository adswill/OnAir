// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/wb/wb_demod.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace wb {

namespace {
using dect2::math::kPi;

// FIR filter of a complex signal, the same length, the group delay taken out (zeros outside)
std::vector<cf32> firFilter(const std::vector<cf32>& x, const std::vector<float>& h) {
    const size_t n = x.size(), N = h.size();
    std::vector<float> re(n + N - 1, 0.f), im(n + N - 1, 0.f), orr(n), oi(n);
    const size_t d = (N - 1) / 2;
    for (size_t i = 0; i < n; i++) { re[i + d] = x[i].real(); im[i + d] = x[i].imag(); }
    desamp(re.data(), 1, h.data(), orr.data(), (int)n, (int)N);
    desamp(im.data(), 1, h.data(), oi.data(), (int)n, (int)N);
    std::vector<cf32> o(n);
    for (size_t i = 0; i < n; i++) o[i] = cf32(orr[i], oi[i]);
    return o;
}

std::vector<float> firReal(const std::vector<float>& x, const std::vector<float>& h) {
    const size_t n = x.size(), N = h.size();
    std::vector<float> re(n + N - 1, 0.f), o(n);
    const size_t d = (N - 1) / 2;
    std::copy(x.begin(), x.end(), re.begin() + (long)d);
    desamp(re.data(), 1, h.data(), o.data(), (int)n, (int)N);
    return o;
}

std::vector<float> rrc(double alpha, double sps, int span) {
    const int n = std::max(3, (int)(span * sps)) | 1;
    std::vector<float> h((size_t)n);
    const double c = (n - 1) / 2.0;
    double e = 0;
    for (int i = 0; i < n; i++) {
        const double t = (i - c) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - alpha + 4 * alpha / kPi;
        else if (alpha > 0 && std::fabs(std::fabs(t) - 1 / (4 * alpha)) < 1e-6)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
        else {
            const double num = std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha));
            const double den = kPi * t * (1 - (4 * alpha * t) * (4 * alpha * t));
            v = num / den;
        }
        h[(size_t)i] = (float)v;
        e += v * v;
    }
    for (float& v : h) v = (float)(v / std::sqrt(e));
    return h;
}

std::vector<float> boxcar(double sps) {
    const int n = std::max(1, (int)std::lround(sps));
    return std::vector<float>((size_t)n, 1.f / (float)n);
}

// cubic (4-point Lagrange) interpolation of z at fractional index t, clamped at the ends
cf32 interp(const std::vector<cf32>& z, double t) {
    const long n = (long)z.size();
    long i = (long)std::floor(t);
    const float mu = (float)(t - (double)i);
    auto g = [&](long k) { return z[(size_t)std::min(std::max(k, 0L), n - 1)]; };
    const cf32 a = g(i - 1), b = g(i), c = g(i + 1), d = g(i + 2);
    const float c0 = -mu * (mu - 1) * (mu - 2) / 6, c1 = (mu + 1) * (mu - 1) * (mu - 2) / 2, c2 = -(mu + 1) * mu * (mu - 2) / 2, c3 = (mu + 1) * mu * (mu - 1) / 6;
    return a * c0 + b * c1 + c * c2 + d * c3;
}

double meanPower(const std::vector<cf32>& z) {
    double s = 0;
    for (const cf32& v : z) s += (double)std::norm(v);
    return s / (double)std::max<size_t>(1, z.size());
}

struct Timing {
    std::vector<double> t;      // sample position of each symbol
    std::vector<cf32> s;        // the sample there
    double sps = 0;             // the mean spacing (follows the loop)
};

Timing recoverTiming(const std::vector<cf32>& z, double sps, int clock, double loopBw) {
    Timing r;
    r.sps = sps;
    const long n = (long)z.size();
    if (n < (long)(8 * sps)) return r;
    // the best sampling phase from the first symbols: the instants where the energy peaks
    double bestM = -1, bestPhi = 0;
    const int nph = 32;
    const long maxSyms = std::min<long>(4000, (long)((double)n / sps) - 2);
    for (int p = 0; p < nph; p++) {
        const double phi = sps * p / nph;
        double m = 0;
        for (long k = 1; k < maxSyms; k++) m += (double)std::norm(interp(z, phi + (double)k * sps));
        if (m > bestM) { bestM = m; bestPhi = phi; }
    }
    const double pw = std::max(meanPower(z), 1e-20);
    double t = bestPhi + sps;
    double integ = 0;
    const double zeta = 0.707, g = std::max(0.0005, loopBw);
    const double kp = 4 * zeta * g, ki = 4 * g * g;
    cf32 prev = interp(z, t - sps);
    double step = sps;
    while (t < (double)n - 2) {
        const cf32 cur = interp(z, t);
        if (clock == 1) {
            const cf32 mid = interp(z, t - sps / 2);
            double e = std::real((cur - prev) * std::conj(mid)) / pw;
            e = std::max(-1.0, std::min(1.0, e));
            integ += ki * e;
            integ = std::max(-0.05, std::min(0.05, integ));
            step = sps * (1.0 - (kp * e + integ));
            step = std::max(sps * 0.9, std::min(sps * 1.1, step));
        }
        r.t.push_back(t); r.s.push_back(cur);
        prev = cur;
        t += step;
    }
    if (!r.t.empty() && r.t.size() > 1) r.sps = (r.t.back() - r.t.front()) / (double)(r.t.size() - 1);
    return r;
}

void percentileCentres(std::vector<float> v, int k, std::vector<float>& c) {
    std::sort(v.begin(), v.end());
    c.assign((size_t)k, 0.f);
    if (v.empty()) return;
    // start: spread over the 5 .. 95 % points, then Lloyd iterations
    for (int i = 0; i < k; i++) c[(size_t)i] = v[(size_t)((0.05 + 0.9 * (k == 1 ? 0.5 : (double)i / (k - 1))) * (double)(v.size() - 1))];
    for (int it = 0; it < 25; it++) {
        std::vector<double> sum((size_t)k, 0.0);
        std::vector<size_t> cnt((size_t)k, 0);
        for (float x : v) {
            int b = 0;
            for (int i = 1; i < k; i++) if (std::fabs(x - c[(size_t)i]) < std::fabs(x - c[(size_t)b])) b = i;
            sum[(size_t)b] += x; cnt[(size_t)b]++;
        }
        for (int i = 0; i < k; i++) if (cnt[(size_t)i]) c[(size_t)i] = (float)(sum[(size_t)i] / (double)cnt[(size_t)i]);
        std::sort(c.begin(), c.end());
    }
}

int nearest(const std::vector<float>& c, float x) {
    int b = 0;
    for (size_t i = 1; i < c.size(); i++) if (std::fabs(x - c[i]) < std::fabs(x - c[(size_t)b])) b = (int)i;
    return b;
}

void decimatePlots(DemodResult& r, const cf32* x, size_t n, double rate) {
    const size_t maxPts = 16384;
    const size_t step = std::max<size_t>(1, (n + maxPts - 1) / maxPts);
    r.plotRate = rate / (double)step;
    double ph = 0;
    cf32 prev = n ? x[0] : cf32(0, 0);
    double fAcc = 0, mAcc = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        const double d = std::arg(x[i] * std::conj(prev));
        prev = x[i];
        ph += d;
        fAcc += d * rate / (2 * kPi);
        mAcc += (double)std::abs(x[i]);
        if (++cnt == step) {
            r.instFreq.push_back((float)(fAcc / (double)cnt));
            r.instPhase.push_back((float)ph);
            r.magnitude.push_back((float)(mAcc / (double)cnt));
            fAcc = mAcc = 0; cnt = 0;
        }
    }
}

void toAudio(DemodResult& r, std::vector<float> a, double rate, int audioRate) {
    // remove the mean, scale to a peak of 0.9 (the 99.9 % point, so one spike does not make it quiet), resample
    if (a.empty()) return;
    double m = 0;
    for (float v : a) m += v;
    m /= (double)a.size();
    for (float& v : a) v = (float)(v - m);
    std::vector<float> ab(a.size());
    for (size_t i = 0; i < a.size(); i++) ab[i] = std::fabs(a[i]);
    const size_t k = std::min(ab.size() - 1, (size_t)((double)ab.size() * 0.999));
    std::nth_element(ab.begin(), ab.begin() + (long)k, ab.end());
    const float pk = std::max(ab[k], 1e-12f);
    for (float& v : a) v = std::max(-1.f, std::min(1.f, v * 0.9f / pk));
    r.audio = resampleReal(a, rate, audioRate);
}

} // namespace

const char* kindName(int k) {
    static const char* n[KindCount] = {"AM", "FM / NFM", "USB", "LSB", "CW", "ASK / OOK", "2-FSK", "4-FSK", "AFSK", "BPSK", "QPSK", "8PSK", "MSK", "GMSK"};
    return k >= 0 && k < KindCount ? n[k] : "?";
}
bool kindHasBits(int k) { return k >= KAsk; }
bool kindIsPsk(int k) { return k == KBpsk || k == KQpsk || k == KPsk8; }

double snapBaud(double b) {
    static const double std_[] = {45.45, 50, 75, 100, 110, 150, 200, 300, 600, 1200, 1800, 2400, 3600, 4800, 7200, 9600, 14400, 19200, 28800, 38400, 57600, 76800, 115200, 230400, 250000, 500000, 1000000};
    double best = b;
    for (double s : std_) if (std::fabs(b - s) < 0.02 * s && (best == b || std::fabs(b - s) < std::fabs(b - best))) best = s;
    return best;
}

DemodResult demodulateBaseband(const cf32* x, size_t n, double rate, const DemodParams& p) {
    DemodResult r;
    r.rate = rate;
    r.baud = p.baud;
    if (n < 64 || rate <= 0) { r.msg = "the selection is too short"; return r; }
    decimatePlots(r, x, n, rate);
    const int k = p.kind;
    // ---- audio demodulators -----------------------------------------------------------------------------------------------------
    if (k == KAm || k == KFm || k == KUsb || k == KLsb || k == KCw) {
        std::vector<float> a(n);
        if (k == KAm) for (size_t i = 0; i < n; i++) a[i] = std::abs(x[i]);
        else if (k == KFm) { cf32 prev = x[0]; for (size_t i = 0; i < n; i++) { a[i] = (float)(std::arg(x[i] * std::conj(prev)) * rate / (2 * kPi)); prev = x[i]; } }
        else {
            const double B = p.bwHz > 0 ? p.bwHz : 0.9 * rate;
            const double f = k == KUsb ? B / 2 : k == KLsb ? -B / 2 : p.cwPitchHz;
            double ph = 0;
            const double w = 2 * kPi * f / rate;
            for (size_t i = 0; i < n; i++) { a[i] = (float)(x[i].real() * std::cos(ph) - x[i].imag() * std::sin(ph)); ph += w; }
        }
        toAudio(r, std::move(a), rate, p.audioRate);
        r.ok = true;
        return r;
    }
    // ---- bit recovery: a soft stream ---------------------------------------------------------------------------------------------
    if (p.baud <= 0) { r.msg = "set a baud rate (or press auto-detect)"; return r; }
    const double sps = rate / p.baud;
    if (sps < 3.5) { r.msg = "the baseband rate is too low for this baud rate"; return r; }
    std::vector<cf32> z;                 // the stream the timing works on (real kinds: imaginary part 0)
    std::vector<float> y;
    const bool psk = kindIsPsk(k);
    int M = k == KBpsk ? 2 : k == KQpsk ? 4 : 8;
    double cfo = 0;
    if (psk) {
        std::vector<cf32> w(x, x + n);
        if (p.carrierRecovery) {   // the x^M line
            const int L = 1 << (int)std::floor(std::log2((double)std::min<size_t>(n, 1u << 18)));
            std::vector<cf32> u((size_t)L);
            for (int i = 0; i < L; i++) {
                cf32 v = x[i];
                const float a = std::abs(v);
                v = a > 0 ? v / a : cf32(0, 0);
                cf32 q = v;
                for (int m = 1; m < M; m++) q *= v;
                u[(size_t)i] = q * (0.5f - 0.5f * (float)std::cos(2 * kPi * i / L));
            }
            std::vector<float> re, im;
            fft(u.data(), L, re, im);
            int best = 0;
            double bm = -1;
            for (int i = 0; i < L; i++) { const double m = std::norm(u[(size_t)i]); if (m > bm) { bm = m; best = i; } }
            auto mag = [&](int i) { return std::log(std::max(1e-30, (double)std::norm(u[(size_t)((i + L) % L)]))); };
            const double a = mag(best - 1), b = mag(best), c = mag(best + 1);
            const double den = a - 2 * b + c;
            const double off = std::fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0;
            double bin = best + off;
            if (bin > L / 2) bin -= L;
            cfo = bin * rate / L / M;
            const double wf = 2 * kPi * cfo / rate;
            for (size_t i = 0; i < n; i++) w[i] *= cf32((float)std::cos(wf * (double)i), (float)-std::sin(wf * (double)i));
        }
        z = firFilter(w, p.rolloff > 0 ? rrc(p.rolloff, sps, 10) : boxcar(sps));
        r.cfoHz = cfo;
    } else if (k == KAfsk) {
        std::vector<float> aud(n);
        double toneBase = 0;   // the tones' positions as seen in the stream: from the DC of the recording, less the band's centre for a complex baseband
        std::vector<cf32> src;
        if (p.fmFirst) {
            cf32 prev = x[0];
            for (size_t i = 0; i < n; i++) { aud[i] = (float)(std::arg(x[i] * std::conj(prev)) * rate / (2 * kPi)); prev = x[i]; }
            src.resize(n);
            for (size_t i = 0; i < n; i++) src[i] = cf32(aud[i], 0);
        } else { src.assign(x, x + n); toneBase = p.centreHz; }
        std::vector<cf32> mk(n), sp(n);
        const double wm = 2 * kPi * (p.markHz - toneBase) / rate, ws = 2 * kPi * (p.spaceHz - toneBase) / rate;
        for (size_t i = 0; i < n; i++) {
            mk[i] = src[i] * cf32((float)std::cos(wm * (double)i), (float)-std::sin(wm * (double)i));
            sp[i] = src[i] * cf32((float)std::cos(ws * (double)i), (float)-std::sin(ws * (double)i));
        }
        const std::vector<float> bx = boxcar(sps);
        mk = firFilter(mk, bx); sp = firFilter(sp, bx);
        y.resize(n);
        for (size_t i = 0; i < n; i++) {
            const double a = std::norm(mk[i]), b = std::norm(sp[i]);
            y[i] = (float)((a - b) / std::max(a + b, 1e-20));
        }
        z.resize(n);
        for (size_t i = 0; i < n; i++) z[i] = cf32(y[i], 0);
        if (p.fmFirst) toAudio(r, aud, rate, p.audioRate);
    } else {   // ASK, FSK, MSK, GMSK
        y.resize(n);
        if (k == KAsk) for (size_t i = 0; i < n; i++) y[i] = std::abs(x[i]);
        else { cf32 prev = x[0]; for (size_t i = 0; i < n; i++) { y[i] = (float)(std::arg(x[i] * std::conj(prev)) * rate / (2 * kPi)); prev = x[i]; } }
        const double cut = k == KAsk ? 0.8 : (k == KMsk || k == KGmsk) ? 0.6 : 0.7;
        const double fcn = std::min(0.45, cut * p.baud / rate);
        y = firReal(y, designLowpass(fcn, std::max(0.01, fcn * 0.8)));
        double m = 0;
        for (float v : y) m += v;
        m /= (double)n;
        for (float& v : y) v = (float)(v - m);
        r.cfoHz = m;
        z.resize(n);
        for (size_t i = 0; i < n; i++) z[i] = cf32(y[i], 0);
    }
    if (!psk && k != KAfsk) {   // the audio of FSK / ASK: the soft signal itself, for listening
        toAudio(r, y, rate, p.audioRate);
    } else if (k == KAfsk && !p.fmFirst) {
        // the tones themselves
        std::vector<float> a(n);
        for (size_t i = 0; i < n; i++) a[i] = x[i].real();
        toAudio(r, std::move(a), rate, p.audioRate);
    }
    // ---- timing ---------------------------------------------------------------------------------------------------------------------
    Timing tm = recoverTiming(z, sps, p.clock, p.loopBw);
    if (tm.s.size() < 8) { r.msg = "too few symbols in the selection"; return r; }
    r.sps = tm.sps;
    r.baud = rate / tm.sps;
    r.symbols = tm.s.size();
    const size_t ns = tm.s.size();
    // ---- decisions ------------------------------------------------------------------------------------------------------------------
    if (psk) {
        // carrier phase: the M-th power loop on the symbols
        std::vector<cf32> sy(ns);
        double th = 0, fr = 0;
        const double zeta = 0.707, g = std::max(0.0005, p.loopBw);
        const double kp = 4 * zeta * g, ki = 4 * g * g;
        double pw = 0;
        for (const cf32& v : tm.s) pw += (double)std::norm(v);
        pw = std::sqrt(pw / (double)ns);
        if (p.carrierRecovery) {
            cf32 acc(0, 0);
            for (size_t i = 0; i < std::min<size_t>(ns, 256); i++) { cf32 q = tm.s[i], v = q; for (int m = 1; m < M; m++) q *= v; acc += q; }
            th = std::arg(M == 4 ? -acc : acc) / M;   // QPSK sits at 45 degrees: its fourth power at 180
        }
        std::vector<double> thetas(ns);
        for (size_t i = 0; i < ns; i++) {
            const cf32 s = tm.s[i] / (float)pw * cf32((float)std::cos(th), (float)-std::sin(th));
            sy[i] = s;
            thetas[i] = th;
            if (p.carrierRecovery) {
                cf32 q = s;
                for (int m = 1; m < M; m++) q *= s;
                const double e = std::arg(M == 4 ? -q : q) / M;
                fr += ki * e;
                th += kp * e + fr;
            }
        }
        r.cfoHz = cfo + (p.carrierRecovery ? fr * (rate / tm.sps) / (2 * kPi) : 0.0);
        // decisions
        r.bitsPerSymbol = k == KBpsk ? 1 : k == KQpsk ? 2 : 3;
        int prevIdx = 0;
        double ev = 0;
        for (size_t i = 0; i < ns; i++) {
            const double ph = std::arg(sy[i]);
            int idx;
            double ideal;
            if (M == 2) { idx = std::real(sy[i]) > 0 ? 0 : 1; ideal = idx * kPi; }
            else if (M == 4) { idx = (((int)std::lround((ph - kPi / 4) / (kPi / 2)) % 4) + 4) % 4; ideal = kPi / 4 + idx * kPi / 2; }
            else { idx = (((int)std::lround(ph / (kPi / 4)) % 8) + 8) % 8; ideal = idx * kPi / 4; }
            ev += std::norm(sy[i] - cf32((float)std::cos(ideal), (float)std::sin(ideal)));
            int d = idx;
            if (p.differential) { d = ((idx - prevIdx) % M + M) % M; prevIdx = idx; }
            int v;
            if (M == 2) v = d == 0 ? 1 : 0;
            else if (M == 4) { static const int toGray[4] = {0, 2, 3, 1}; v = toGray[d]; }
            else v = d ^ (d >> 1);
            for (int b = r.bitsPerSymbol - 1; b >= 0; b--) r.bits.push_back((uint8_t)((v >> b) & 1));
        }
        ev /= (double)ns;
        r.snrDb = (float)(-10 * std::log10(std::max(ev, 1e-9)));
        for (size_t i = 0; i < ns; i++) r.constellation.push_back(sy[i]);
        if (r.constellation.size() > 20000) r.constellation.resize(20000);
        // the eye: the in-phase part round each symbol, rotated by the loop's phase there
        const int half = 16;
        r.eyeLen = 2 * half * 1 + 1;
        const size_t step = std::max<size_t>(1, ns / 300);
        for (size_t i = 2; i + 2 < ns; i += step) {
            for (int j = -half; j <= half; j++) {
                const cf32 v = interp(z, tm.t[i] + (double)j / half * tm.sps) / (float)pw * cf32((float)std::cos(thetas[i]), (float)-std::sin(thetas[i]));
                r.eye.push_back(v.real());
            }
            r.eyeTraces++;
        }
        r.ok = true;
        return r;
    }
    // real soft symbols
    std::vector<float> soft(ns);
    for (size_t i = 0; i < ns; i++) soft[i] = tm.s[i].real();
    r.soft = soft;
    std::vector<float> c;
    double snr = 0;
    if (k == KFsk4) {
        percentileCentres(soft, 4, c);
        r.bitsPerSymbol = 2;
        for (size_t i = 0; i < ns; i++) {
            const int l = nearest(c, soft[i]);   // 0 lowest .. 3 highest
            static const int bitsOf[4] = {3, 2, 0, 1};   // -3: 11, -1: 10, +1: 00, +3: 01
            int v = bitsOf[l];
            if (p.fskInvert) v = bitsOf[3 - l];
            r.bits.push_back((uint8_t)(v >> 1)); r.bits.push_back((uint8_t)(v & 1));
        }
        r.devHz = (c[3] - c[0]) / 2;
    } else {
        percentileCentres(soft, 2, c);
        r.bitsPerSymbol = 1;
        const float thr = 0.5f * (c[0] + c[1]);
        for (size_t i = 0; i < ns; i++) { bool b = soft[i] > thr; if (p.fskInvert) b = !b; r.bits.push_back(b ? 1 : 0); }
        r.devHz = (c[1] - c[0]) / 2;
        if (k == KFsk2 || k == KMsk || k == KGmsk) r.cfoHz += thr;
    }
    {   // SNR of the clusters
        std::vector<double> var(c.size(), 0.0);
        std::vector<size_t> cnt(c.size(), 0);
        for (float v : soft) { const int l = nearest(c, v); var[(size_t)l] += (v - c[(size_t)l]) * (v - c[(size_t)l]); cnt[(size_t)l]++; }
        double v = 0; size_t nn = 0;
        for (size_t i = 0; i < c.size(); i++) { v += var[i]; nn += cnt[i]; }
        v /= (double)std::max<size_t>(1, nn);
        const double half = c.size() >= 2 ? (c[1] - c[0]) / 2 : 0;
        snr = half * half / std::max(v, 1e-20);
        r.snrDb = (float)(10 * std::log10(std::max(snr, 1e-6)));
    }
    r.levels = c;
    // eye: the soft signal round each symbol, scaled so the outer levels are +-1
    {
        float sc = 0;
        for (float v : c) sc = std::max(sc, std::fabs(v));
        if (sc <= 0) sc = 1;
        // a centred eye: remove the middle of the levels
        const float mid = 0.5f * (c.front() + c.back());
        sc = std::max(1e-12f, 0.5f * (c.back() - c.front()));
        const int half = 16;
        r.eyeLen = 2 * half + 1;
        const size_t step = std::max<size_t>(1, ns / 300);
        for (size_t i = 2; i + 2 < ns; i += step) {
            for (int j = -half; j <= half; j++) r.eye.push_back((interp(z, tm.t[i] + (double)j / half * tm.sps).real() - mid) / sc);
            r.eyeTraces++;
        }
    }
    r.ok = true;
    return r;
}

DemodResult demodulate(const Source& s, uint64_t t0, uint64_t t1, const DemodParams& p) {
    DemodResult r;
    double bw = p.bwHz, centre = p.centreHz;
    if (bw <= 0) { bw = s.real() ? 0.96 * s.rate() / 2 : 0.96 * s.rate(); centre = s.real() ? s.rate() / 4 : 0; }
    double wanted;
    if (kindHasBits(p.kind) && p.baud > 0) wanted = std::max(bw * 2.5, p.baud * 8);
    else wanted = std::max(bw * 2.5, p.kind <= KCw ? 16000.0 : 0.0);
    wanted = std::min(wanted, s.rate());
    Extracted e;
    if (!extractBand(s, t0, t1, centre, bw, wanted, e, p.maxSamples)) { r.msg = "nothing to demodulate"; return r; }
    DemodParams q = p;
    q.bwHz = bw; q.centreHz = centre;
    r = demodulateBaseband(e.x.data(), e.x.size(), e.rate, q);
    r.startSample = t0;
    if (e.truncated && r.ok) r.msg = "the selection was longer than the limit: the first part was used";
    return r;
}

} // namespace wb
} // namespace dect2
