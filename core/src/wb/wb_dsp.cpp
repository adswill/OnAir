// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/wb/wb_dsp.h"
#include "dect2/common/window.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace wb {

namespace {
using dect2::math::kPi;

double bessel0(double x) { return dect2::window::besselI0(x, 40, 1e-12); }
} // namespace

const char* windowName(int w) {
    static const char* n[WinCount] = {"Rectangular", "Hann", "Hamming", "Blackman", "Blackman-Harris", "Flat top", "Kaiser 8"};
    return w >= 0 && w < WinCount ? n[w] : "?";
}

std::vector<float> makeWindow(int w, int n) {
    std::vector<float> o((size_t)n, 1.f);
    if (n < 2) return o;
    const double N = n;
    for (int i = 0; i < n; i++) {
        const double x = 2 * kPi * i / N;   // periodic windows (spectral analysis)
        double v = 1;
        switch (w) {
        case WinHann: v = 0.5 - 0.5 * std::cos(x); break;
        case WinHamming: v = 0.54 - 0.46 * std::cos(x); break;
        case WinBlackman: v = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x); break;
        case WinBlackmanHarris: v = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x); break;
        case WinFlatTop: v = 0.21557895 - 0.41663158 * std::cos(x) + 0.277263158 * std::cos(2 * x) - 0.083578947 * std::cos(3 * x) + 0.006947368 * std::cos(4 * x); break;
        case WinKaiser: { const double r = 2.0 * i / N - 1.0; v = bessel0(8 * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(8); break; }
        default: break;
        }
        o[(size_t)i] = (float)v;
    }
    return o;
}

void fft(cf32* x, int n, std::vector<float>& re, std::vector<float>& im) {
    int l = 0;
    while ((1 << l) < n) l++;
    if ((int)re.size() < n) { re.resize((size_t)n); im.resize((size_t)n); }
    for (int i = 0; i < n; i++) { re[(size_t)i] = x[i].real(); im[(size_t)i] = x[i].imag(); }
    fftSplit(re.data(), im.data(), l, false);
    for (int i = 0; i < n; i++) x[i] = cf32(re[(size_t)i], im[(size_t)i]);
}

std::vector<float> designLowpass(double cutoff, double transition) {
    transition = std::max(transition, 1e-4);
    int n = (int)std::ceil(5.5 / transition) | 1;
    n = std::max(7, std::min(n, 8191));
    std::vector<float> h((size_t)n);
    const double c = (n - 1) / 2.0;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double t = i - c;
        const double s = t == 0 ? 2 * cutoff : std::sin(2 * kPi * cutoff * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
        h[(size_t)i] = (float)(s * w);
        sum += s * w;
    }
    for (float& v : h) v = (float)(v / sum);
    return h;
}

double dbOf(double p) { return 10 * std::log10(std::max(p, 1e-30)); }

// ---- band extractor -----------------------------------------------------------------------------------------------------------

namespace {

struct Stage {
    int D = 1;
    std::vector<float> taps;
    std::vector<float> re, im;   // buffer: the history plus what has not been used yet
    size_t pos = 0;
};

} // namespace

struct BandExtractor::Impl {
    double srcRate = 0, centre = 0, bw = 0, outRate = 0;
    bool real = false, passthrough = false;
    int D = 1;
    std::vector<Stage> stages;
    // Hilbert
    std::vector<float> hil, hbuf;
    // oscillator
    double phase = 0, dphi = 0;
    cf32 osc = cf32(1, 0), rot = cf32(1, 0);
    int renorm = 0;
    std::vector<float> a_re, a_im, t_re, t_im, o_re, o_im;
};

BandExtractor::BandExtractor(double srcRate, bool realInput, double centreHz, double bwHz, double wantedRate) : p_(new Impl) {
    Impl& m = *p_;
    m.srcRate = srcRate;
    m.real = realInput;
    int D = 1;
    if (wantedRate > 0) D = std::max(1, (int)std::floor(srcRate / wantedRate));
    m.D = D;
    if (bwHz <= 0) {
        m.passthrough = true;
        bwHz = 0.8 * srcRate / D;
        centreHz = 0;
    }
    m.centre = centreHz; m.bw = bwHz;
    if (m.passthrough && D == 1) { m.outRate = srcRate; return; }
    // stages: 8 at a time, the rest last
    std::vector<int> ds;
    int rem = D;
    while (rem >= 16) { ds.push_back(8); rem /= 8; }
    if (rem > 1) ds.push_back(rem);
    int total = 1;
    for (int d : ds) total *= d;
    m.D = total;
    m.outRate = srcRate / total;
    double rateIn = srcRate;
    for (size_t i = 0; i < ds.size(); i++) {
        Stage st;
        st.D = ds[i];
        const double rateOut = rateIn / ds[i];
        const bool last = i + 1 == ds.size();
        double edge = last ? bwHz / 2 : std::min(bwHz / 2 * 1.05, 0.4 * rateOut);   // passband edge, Hz
        edge = std::min(edge, 0.4 * rateOut);
        double stop = rateOut - bwHz / 2 * (last ? 1.0 : 1.05);                    // where aliases would reach the band
        stop = std::max(stop, edge * 1.2);
        const double mid = 0.5 * (edge + stop);
        st.taps = designLowpass(mid / rateIn, (stop - edge) / rateIn);
        st.re.assign(st.taps.size() - 1, 0.f); st.im.assign(st.taps.size() - 1, 0.f);
        m.stages.push_back(std::move(st));
        rateIn = rateOut;
    }
    if (m.real && !m.passthrough) {
        const int N = 127;
        m.hil.resize(N);
        for (int k = 0; k < N; k++) {
            const int j = (N - 1) / 2 - k;
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (N - 1)) + 0.08 * std::cos(4 * kPi * k / (N - 1));
            m.hil[(size_t)k] = (j & 1) ? (float)(2.0 / (kPi * j) * w) : 0.f;
        }
        m.hbuf.assign(N - 1, 0.f);
    }
    m.dphi = -2 * kPi * m.centre / srcRate;
    m.rot = cf32((float)std::cos(m.dphi), (float)std::sin(m.dphi));
}

BandExtractor::~BandExtractor() { delete p_; }
double BandExtractor::outRate() const { return p_->outRate; }
int BandExtractor::decimation() const { return p_->D; }

void BandExtractor::push(const cf32* in, size_t n, std::vector<cf32>& out) {
    Impl& m = *p_;
    if (n == 0) return;
    if (m.passthrough && m.D == 1) { out.insert(out.end(), in, in + n); return; }
    m.a_re.resize(n); m.a_im.resize(n);
    if (m.real && !m.passthrough) {
        // analytic signal: re = x delayed by 63, im = Hilbert(x); the first 63 outputs of the first call are the start-up
        const size_t H = m.hil.size();
        m.t_re.assign(m.hbuf.begin(), m.hbuf.end());
        for (size_t i = 0; i < n; i++) m.t_re.push_back(in[i].real());
        m.t_im.resize(n);
        desamp(m.t_re.data(), 1, m.hil.data(), m.t_im.data(), (int)n, (int)H);
        for (size_t i = 0; i < n; i++) { m.a_re[i] = m.t_re[i + (H - 1) / 2]; m.a_im[i] = m.t_im[i]; }
        m.hbuf.assign(m.t_re.end() - (long)(H - 1), m.t_re.end());
    } else {
        for (size_t i = 0; i < n; i++) { m.a_re[i] = in[i].real(); m.a_im[i] = in[i].imag(); }
    }
    if (!m.passthrough && m.centre != 0) {
        for (size_t i = 0; i < n; i++) {
            const float r = m.a_re[i], q = m.a_im[i];
            m.a_re[i] = r * m.osc.real() - q * m.osc.imag();
            m.a_im[i] = r * m.osc.imag() + q * m.osc.real();
            m.osc *= m.rot;
            if (++m.renorm >= 2048) { m.renorm = 0; m.osc /= std::abs(m.osc); }
        }
    }
    const float* cr = m.a_re.data();
    const float* ci = m.a_im.data();
    size_t cn = n;
    for (Stage& st : m.stages) {
        st.re.insert(st.re.end(), cr, cr + cn);
        st.im.insert(st.im.end(), ci, ci + cn);
        const size_t N = st.taps.size();
        const size_t len = st.re.size();
        size_t nout = 0;
        if (len >= st.pos + N) nout = (len - st.pos - N) / (size_t)st.D + 1;
        m.o_re.resize(std::max<size_t>(nout, 1)); m.o_im.resize(std::max<size_t>(nout, 1));
        if (nout) {
            desamp(st.re.data() + st.pos, st.D, st.taps.data(), m.o_re.data(), (int)nout, (int)N);
            desamp(st.im.data() + st.pos, st.D, st.taps.data(), m.o_im.data(), (int)nout, (int)N);
        }
        st.pos += nout * (size_t)st.D;
        if (st.pos > 0) {   // drop what is used (keep N - 1 of history: pos counts from the history's start)
            st.re.erase(st.re.begin(), st.re.begin() + (long)st.pos);
            st.im.erase(st.im.begin(), st.im.begin() + (long)st.pos);
            st.pos = 0;
        }
        m.t_re.assign(m.o_re.begin(), m.o_re.begin() + (long)nout);
        m.t_im.assign(m.o_im.begin(), m.o_im.begin() + (long)nout);
        cr = m.t_re.data(); ci = m.t_im.data(); cn = nout;
        // the next stage copies from t_*, which it must not overwrite: swap into a_*
        m.a_re.swap(m.t_re); m.a_im.swap(m.t_im);
        cr = m.a_re.data(); ci = m.a_im.data();
        if (cn == 0) break;
    }
    for (size_t i = 0; i < cn; i++) out.emplace_back(cr[i], ci[i]);
}

bool extractBand(const Source& s, uint64_t start, uint64_t end, double centreHz, double bwHz, double wantedRate, Extracted& out, size_t maxOut,
                 const std::function<bool(double)>& progress) {
    end = std::min<uint64_t>(end, s.length());
    out = Extracted();
    if (start >= end) return false;
    BandExtractor be(s.rate(), s.real(), centreHz, bwHz, wantedRate);
    out.rate = be.outRate(); out.decim = be.decimation();
    out.centreHz = bwHz > 0 ? centreHz : 0;
    out.bwHz = bwHz > 0 ? bwHz : s.rate();
    out.start = start;
    const size_t chunk = 1u << 17;
    std::vector<cf32> buf(chunk);
    out.x.reserve((size_t)std::min<uint64_t>((end - start) / (uint64_t)out.decim + 16, maxOut + chunk));
    uint64_t pos = start;
    while (pos < end) {
        const size_t want = (size_t)std::min<uint64_t>(chunk, end - pos);
        const size_t got = s.read(pos, want, buf.data());
        if (got == 0) break;
        be.push(buf.data(), got, out.x);
        pos += got;
        if (out.x.size() >= maxOut) { out.truncated = pos < end || out.x.size() > maxOut; out.x.resize(maxOut); break; }
        if (progress && !progress((double)(pos - start) / (double)(end - start))) return false;
    }
    out.srcCount = pos - start;
    return !out.x.empty();
}

// ---- spectra ------------------------------------------------------------------------------------------------------------------

void welch(const cf32* x, size_t n, int fftSize, int window, double overlap, bool real, std::vector<float>& outDb) {
    std::vector<float> w = makeWindow(window, fftSize);
    double sw = 0;
    for (float v : w) sw += v;
    std::vector<double> acc((size_t)fftSize, 0.0);
    std::vector<cf32> buf((size_t)fftSize);
    std::vector<float> re, im;
    const size_t step = std::max<size_t>(1, (size_t)((double)fftSize * (1.0 - std::min(0.95, std::max(0.0, overlap)))));
    int cnt = 0;
    for (size_t p = 0; p + (size_t)fftSize <= n; p += step) {
        for (int i = 0; i < fftSize; i++) buf[(size_t)i] = x[p + (size_t)i] * w[(size_t)i];
        fft(buf.data(), fftSize, re, im);
        for (int i = 0; i < fftSize; i++) acc[(size_t)i] += (double)std::norm(buf[(size_t)i]);
        cnt++;
    }
    if (cnt == 0) { outDb.clear(); return; }
    const double norm = 1.0 / ((double)cnt * sw * sw);
    if (real) {
        outDb.resize((size_t)fftSize / 2 + 1);
        for (int i = 0; i <= fftSize / 2; i++) outDb[(size_t)i] = (float)dbOf(acc[(size_t)i] * norm * 4);
    } else {
        outDb.resize((size_t)fftSize);
        for (int i = 0; i < fftSize; i++) outDb[(size_t)i] = (float)dbOf(acc[(size_t)((i + fftSize / 2) % fftSize)] * norm);
    }
}

std::vector<float> resampleReal(const std::vector<float>& in, double inRate, double outRate) {
    if (in.empty() || inRate <= 0 || outRate <= 0) return {};
    std::vector<float> src;
    const std::vector<float>* p = &in;
    if (outRate < inRate * 0.999) {
        const std::vector<float> h = designLowpass(0.45 * outRate / inRate, 0.1 * outRate / inRate);
        const size_t N = h.size();
        src.assign(in.size(), 0.f);
        std::vector<float> pad(in.size() + N - 1, 0.f);
        std::copy(in.begin(), in.end(), pad.begin() + (long)((N - 1) / 2));
        desamp(pad.data(), 1, h.data(), src.data(), (int)in.size(), (int)N);
        p = &src;
    }
    const size_t no = (size_t)((double)in.size() * outRate / inRate);
    std::vector<float> o(no);
    const double r = inRate / outRate;
    for (size_t i = 0; i < no; i++) {
        const double t = (double)i * r;
        const size_t k = (size_t)t;
        const double f = t - (double)k;
        const float a = (*p)[std::min(k, p->size() - 1)], b = (*p)[std::min(k + 1, p->size() - 1)];
        o[i] = (float)(a + (b - a) * f);
    }
    return o;
}

} // namespace wb
} // namespace dect2
