#include "dect2/engine/spantv/span_analyzer.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

void SpanAnalyzer::configure(double spanRate, int bins, int takeEvery) {
    rate_ = spanRate;
    int n = 1024;
    while (n < bins && n < 65536) n <<= 1;
    bins_ = n;
    take_ = std::max(1, takeEvery);
    win_.resize((size_t)n);
    for (int i = 0; i < n; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / n));
    fft_ = std::make_shared<Fft>(n);
    reset();
}

void SpanAnalyzer::reset() {
    acc_.assign((size_t)bins_, 0.f);
    blk_.assign((size_t)bins_, cf32(0, 0));
    fill_ = 0; nFrames_ = 0; blocks_ = 0;
    floor_.clear();
}

void SpanAnalyzer::feed(const cf32* x, size_t n) {
    if (!fft_) return;
    const size_t N = (size_t)bins_;
    size_t i = 0;
    while (i < n) {
        const bool take = (blocks_ % (size_t)take_) == 0;
        const size_t m = std::min(n - i, N - fill_);
        if (take) std::copy(x + i, x + i + m, blk_.begin() + (long)fill_);
        fill_ += m; i += m;
        if (fill_ < N) continue;
        fill_ = 0; blocks_++;
        if (!take) continue;
        buf_.resize(N);
        for (size_t k = 0; k < N; k++) buf_[k] = blk_[k] * win_[k];
        fft_->forward(buf_.data());
        // exponential average over about 32 frames (the first ones weigh equally)
        const float a = nFrames_ < 32 ? 1.f / (float)(nFrames_ + 1) : 1.f / 32.f;
        const float norm = 1.f / ((float)N * (float)N * 0.375f);
        for (size_t k = 0; k < N; k++) {
            const size_t j = (k + N / 2) % N;   // DC in the middle
            acc_[k] += a * (std::norm(buf_[j]) * norm - acc_[k]);
        }
        nFrames_++;
    }
}

std::vector<float> SpanAnalyzer::spectrumDb() const {
    std::vector<float> v(acc_.size());
    for (size_t k = 0; k < acc_.size(); k++) v[k] = 10.f * std::log10(std::max(acc_[k], 1e-20f));
    return v;
}

double SpanAnalyzer::snapRollOff(double a) {
    if (!(a > 0) || a > 0.7) return 0;
    static const double k[] = {0.05, 0.10, 0.15, 0.20, 0.25, 0.35};
    double best = k[0];
    for (double v : k) if (std::fabs(v - a) < std::fabs(best - a)) best = v;
    return best;
}

std::vector<SignalCand> SpanAnalyzer::detect(const Params& p) const {
    std::vector<SignalCand> out;
    const int N = bins_;
    if (!nFrames_ || N <= 0 || rate_ <= 0) return out;
    const double bin = rate_ / N;
    const int lo = std::max(0, (int)std::lround(N / 2 - p.edgeFrac * N)), hi = std::min(N, (int)std::lround(N / 2 + p.edgeFrac * N));
    if (hi - lo < 64) return out;
    // smoothing across frequency (linear power): a box of about an eighth of the narrowest signal
    const int ws = std::max(1, std::min(N / 64, (int)std::lround(p.minBwHz / bin / 8.0))) | 1;
    std::vector<double> sm((size_t)N, 0.0);
    {
        std::vector<double> cum((size_t)N + 1, 0.0);
        for (int k = 0; k < N; k++) cum[(size_t)k + 1] = cum[(size_t)k] + acc_[(size_t)k];
        for (int k = 0; k < N; k++) {
            const int a = std::max(0, k - ws / 2), b = std::min(N, k + ws / 2 + 1);
            sm[(size_t)k] = (cum[(size_t)b] - cum[(size_t)a]) / (b - a);
        }
    }
    auto db = [](double v) { return 10.0 * std::log10(std::max(v, 1e-30)); };
    // the floor: the 10th percentile of each segment, a line fitted through them, segments well above the line (signals) dropped one by one
    const int seg = std::max(16, (hi - lo) / 32), M = std::max(1, (hi - lo) / seg);
    std::vector<double> xs, ys;
    for (int s = 0; s < M; s++) {
        const int a = lo + s * seg, b = s == M - 1 ? hi : a + seg;
        std::vector<double> v;
        for (int k = a; k < b; k++) v.push_back(db(sm[(size_t)k]));
        std::nth_element(v.begin(), v.begin() + (long)(v.size() / 10), v.end());
        xs.push_back(0.5 * (a + b)); ys.push_back(v[v.size() / 10]);
    }
    std::vector<bool> act(xs.size(), true);
    double fa = 0, fb = 0;
    for (;;) {
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < xs.size(); i++) if (act[i]) { n++; sx += xs[i]; sy += ys[i]; sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; }
        const double den = n * sxx - sx * sx;
        fb = n >= 3 && std::fabs(den) > 1e-9 ? (n * sxy - sx * sy) / den : 0;
        fa = n > 0 ? (sy - fb * sx) / n : -200;
        // the floor cannot tilt more than 20 dB across the span
        const double maxSlope = 20.0 / (hi - lo);
        if (std::fabs(fb) > maxSlope) { fb = fb > 0 ? maxSlope : -maxSlope; double s = 0; for (size_t i = 0; i < xs.size(); i++) if (act[i]) s += ys[i] - fb * xs[i]; fa = s / n; }
        size_t worst = SIZE_MAX;
        double wr = 1.5;
        for (size_t i = 0; i < xs.size(); i++) if (act[i]) { const double r = ys[i] - (fa + fb * xs[i]); if (r > wr) { wr = r; worst = i; } }
        if (worst == SIZE_MAX || n <= std::max(3.0, (double)xs.size() / 6)) break;
        act[worst] = false;
    }
    floor_.assign((size_t)N, 0.f);
    for (int k = 0; k < N; k++) floor_[(size_t)k] = (float)(fa + fb * k);
    // runs above floor + threshold, ending 2 dB lower
    struct Run { int s, e; };
    std::vector<Run> runs;
    {
        bool in = false;
        int s = 0;
        for (int k = lo; k < hi; k++) {
            const double above = db(sm[(size_t)k]) - floor_[(size_t)k];
            if (!in && above >= p.thresholdDb) { in = true; s = k; }
            else if (in && above < p.thresholdDb - 2) { in = false; runs.push_back({s, k}); }
        }
        if (in) runs.push_back({s, hi});
    }
    std::vector<Run> merged;
    for (const Run& r : runs) {
        if (!merged.empty() && r.s - merged.back().e < std::max(2, ws)) merged.back().e = r.e;
        else merged.push_back(r);
    }
    for (const Run& r : merged) {
        if (r.e - r.s < 4 || (r.e - r.s) * bin < 0.5 * p.minBwHz) continue;   // a tone or a spur
        SignalCand c;
        c.clipped = r.s <= lo + 1 || r.e >= hi - 1;
        // plateau: median of the centre half
        {
            const int q = (r.e - r.s) / 4;
            std::vector<double> v;
            for (int k = r.s + q; k < r.e - q; k++) v.push_back(db(sm[(size_t)k]));
            if (v.empty()) continue;
            std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
            c.plateauDb = v[v.size() / 2];
        }
        const int mid = (r.s + r.e) / 2;
        c.floorDb = floor_[(size_t)mid];
        c.snrDb = c.plateauDb - c.floorDb;
        const double P = std::pow(10.0, c.plateauDb / 10) - std::pow(10.0, c.floorDb / 10);
        if (P <= 0) continue;
        auto sig = [&](int k) { return k < 0 || k >= N ? 0.0 : sm[(size_t)k] - std::pow(10.0, floor_[(size_t)k] / 10); };
        // the edge at a fraction of the plateau's power, walking inward from outside the run (linear interpolation between bins)
        const int margin = std::max(4, 2 * ws);
        auto edgeL = [&](double frac) {
            const double t = frac * P;
            for (int k = std::max(0, r.s - margin); k <= mid; k++) {
                const double v1 = sig(k);
                if (v1 >= t) { const double v0 = sig(k - 1); return v1 > v0 ? (k - 1) + (t - v0) / (v1 - v0) : (double)k; }
            }
            return (double)mid;
        };
        auto edgeR = [&](double frac) {
            const double t = frac * P;
            for (int k = std::min(N - 1, r.e + margin); k >= mid; k--) {
                const double v1 = sig(k);
                if (v1 >= t) { const double v0 = sig(k + 1); return v1 > v0 ? (k + 1) - (t - v0) / (v1 - v0) : (double)k; }
            }
            return (double)mid;
        };
        const double l3 = edgeL(0.5), r3 = edgeR(0.5);
        c.bw3Hz = (r3 - l3) * bin;
        c.centerHz = (0.5 * (l3 + r3) - N / 2) * bin;
        const double l9 = edgeL(0.9), r9 = edgeR(0.9), l1 = edgeL(0.1), r1 = edgeR(0.1);
        c.edgeHz = 0.5 * ((l9 - l1) + (r1 - r9)) * bin;
        c.rsEstimate = c.bw3Hz;
        // a raised cosine shoulder from 90 % to 10 % is 0.5904 alpha Rs wide; the smoothing widens it by about the box
        const double shoulder = std::max(0.0, c.edgeHz - 0.5 * ws * bin);
        c.rollOff = c.snrDb >= 10 && c.bw3Hz > 0 ? snapRollOff(shoulder / (0.5904 * c.bw3Hz)) : 0;
        c.bwHz = c.rollOff > 0 ? c.bw3Hz * (1 + c.rollOff) : (r1 - l1) * bin;
        if (c.bw3Hz < p.minBwHz || c.bw3Hz > p.maxBwHz) continue;
        out.push_back(c);
    }
    return out;
}

} // namespace dect2
