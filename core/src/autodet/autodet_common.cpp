// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Automatic standard detection: shared helpers and the names (autodet.h).
#include "autodet_internal.h"
#include "dect2/dsp/carrierfind.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>

namespace dect2 {
namespace autodet {

const char* verdictName(int v) {
    switch (v) {
    case VerdictUnsure: return "unsure";
    case VerdictLikely: return "likely";
    case VerdictSure: return "sure";
    default: return "no signal";
    }
}

const char* tvStdName(int s) {
    static const char* const t[TvCount] = {"unknown", "DVB-T", "DVB-T2", "DVB-C", "DVB-S/S2", "ISDB-T", "ATSC 1.0", "ATSC 3.0", "DTMB", "Analogue TV"};
    return s >= 0 && s < TvCount ? t[s] : "unknown";
}
int tvEngineStd(int s) {
    static const int t[TvCount] = {-1, 2, 1, 42, 8, 6, 3, 5, 9, 10};
    return s >= 0 && s < TvCount ? t[s] : -1;
}
int tvAppFamily(int s) {
    static const int t[TvCount] = {-1, 0, 0, 40, 6, 4, 1, 3, 7, 8};
    return s >= 0 && s < TvCount ? t[s] : -1;
}

const char* voiceSysName(int s) {
    static const char* const t[VoiceCount] = {"unknown", "DMR", "P25 Phase 1", "P25 Phase 2", "NXDN48", "NXDN96", "D-STAR", "dPMR", "Yaesu System Fusion",
                                              "TETRA", "M17", "Analogue FM"};
    return s >= 0 && s < VoiceCount ? t[s] : "unknown";
}
const char* voiceModeId(int s) {
    static const char* const t[VoiceCount] = {"", "dmr", "p25", "p25p2", "nxdn", "nxdn", "dstar", "dpmr", "fusion", "tetra", "m17", "general"};
    return s >= 0 && s < VoiceCount ? t[s] : "";
}
int voiceAppFamily(int s) {
    static const int t[VoiceCount] = {-1, 9, 50, 180, 38, 38, 33, 45, 128, 46, 31, 55};
    return s >= 0 && s < VoiceCount ? t[s] : -1;
}

namespace detail {

double dB(double x) { return 10 * std::log10(std::max(1e-30, x)); }

double quantile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)(std::max(0.0, q) * (double)v.size()));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

std::vector<double> welch(const cf32* x, size_t n, int nfft, int maxSegs) {
    std::vector<double> acc((size_t)nfft, 0.0);
    if (n < (size_t)nfft || nfft < 8) return acc;
    std::vector<float> win((size_t)nfft);
    for (int i = 0; i < nfft; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / nfft));
    const size_t segs = (n - (size_t)nfft) / (size_t)(nfft / 2) + 1;
    const size_t use = std::min(segs, (size_t)std::max(1, maxSegs));
    Fft f(nfft);
    std::vector<cf32> b((size_t)nfft);
    for (size_t s = 0; s < use; s++) {
        const size_t seg = use == segs ? s : s * (segs - 1) / std::max<size_t>(1, use - 1);
        const cf32* p = x + seg * (size_t)(nfft / 2);
        for (int i = 0; i < nfft; i++) b[(size_t)i] = p[i] * win[(size_t)i];
        f.forward(b.data());
        for (int k = 0; k < nfft; k++) acc[(size_t)((k + nfft / 2) % nfft)] += std::norm(b[(size_t)k]);
    }
    for (double& v : acc) v /= (double)use;
    return acc;
}

std::vector<cf32> toBaseband(const cf32* x, size_t n, double rate, double centreHz, double occupiedHz, double wantRate, double& gotRate) {
    std::vector<cf32> out;
    gotRate = 0;
    {   // an integer decimation (L = 1), so the rate is exact: a symbol rate measured from it is right
        const int k = std::max(1, (int)std::floor(rate / wantRate));
        const double r = rate / k;
        Channelizer ch;
        if (k > 1 && ch.configure(rate, r, centreHz, std::min(occupiedHz, r * 0.9))) {
            out.reserve((size_t)((double)n / k) + 64);
            ch.process(x, n, out);
            gotRate = r;
            return out;
        }
        if (k == 1) {   // no decimation: only the mix
            out.resize(n);
            const double w = -2 * M_PI * centreHz / rate;
            std::complex<double> ph(1, 0);
            const std::complex<double> st(std::cos(w), std::sin(w));
            for (size_t i = 0; i < n; i++) {
                out[i] = cf32((float)(x[i].real() * ph.real() - x[i].imag() * ph.imag()), (float)(x[i].real() * ph.imag() + x[i].imag() * ph.real()));
                ph *= st;
                if ((i & 1023) == 0) ph /= std::abs(ph);
            }
            gotRate = rate;
            return out;
        }
    }
    // no rational ratio (an odd radio rate): mix and decimate by an integer with a windowed-sinc low-pass
    const int D = std::max(1, (int)std::floor(rate / wantRate));
    gotRate = rate / D;
    const double fc = std::min(0.45, occupiedHz / 2 / rate * 1.2);
    const int taps = std::min(4001, std::max(31, (int)(4.0 / std::max(1e-6, (fc * 0.5))) | 1));
    std::vector<float> h((size_t)taps);
    double hs = 0;
    for (int i = 0; i < taps; i++) {
        const double t = i - (taps - 1) / 2.0;
        const double s = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        h[(size_t)i] = (float)(s * w); hs += s * w;
    }
    for (float& v : h) v = (float)(v / hs);
    const double w = -2 * M_PI * centreHz / rate;
    for (size_t o = (size_t)taps; o < n; o += (size_t)D) {
        std::complex<double> a(0, 0);
        for (int i = 0; i < taps; i++) {
            const size_t k = o - (size_t)i;
            const double ph = w * (double)k;
            a += std::complex<double>(x[k]) * std::complex<double>(std::cos(ph), std::sin(ph)) * (double)h[(size_t)i];
        }
        out.push_back(cf32((float)a.real(), (float)a.imag()));
    }
    return out;
}

Occupied occupied(const std::vector<double>& psd, double rate, double minSnrDb) {
    Occupied o;
    const int N = (int)psd.size();
    if (N < 64) return o;
    const int w = std::max(3, N / 256) | 1;   // ~0.4 % of the span: fills the narrow fades of multipath
    std::vector<double> d((size_t)N);
    {
        std::vector<double> lin((size_t)N);
        for (int k = 0; k < N; k++) {
            double s = 0; int c = 0;
            for (int j = -w / 2; j <= w / 2; j++) { const int q = k + j; if (q >= 0 && q < N) { s += psd[(size_t)q]; c++; } }
            lin[(size_t)k] = s / c;
        }
        for (int k = 0; k < N; k++) d[(size_t)k] = dB(lin[(size_t)k]);
    }
    const int edge = N / 50;   // the radio's filter roll-off
    std::vector<double> inner(d.begin() + edge, d.end() - edge);
    const double floorDb = quantile(inner, 0.08);
    // start from the middle of the strongest wide stretch (a pilot or a vision carrier is one bin, not the signal)
    int kmax = edge;
    {
        const int ww = std::max(4, N / 64);
        double best = -1e300, acc = 0;
        for (int k = edge; k < N - edge; k++) {
            acc += d[(size_t)k];
            if (k - edge >= ww) acc -= d[(size_t)(k - ww)];
            if (k - edge >= ww - 1 && acc > best) { best = acc; kmax = k - ww / 2; }
        }
    }
    auto run = [&](double th, int& lo, int& hi) {
        const int gap = std::max(2, N / 40);   // a fade up to 2.5 % of the span wide does not split the signal
        lo = hi = kmax;
        for (int k = kmax, miss = 0; k >= 1 && miss <= gap; k--) { if (d[(size_t)k] >= th) { lo = k; miss = 0; } else miss++; }
        for (int k = kmax, miss = 0; k < N - 1 && miss <= gap; k++) { if (d[(size_t)k] >= th) { hi = k; miss = 0; } else miss++; }
    };
    int lo, hi;
    run(floorDb + 3, lo, hi);
    std::vector<double> in(d.begin() + lo, d.begin() + hi + 1);
    const double plateau = quantile(in, 0.5);
    if (plateau - floorDb < minSnrDb) { o.floorDb = floorDb; o.plateauDb = plateau; return o; }
    int l8, h8, l2, h2;
    run(floorDb + (plateau - floorDb) * 0.8, l8, h8);
    run(floorDb + (plateau - floorDb) * 0.2, l2, h2);
    run(floorDb + (plateau - floorDb) / 2, lo, hi);
    o.edge = hi > lo ? (double)((h2 - l2) - (h8 - l8)) / (double)(hi - lo) : 1;
    o.ok = true;
    o.loBin = lo; o.hiBin = hi;
    const double bin = rate / N;
    o.loHz = (lo - N / 2 - 0.5) * bin; o.hiHz = (hi - N / 2 + 0.5) * bin;
    o.centreHz = (o.loHz + o.hiHz) / 2; o.bwHz = o.hiHz - o.loHz;
    o.floorDb = floorDb; o.plateauDb = plateau; o.snrDb = plateau - floorDb;
    return o;
}

} // namespace detail
} // namespace autodet
} // namespace dect2
