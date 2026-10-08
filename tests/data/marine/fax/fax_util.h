// Helpers shared by the fax tests: audio from the test source with impairments, decoding, and comparison with the chart.
#pragma once
#include "dect2/marine_fax.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace faxt {
using namespace dect2;

static int fails = 0;
#define FCHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); faxt::fails++; } } while (0)

struct Opts {
    double rate = 12000;
    int ioc = 576, lpm = 120, lines = 200;
    uint32_t seed = 1;
    double seconds = 0;          // 0: one full transmission up to the end of the stop tone
    double ppm = 0;              // audio clock error: the source runs at rate * (1 + ppm * 1e-6)
    double snrDb = 1000;         // audio SNR over 0 .. rate / 2 (white noise)
    double mistuneHz = 0;        // everything shifts up by this much (a real sideband tuning error shifts all tones)
    double dc = 0;
    int quantBits = 0;
    double phasingS = 30;
    std::vector<double> gaps;    // start times in seconds of dropouts of gapS seconds (audio set to zero)
    double gapS = 0.02;
    double gain = 1.0;
    double fadeDepth = 0, fadeHz = 0.3;   // slow amplitude fading: envelope 1 + depth * sin, normalised to a mean of 1
    uint32_t noiseSeed = 7;
};

// Mistuning of a USB signal shifts every tone by the same number of Hz; the source cannot do that itself, so the audio
// is shifted with a Hilbert-free trick: the source is run through a frequency shift on its analytic signal. Here a simple
// way: generate the source at the wanted tones by resampling is not equivalent, so use a one-sided FIR Hilbert transformer.
inline std::vector<float> hilbertShift(const std::vector<float>& x, double fs, double shiftHz) {
    const int N = 255, h = N / 2;
    std::vector<double> ht(N, 0.0);
    for (int i = 0; i < N; i++) {
        const int m = i - h;
        if (m % 2 != 0) {
            const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N);   // Hann
            ht[i] = (2.0 / (M_PI * m)) * w;
        }
    }
    std::vector<float> y(x.size());
    double ph = 0;
    const double step = 2 * M_PI * shiftHz / fs;
    for (size_t n = 0; n < x.size(); n++) {
        double im = 0;
        for (int i = 0; i < N; i++) {
            const long k = static_cast<long>(n) - i + h;      // centred: output n uses x[n + h - i]
            if (k >= 0 && k < static_cast<long>(x.size())) im += ht[i] * x[k];
        }
        const double re = x[n];
        y[n] = static_cast<float>(re * std::cos(ph) - im * std::sin(ph));
        ph += step;
    }
    return y;
}

inline std::vector<float> makeAudio(const Opts& o) {
    FaxAudioSource src(o.rate * (1.0 + o.ppm * 1e-6), o.ioc, o.lpm, o.lines, o.seed);
    src.setTiming(5, o.phasingS, 5, 1);
    const double lineS = 60.0 / o.lpm;
    double secs = o.seconds > 0 ? o.seconds : 5 + std::round(o.phasingS * o.lpm / 60.0) * lineS + lineS + o.lines * lineS + 6.5;
    const size_t n = static_cast<size_t>(secs * o.rate);
    std::vector<float> a(n);
    // The source rate differs by ppm from the decoder's rate: that is the clock error.
    src.generate(a.data(), n);
    if (o.mistuneHz != 0) a = hilbertShift(a, o.rate, o.mistuneHz);
    std::mt19937 rng(o.noiseSeed);
    std::normal_distribution<double> g(0, 1);
    if (o.fadeDepth > 0)
        for (size_t i = 0; i < a.size(); i++) a[i] *= static_cast<float>(1.0 + o.fadeDepth * std::sin(2 * M_PI * o.fadeHz * i / o.rate));
    if (o.gain != 1.0) for (float& v : a) v *= static_cast<float>(o.gain);
    if (o.snrDb < 500) {
        // signal power: sine amplitude 0.5 -> 0.125
        const double sigma = std::sqrt(0.125 * o.gain * o.gain / std::pow(10.0, o.snrDb / 10.0));
        for (float& v : a) v += static_cast<float>(sigma * g(rng));
    }
    if (o.dc != 0) for (float& v : a) v += static_cast<float>(o.dc);
    if (o.quantBits > 0) {
        const double q = std::pow(2.0, o.quantBits - 1);
        for (float& v : a) v = static_cast<float>(std::round(std::min(1.0, std::max(-1.0, (double)v)) * q) / q);
    }
    for (double at : o.gaps) {
        const size_t g0 = static_cast<size_t>(at * o.rate), g1 = std::min(n, g0 + static_cast<size_t>(o.gapS * o.rate));
        for (size_t i = g0; i < g1; i++) a[i] = 0;
    }
    return a;
}

inline void feedAll(FaxDecoder& d, const std::vector<float>& a, size_t chunk) {
    for (size_t i = 0; i < a.size(); i += chunk) d.push(a.data() + i, std::min(chunk, a.size() - i));
}

// Row shift (pixels, cyclic, sub-pixel) that best matches the decoded row to the reference row; also the correlation there.
inline void rowShift(const uint8_t* dec, const uint8_t* ref, int W, int range, double& shift, double& corr) {
    std::vector<double> c(2 * range + 1, 0.0);
    double mr = 0, md = 0;
    for (int i = 0; i < W; i++) { mr += ref[i]; md += dec[i]; }
    mr /= W; md /= W;
    double vr = 0, vd = 0;
    for (int i = 0; i < W; i++) { vr += (ref[i] - mr) * (ref[i] - mr); vd += (dec[i] - md) * (dec[i] - md); }
    int best = 0;
    for (int s = -range; s <= range; s++) {
        double acc = 0;
        for (int i = 0; i < W; i++) {
            int j = (i + s) % W; if (j < 0) j += W;
            acc += (dec[j] - md) * (ref[i] - mr);
        }
        c[s + range] = acc;
        if (acc > c[best + range]) best = s;
    }
    shift = best;
    if (best > -range && best < range) {
        const double l = c[best + range - 1], m = c[best + range], r = c[best + range + 1];
        const double den = l - 2 * m + r;
        if (den < 0) shift += 0.5 * (l - r) / den;
    }
    corr = vr < 1e-6 ? -2.0 : (vd > 0 ? c[best + range] / std::sqrt(vr * vd) : 0);   // -2: the chart row is flat, nothing to correlate
}

// Correlation of every row (decoded row r + rowOffset against chart row r) after the best shift within +-6 px.
inline std::vector<double> rowCorrelations(const FaxImage& dec, const FaxImage& ref, int rowOffset) {
    std::vector<double> c;
    if (dec.width != ref.width) return c;
    for (int r = 0; r < ref.height && r + rowOffset < dec.height; r++) {
        double s, k;
        rowShift(&dec.pix[static_cast<size_t>(r + rowOffset) * ref.width], &ref.pix[static_cast<size_t>(r) * ref.width], ref.width, 6, s, k);
        c.push_back(k);
    }
    return c;
}

struct Quality {
    bool ok = false;
    double meanCorr = 0;         // mean row correlation after the best shift
    double shiftFirst = 0, shiftLast = 0, drift = 0;
    int rows = 0;
};

// Decoded row r + rowOffset belongs to chart row r.
inline Quality compare(const FaxImage& dec, const FaxImage& ref, int rowOffset, int margin = 20) {
    Quality q;
    if (dec.width != ref.width || dec.height < rowOffset + ref.height - 2) return q;
    q.ok = true;
    q.rows = dec.height;
    const int W = ref.width;
    double sumC = 0; int nC = 0;
    double first = 0, last = 0; int nf = 0, nl = 0;
    const int rA = std::min(margin, ref.height / 4), rB = ref.height - std::min(margin, ref.height / 4);
    const int step = std::max(1, (rB - rA) / 40);
    for (int r = rA; r < rB; r += step) {
        double s, c;
        rowShift(&dec.pix[static_cast<size_t>(r + rowOffset) * W], &ref.pix[static_cast<size_t>(r) * W], W, 40, s, c);
        sumC += c; nC++;
        if (r < rA + (rB - rA) / 5) { first += s; nf++; }
        if (r >= rB - (rB - rA) / 5) { last += s; nl++; }
    }
    q.meanCorr = nC ? sumC / nC : 0;
    q.shiftFirst = nf ? first / nf : 0;
    q.shiftLast = nl ? last / nl : 0;
    q.drift = q.shiftLast - q.shiftFirst;
    return q;
}

} // namespace faxt
