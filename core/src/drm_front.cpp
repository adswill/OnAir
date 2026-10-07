// DRM receiver front end: decimation to 48 kHz.
#include "drm_front.h"
#include <algorithm>
#include <cmath>

namespace dect2 { namespace drm {

namespace {
double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}

// factors of d, none above 16 where that is possible, largest first
std::vector<int> splitFactors(int d) {
    std::vector<int> pr;
    int r = d;
    for (int p = 2; r > 1; p++) while (r % p == 0) { pr.push_back(p); r /= p; }
    std::sort(pr.begin(), pr.end(), std::greater<int>());
    std::vector<int> out;
    for (int p : pr) {
        bool merged = false;
        for (auto& o : out) if (o * p <= 16) { o *= p; merged = true; break; }
        if (!merged) out.push_back(p);
    }
    return out;
}
bool smooth(int d) { int r = d; for (int p : {2, 3, 5, 7, 11, 13}) while (r % p == 0) r /= p; return r == 1; }
}

void DecimFir::design(double passHz, double stopHz, double inRate, int d, double attenDb) {
    d_ = std::max(1, d);
    stopHz = std::min(stopHz, inRate * 0.5 * 0.999);
    const double fc = 0.5 * (passHz + stopHz) / inRate;
    const double df = std::max((stopHz - passHz) / inRate, 1e-5);
    int n = (int)std::ceil((attenDb - 7.95) / (14.36 * df)) + 1;
    n = std::min(4001, std::max(5, n)) | 1;
    const double beta = attenDb > 50 ? 0.1102 * (attenDb - 8.7) : 0.5842 * std::pow(attenDb - 21, 0.4) + 0.07886 * (attenDb - 21);
    h_.assign((size_t)n, 0.f);
    const int m = n / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double x = i - m;
        const double sinc = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
        const double r = x / (m + 0.5);
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
        h_[(size_t)i] = (float)(sinc * w);
        sum += h_[(size_t)i];
    }
    for (auto& v : h_) v = (float)(v / sum);
    reset();
}

void DecimFir::reset() { hist_.assign(h_.size() - 1, cf32(0, 0)); cnt_ = 0; }

void DecimFir::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    const size_t nt = h_.size();
    x_.assign(hist_.begin(), hist_.end());
    x_.insert(x_.end(), in, in + n);
    const size_t base = nt - 1;
    for (size_t j = (size_t)(d_ - cnt_ - 1); j < n; j += (size_t)d_) {
        const cf32* p = x_.data() + base + j;
        float ar = 0, ai = 0;
        for (size_t k = 0; k < nt; k++) { const cf32 v = p[-(ptrdiff_t)k]; ar += v.real() * h_[k]; ai += v.imag() * h_[k]; }
        out.push_back(cf32(ar, ai));
    }
    cnt_ = (int)((cnt_ + n) % (size_t)d_);
    hist_.assign(x_.end() - (ptrdiff_t)base, x_.end());
}

SroCorrector::SroCorrector() {
    bank_.assign((size_t)(kPhases + 1) * kTaps, 0.f);
    const double beta = 8.0;
    for (int p = 0; p <= kPhases; p++) {
        const double f = (double)p / kPhases;
        double sum = 0;
        for (int j = 0; j < kTaps; j++) {
            const double x = (double)j - (kTaps / 2 - 1) - f;          // distance from the wanted position in samples
            const double r = x / (kTaps / 2);
            const double w = std::fabs(r) < 1 ? besselI0(beta * std::sqrt(1 - r * r)) / besselI0(beta) : 0;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * 0.92 * x) / (M_PI * x);
            bank_[(size_t)p * kTaps + (size_t)j] = (float)(sinc * w);
            sum += sinc * w;
        }
        for (int j = 0; j < kTaps; j++) bank_[(size_t)p * kTaps + (size_t)j] = (float)(bank_[(size_t)p * kTaps + (size_t)j] / sum);
    }
    reset();
}
void SroCorrector::reset() { x_.assign((size_t)kTaps, cf32(0, 0)); pos_ = kTaps / 2 - 1; }

void SroCorrector::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    // always through the filter, so that the delay of the stream never steps when the correction starts
    x_.insert(x_.end(), in, in + n);
    const double step = 1.0 + ppm_ * 1e-6;
    const size_t need = (size_t)kTaps / 2 + 1;
    while ((size_t)pos_ + need < x_.size()) {
        const size_t i0 = (size_t)pos_;
        const double fr = (pos_ - (double)i0) * kPhases;
        const size_t ph = (size_t)fr;
        const float a = (float)(fr - (double)ph);
        const float* h0 = &bank_[ph * kTaps];
        const float* h1 = &bank_[(ph + 1) * kTaps];
        const cf32* xs = &x_[i0 - (kTaps / 2 - 1)];
        float re = 0, im = 0;
        for (int j = 0; j < kTaps; j++) {
            const float h = h0[j] + a * (h1[j] - h0[j]);
            re += xs[j].real() * h; im += xs[j].imag() * h;
        }
        out.push_back(cf32(re, im));
        pos_ += step;
    }
    const size_t keepFrom = (size_t)pos_ - (kTaps / 2 - 1);
    x_.erase(x_.begin(), x_.begin() + (ptrdiff_t)keepFrom);
    pos_ -= (double)keepFrom;
}

bool DrmFront::configure(double inRate) {
    inRate_ = inRate;
    ready_ = false; pass_ = false; useRs_ = false;
    stages_.clear();
    if (inRate < kOutRate * 0.999) return false;
    dcA_ = (float)(1.0 / (kOutRate * 0.3));
    if (std::fabs(inRate - kOutRate) < 0.5) { pass_ = true; ready_ = true; reset(); return true; }
    // integer decimation to a rate of 192 kHz or more, then the exact resampler
    int D = (int)std::floor(inRate / 192000.0);
    while (D > 1 && !smooth(D)) D--;
    double r = inRate;
    if (D > 1) {
        for (int d : splitFactors(D)) {
            DecimFir f;
            const double out = r / d;
            f.design(22000.0, out - 26000.0, r, d);
            stages_.push_back(std::move(f));
            r = out;
        }
    }
    if (!rs_.configure(r, kOutRate)) return false;
    useRs_ = true;
    ready_ = true;
    reset();
    return true;
}

void DrmFront::reset() {
    for (auto& s : stages_) s.reset();
    if (useRs_) rs_.reset();
    dc_ = cf32(0, 0);
    pw_ = 0; pn_ = 0;
}

void DrmFront::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (!ready_ || n == 0) return;
    const size_t start = out.size();
    if (pass_) {
        out.insert(out.end(), in, in + n);
    } else {
        const cf32* cur = in; size_t cn = n;
        for (size_t s = 0; s < stages_.size(); s++) {
            std::vector<cf32>& dst = (s & 1) ? b_ : a_;
            dst.clear();
            stages_[s].process(cur, cn, dst);
            cur = dst.data(); cn = dst.size();
        }
        rs_.process(cur, cn, out);
    }
    // DC removal (the radio's DC spike) with a time constant of 0.3 s, and the level
    for (size_t i = start; i < out.size(); i++) {
        dc_ += (out[i] - dc_) * dcA_;
        out[i] -= dc_;
        pw_ += std::norm(out[i]);
    }
    pn_ += out.size() - start;
}

double DrmFront::levelDbfs() const { return pn_ ? 10 * std::log10(std::max(pw_ / (double)pn_, 1e-12)) : -120.0; }

}} // namespace dect2::drm
