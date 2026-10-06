#include "dect2/spectrum.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include <mutex>

namespace dect2 {

constexpr size_t kMaxBlocksPerFrame = 32;

struct SpectrumAnalyzer::Impl {
    int log2n = 0;
    std::vector<float> window, re, im, accum;
    std::vector<cf32> pending; // partial block carried across feed() calls
    size_t blocks = 0;
    // stats accumulators
    double sumSq = 0, sumI = 0, sumQ = 0;
    float peak = 0;
    uint64_t count = 0, clip = 0;
    uint32_t hist[64] = {};
    uint64_t seq = 0;
    std::atomic<bool> transform{true};
    std::mutex mu;   // the worker thread feeds, the analysis thread takes frames and resets
};

SpectrumAnalyzer::SpectrumAnalyzer(size_t fftSize) : p_(new Impl), n_(fftSize) {
    p_->log2n = (int)std::lround(std::log2((double)n_));
    p_->window.resize(n_);
    hannWindowNorm(p_->window.data(), (int)n_);
    p_->re.resize(n_);
    p_->im.resize(n_);
    p_->accum.assign(n_, 0.f);
}

SpectrumAnalyzer::~SpectrumAnalyzer() {
    delete p_;
}

void SpectrumAnalyzer::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    // Power, DC and peak over every sample, in eight float lanes per piece of 4096 samples (summed into doubles between pieces).
    // The ADC histogram and the clip count look at every fourth sample, each counted four times: they are display statistics.
    for (size_t p0 = 0; p0 < n; p0 += 4096) {
        const size_t pn = std::min<size_t>(4096, n - p0), p8 = pn & ~(size_t)7;
        const cf32* y = x + p0;
        float sq[8] = {}, si[8] = {}, sqq[8] = {}, pk[8] = {};
        for (size_t i = 0; i < p8; i += 8)
            for (int j = 0; j < 8; j++) {
                const float I = y[i + j].real(), Q = y[i + j].imag();
                sq[j] += I * I + Q * Q; si[j] += I; sqq[j] += Q;
                pk[j] = std::max(pk[j], std::max(std::fabs(I), std::fabs(Q)));
            }
        for (int j = 0; j < 8; j++) { s.sumSq += sq[j]; s.sumI += si[j]; s.sumQ += sqq[j]; s.peak = std::max(s.peak, pk[j]); }
        for (size_t i = p8; i < pn; i++) {
            const float I = y[i].real(), Q = y[i].imag();
            s.sumSq += (double)I * I + (double)Q * Q; s.sumI += I; s.sumQ += Q;
            s.peak = std::max(s.peak, std::max(std::fabs(I), std::fabs(Q)));
        }
        for (size_t i = 0; i < pn; i += 4) {
            const float I = y[i].real(), Q = y[i].imag();
            if (std::max(std::fabs(I), std::fabs(Q)) >= 126.0f / 128.0f) s.clip += 4;
            s.hist[(int)std::min(63.f, std::fabs(I) * 64)] += 4;   // clamp as a float: (int) of a huge value is INT_MIN on x86
            s.hist[(int)std::min(63.f, std::fabs(Q) * 64)] += 4;
        }
    }
    s.count += n;

    // display doesn't need every block
    if (!s.transform || s.blocks >= kMaxBlocksPerFrame) { s.pending.clear(); return; }
    s.pending.insert(s.pending.end(), x, x + n);
    size_t off = 0;
    while (s.pending.size() - off >= n_ && s.blocks < kMaxBlocksPerFrame) {
        const cf32* b = s.pending.data() + off;
        for (size_t i = 0; i < n_; i++) {
            s.re[i] = b[i].real() * s.window[i];
            s.im[i] = b[i].imag() * s.window[i];
        }
        fftSplit(s.re.data(), s.im.data(), s.log2n, false);
        for (size_t i = 0; i < n_; i++) s.accum[i] += s.re[i] * s.re[i] + s.im[i] * s.im[i];
        s.blocks++;
        off += n_;
    }
    s.pending.erase(s.pending.begin(), s.pending.begin() + off);
}

void SpectrumAnalyzer::setTransform(bool on) { p_->transform = on; }

void SpectrumAnalyzer::reset() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    std::fill(s.accum.begin(), s.accum.end(), 0.f);
    s.pending.clear();
    s.blocks = 0; s.sumSq = s.sumI = s.sumQ = 0; s.peak = 0; s.count = s.clip = 0;
    std::fill(std::begin(s.hist), std::end(s.hist), 0u);
}

bool SpectrumAnalyzer::takeFrame(SpectrumFrame& out) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.count == 0 || (s.blocks == 0 && s.transform)) return false;
    out.dbfs.resize(n_);
    // normalise so the sum over bins equals total power (dBFS/bin)
    float inv = 1.0f / ((float)std::max<size_t>(s.blocks, 1) * (float)n_ * (float)n_);
    for (size_t i = 0; i < n_; i++) {
        size_t src = (i + n_ / 2) % n_; // DC-centre
        out.dbfs[i] = 10.0f * std::log10(std::max(s.accum[src] * inv, 1e-14f));
    }
    out.stats.peak = s.peak;
    out.stats.rmsDbfs = 10.0f * std::log10(std::max(s.sumSq / (double)s.count, 1e-12));
    out.stats.clipFraction = (float)s.clip / (float)s.count;
    out.stats.dcI = (float)(s.sumI / s.count);
    out.stats.dcQ = (float)(s.sumQ / s.count);
    std::copy(std::begin(s.hist), std::end(s.hist), out.stats.hist);
    out.seq = ++s.seq;

    std::fill(s.accum.begin(), s.accum.end(), 0.f);
    s.blocks = 0; s.sumSq = s.sumI = s.sumQ = 0; s.peak = 0; s.count = s.clip = 0;
    std::fill(std::begin(s.hist), std::end(s.hist), 0u);
    return true;
}

} // namespace dect2
