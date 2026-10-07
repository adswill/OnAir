// DTMB demodulator pieces (see dtmb_demod.h).
#include "dect2/dtmb_demod.h"
#include <algorithm>
#include <cmath>
#include "dtmb_simd.h"

namespace dect2::dtmb {

// ---------------------------------------------------------------- channel estimate
namespace {
// sum over t < n of (re + j im)[t] * c[t]; re and im carry zeros up to the next multiple of 8, the chips may run on
inline void correlate(const float* re, const float* im, const float* c, int n, float& sr, float& si) {
    dotRI(re, im, c, (n + 7) & ~7, sr, si);
}
} // namespace

HeaderEstimator::HeaderEstimator(Header h) : h_(h) {
    amp_ = std::sqrt(headerInfo(h).powerRatio / 2.0);
    setWindow(16, 64);
}

void HeaderEstimator::setWindow(int pre, int post) {
    const HeaderInfo& hi = headerInfo(h_);
    // the window must leave room for rows: a full period for the cyclic headers, a few hundred rows for PN595
    // (PN595: the fixed sequence must give clearly more rows than unknown taps)
    const int maxTotal = hi.cyclic() ? hi.prefix + hi.suffix : (hi.length + 1) * 10 / 26;
    pre = std::max(0, pre);
    post = std::max(1, post);
    if (pre + post > maxTotal) { const double f = (double)maxTotal / (pre + post); pre = (int)(pre * f); post = std::max(1, (int)(post * f)); }
    pre_ = pre; post_ = post;
    const int L = pre + post;
    if (hi.cyclic()) {
        rows_ = hi.core;
        const int lo = post - 1, hiT = hi.length - hi.core - pre;   // first row index range
        t0_ = (lo + hiT) / 2;
    } else {
        t0_ = post - 1;
        rows_ = hi.length - pre - t0_;
        // normal matrix of the fixed sequence over these rows
        const auto& c = pnChips(h_);
        std::vector<double> a((size_t)L * (size_t)L, 0.0);
        for (int m = 0; m < L; m++) for (int k = m; k < L; k++) {
            double s = 0;
            for (int t = t0_; t < t0_ + rows_; t++) s += (double)c[(size_t)(t - (m - pre))] * (double)c[(size_t)(t - (k - pre))];
            a[(size_t)m * (size_t)L + (size_t)k] = a[(size_t)k * (size_t)L + (size_t)m] = s;
        }
        for (int i = 0; i < L; i++) a[(size_t)i * (size_t)L + (size_t)i] += 1e-3;   // tiny regularisation
        // invert by Gauss-Jordan (symmetric positive definite)
        minv_.assign((size_t)L * (size_t)L, 0.0);
        for (int i = 0; i < L; i++) minv_[(size_t)i * (size_t)L + (size_t)i] = 1.0;
        for (int col = 0; col < L; col++) {
            const double d = a[(size_t)col * (size_t)L + (size_t)col];
            for (int j = 0; j < L; j++) { a[(size_t)col * (size_t)L + (size_t)j] /= d; minv_[(size_t)col * (size_t)L + (size_t)j] /= d; }
            for (int r = 0; r < L; r++) {
                if (r == col) continue;
                const double f = a[(size_t)r * (size_t)L + (size_t)col];
                if (f == 0.0) continue;
                for (int j = 0; j < L; j++) { a[(size_t)r * (size_t)L + (size_t)j] -= f * a[(size_t)col * (size_t)L + (size_t)j]; minv_[(size_t)r * (size_t)L + (size_t)j] -= f * minv_[(size_t)col * (size_t)L + (size_t)j]; }
            }
        }
    }
}

void HeaderEstimator::estimate(const cf32* r, int phase, Taps& out) {
    const HeaderInfo& hi = headerInfo(h_);
    const int L = pre_ + post_;
    chipf_.assign((size_t)hi.length + 16, 0.f);
    {
        int8_t c[945];
        pnHeader(h_, phase, c);
        for (int i = 0; i < hi.length; i++) chipf_[(size_t)i] = (float)c[i];
    }
    // b[m] = sum_t r[t] chip[t - m] over the rows: the correlation of the received header with the PN
    std::vector<std::complex<double>> b((size_t)L);
    re_.resize((size_t)rows_ + 8); im_.resize((size_t)rows_ + 8);
    double energy = 0;
    for (int t = 0; t < rows_; t++) { re_[(size_t)t] = r[t0_ + t].real(); im_[(size_t)t] = r[t0_ + t].imag(); energy += (double)re_[(size_t)t] * re_[(size_t)t] + (double)im_[(size_t)t] * im_[(size_t)t]; }
    for (int t = rows_; t < rows_ + 8; t++) { re_[(size_t)t] = im_[(size_t)t] = 0.f; }
    for (int mi = 0; mi < L; mi++) {
        const int m = mi - pre_;
        float sr, si;
        correlate(re_.data(), im_.data(), &chipf_[(size_t)(t0_ - m)], rows_, sr, si);
        b[(size_t)mi] = std::complex<double>(sr, si);
    }
    std::vector<std::complex<double>> g((size_t)L);
    if (hi.cyclic()) {
        // normal matrix (P + 1) I - 1 1^T over a full period
        std::complex<double> sum = 0;
        for (auto& v : b) sum += v;
        const double P = hi.core;
        for (int i = 0; i < L; i++) g[(size_t)i] = (b[(size_t)i] + sum / (P + 1.0 - L)) / (P + 1.0);
    } else {
        for (int i = 0; i < L; i++) {
            std::complex<double> acc = 0;
            for (int j = 0; j < L; j++) acc += minv_[(size_t)i * (size_t)L + (size_t)j] * b[(size_t)j];
            g[(size_t)i] = acc;
        }
    }
    // residual energy of the fit
    double fit = 0;
    for (int i = 0; i < L; i++) fit += (std::conj(g[(size_t)i]) * b[(size_t)i]).real();
    const double resid = std::max(0.0, energy - fit);
    out.pre = pre_; out.post = post_;
    out.noise = (float)(resid / std::max(1, rows_ - L));
    out.tapVar = out.noise / (float)(rows_ * headerInfo(h_).powerRatio);
    out.v.resize((size_t)L);
    // r = sum g chip with g = h * amp (1 + j): divide the header symbol out
    const std::complex<double> inv = 1.0 / (amp_ * std::complex<double>(1.0, 1.0));
    for (int i = 0; i < L; i++) out.v[(size_t)i] = cf32(g[(size_t)i] * inv);
    // variance of a tap before the division is about noise / rows
    out.energy = 0; out.peakPower = 0; out.peakIndex = 0;
    for (int i = 0; i < L; i++) {
        const float p = std::norm(out.v[(size_t)i]);
        out.energy += p;
        if (p > out.peakPower) { out.peakPower = p; out.peakIndex = i - pre_; }
    }
}

void HeaderEstimator::clean(Taps& t, float sigmaTap, float threshold) {
    const float lim = threshold * sigmaTap;
    t.energy = 0; t.peakPower = 0; t.peakIndex = 0; t.peakFrac = 0;
    const int L = (int)t.v.size();
    for (int i = 0; i < L; i++) {
        float p = std::norm(t.v[(size_t)i]);
        if (p < lim) { t.v[(size_t)i] = cf32(0, 0); p = 0; }
        t.energy += p;
        if (p > t.peakPower) { t.peakPower = p; t.peakIndex = i - t.pre; }
    }
    // fractional position of the strongest tap by a parabola through its power and its neighbours
    const int ip = t.peakIndex + t.pre;
    if (t.peakPower > 0 && ip > 0 && ip + 1 < L) {
        const float a = std::sqrt(std::norm(t.v[(size_t)ip - 1])), b = std::sqrt(t.peakPower), c = std::sqrt(std::norm(t.v[(size_t)ip + 1]));
        const float den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12f) t.peakFrac = std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / den));
    }
}

// ---------------------------------------------------------------- body
BodyEqualizer::BodyEqualizer() : fft_(kBody) {
    z_.resize((size_t)kBody + 2048);
    y_.resize((size_t)kBody);
    hh_.resize((size_t)kBody);
    resp_.resize((size_t)kBody);
}

// z[t' + pre] for t' = -pre .. N + post - 1: the received samples of the body (r points at t' = 0) with the leakage of the header before it and of
// the header after it taken out, using the channel estimates of those two headers
static void removeLeakage(const cf32* r, const Taps& ha, const int8_t* chipsA, const Taps& hb, const int8_t* chipsB, int Lh, double amp, int N, cf32* z) {
    const int pre = ha.pre, post = ha.post;
    const int span = N + post + pre;
    for (int i = 0; i < span; i++) z[i] = r[i - pre];
    const cf32 pa = cf32((float)amp, (float)amp);
    // header before the body: it ends at t' = 0, its chip n sits at t' = n - Lh; a tap at lag m puts chip n at t' = n - Lh + m
    for (int mi = 0; mi < (int)ha.v.size(); mi++) {
        const cf32 h = ha.v[(size_t)mi];
        if (h == cf32(0, 0)) continue;
        const int m = mi - ha.pre;
        const cf32 hv = h * pa;
        for (int tp = -pre; tp < post; tp++) {
            const int n = tp + Lh - m;
            if (n < 0 || n >= Lh) continue;
            z[tp + pre] -= hv * (float)chipsA[n];
        }
    }
    // header after the body: chip n sits at t' = N + n + m
    for (int mi = 0; mi < (int)hb.v.size(); mi++) {
        const cf32 h = hb.v[(size_t)mi];
        if (h == cf32(0, 0)) continue;
        const int m = mi - hb.pre;
        const cf32 hv = h * pa;
        for (int tp = N - pre; tp < N + post; tp++) {
            const int n = tp - N - m;
            if (n < 0 || n >= Lh) continue;
            z[tp + pre] -= hv * (float)chipsB[n];
        }
    }
}

// Variance of the error that a channel change between the two headers puts on the body (see BodyEqualizer::run)
static float headerDifference(const Taps& ha, const Taps& hb) {
    float diff = 0.f;
    const int lo = -std::max(ha.pre, hb.pre), hi = std::max(ha.post, hb.post);
    for (int m = lo; m < hi; m++) diff += std::norm(ha.at(m) - hb.at(m));
    return diff;
}

void BodyEqualizer::run(const cf32* r, const Taps& ha, const int8_t* chipsA, const Taps& hb, const int8_t* chipsB, int Lh, double amp, cf32* bins, float* var) {
    const int N = kBody;
    const int pre = ha.pre, post = ha.post;
    // z[t'] = r - leakage of both headers, for t' = -pre .. N + post - 1 (stored at z[t' + pre])
    removeLeakage(r, ha, chipsA, hb, chipsB, Lh, amp, N, z_.data());
    // circular convolution: the tail beyond the body wraps to its start, the leakage before it to its end
    for (int t = 0; t < N; t++) y_[(size_t)t] = z_[(size_t)(t + pre)];
    for (int t = 0; t < post; t++) y_[(size_t)t] += z_[(size_t)(N + t + pre)];
    for (int t = N - pre; t < N; t++) y_[(size_t)t] += z_[(size_t)(t - N + pre)];
    const float norm = 1.f / std::sqrt((float)N);
    fft_.forward(y_.data());
    for (auto& v : y_) v *= norm;
    // frequency response: the mean of the two header estimates, unit gain for a single tap at lag 0
    std::fill(hh_.begin(), hh_.end(), cf32(0, 0));
    for (int mi = 0; mi < (int)ha.v.size(); mi++) { const int m = mi - ha.pre; hh_[(size_t)((m + N) % N)] += 0.5f * ha.v[(size_t)mi]; }
    for (int mi = 0; mi < (int)hb.v.size(); mi++) { const int m = mi - hb.pre; hh_[(size_t)((m + N) % N)] += 0.5f * hb.v[(size_t)mi]; }
    fft_.forward(hh_.data());
    for (int k = 0; k < N; k++) resp_[(size_t)k] = hh_[(size_t)k];
    // The body is equalised with the mean of the two responses; if the channel (or the level, or the carrier phase) moved between the headers, the
    // difference is an error of the carriers that no noise estimate sees. A body with a level step in the middle would otherwise come out as
    // confidently wrong symbols, and the symbol interleaver spreads those over about 170 frames of codewords. Half the difference is the worst
    // error, a quarter of its energy the variance of a linear change.
    const float diff = headerDifference(ha, hb);
    const float noise = 0.5f * (ha.noise + hb.noise) + 0.25f * diff;
    for (int k = 0; k < N; k++) {
        const float p = std::norm(hh_[(size_t)k]);
        const float den = std::max(p, 1e-9f);
        bins[k] = y_[(size_t)k] * std::conj(hh_[(size_t)k]) / den;
        var[k] = noise / den;
    }
}

// ---------------------------------------------------------------- single carrier body
SingleCarrierEqualizer::SingleCarrierEqualizer() : fft_(kFft) {
    z_.resize((size_t)kBody + 1024);
    y_.resize((size_t)kFft);
    hh_.resize((size_t)kFft);
}

// After the leakage of the headers is out, the samples of the body window are the plain (linear) convolution of the 3780 symbols with the channel,
// which is 3780 + pre + post - 1 long: it fits a 4096 point circular convolution without wrapping, so the channel can be undone in the frequency
// domain with the symbols zero padded. The filter is the minimum mean square error one; its output is scaled to unit gain.
void SingleCarrierEqualizer::run(const cf32* r, const Taps& ha, const int8_t* chipsA, const Taps& hb, const int8_t* chipsB, int Lh, double amp, cf32* sym, float* var) {
    const int N = kBody, P = kFft;
    const int pre = ha.pre, post = ha.post;
    removeLeakage(r, ha, chipsA, hb, chipsB, Lh, amp, N, z_.data());
    const int span = N + pre + post - 1;
    std::fill(y_.begin(), y_.end(), cf32(0, 0));
    for (int i = 0; i < span; i++) y_[(size_t)((i - pre + P) % P)] = z_[(size_t)i];   // sample t' sits at t' mod P
    fft_.forward(y_.data());
    std::fill(hh_.begin(), hh_.end(), cf32(0, 0));
    for (int mi = 0; mi < (int)ha.v.size(); mi++) { const int m = mi - ha.pre; hh_[(size_t)((m + P) % P)] += 0.5f * ha.v[(size_t)mi]; }
    for (int mi = 0; mi < (int)hb.v.size(); mi++) { const int m = mi - hb.pre; hh_[(size_t)((m + P) % P)] += 0.5f * hb.v[(size_t)mi]; }
    fft_.forward(hh_.data());
    // noise per sample (header fit, plus what a change of the channel between the headers does), per bin of the symbol energy: lambda
    const float noise = 0.5f * (ha.noise + hb.noise) + 0.25f * headerDifference(ha, hb);
    const float lambda = std::max(noise * (float)span / (float)N, 1e-9f);
    double gSum = 0;
    for (int k = 0; k < P; k++) {
        const float p = std::norm(hh_[(size_t)k]);
        gSum += p / (p + lambda);
        y_[(size_t)k] *= std::conj(hh_[(size_t)k]) / (p + lambda);
    }
    const float g = std::max(1e-3f, (float)(gSum / P));
    fft_.inverse(y_.data());
    const float scale = 1.f / ((float)P * g);
    // unbiased output: the error is the MMSE (1 - g) in units of the biased output, so (1 - g) / g after scaling
    const float v = std::max(1e-9f, (1.f - g) / g);
    for (int t = 0; t < N; t++) { sym[t] = y_[(size_t)t] * scale; var[t] = v; }
}

// ---------------------------------------------------------------- system information
void siScores(const cf32* si, const float* w, float* scores) {
    float v[kSiSymbols], sw = 0;
    const float r2 = 1.f / std::sqrt(2.f);
    for (int s = 0; s < kSiSymbols; s++) { v[s] = (si[s].real() + si[s].imag()) * r2; sw += w[s]; }
    // the unit points are (+-1 +-j) / sqrt 2: the projection on (1 + j) / sqrt 2 is +-1
    uint8_t chips[kSiSymbols];
    for (int i = 3; i <= 24; i++) {
        siChips(i, chips);
        float acc = 0;
        for (int s = 0; s < kSiSymbols; s++) acc += w[s] * v[s] * (chips[s] ? 1.f : -1.f);
        scores[i - 3] = sw > 0 ? acc / sw : 0.f;
    }
}

} // namespace dect2::dtmb
