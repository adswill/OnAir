#include "dect2/source/gain_meter.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>

namespace dect2::ag {

GainMeter::GainMeter() {
    win_.resize(kFft);
    double s2 = 0;
    for (int i = 0; i < kFft; i++) { win_[(size_t)i] = 0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)kFft); s2 += (double)win_[(size_t)i] * win_[(size_t)i]; }
    // power per bin = |X|^2 / (N * sum w^2): white noise of power P reads P / N per bin, all bins together read the signal's power
    winNorm_ = (float)(1.0 / ((double)kFft * s2));
    re_.resize(kFft); im_.resize(kFft); specAcc_.assign(kFft, 0.f); buf_.resize(kFft);
    configure(2.4e6, 8);
}

void GainMeter::configure(double rateHz, int adcBits) {
    rate_ = rateHz > 0 ? rateHz : 1e6;
    bits_ = adcBits;
    wTarget_ = std::max<size_t>(256, (size_t)(rate_ * 0.05));   // 50 ms windows
    specEvery_ = std::max<size_t>(kFft, (size_t)(rate_ * 0.1)); // one transform per 100 ms
    reset();
}

void GainMeter::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    wPeak_ = 0; wClip_ = wN_ = 0; wPow_ = 0; std::fill(std::begin(wHist_), std::end(wHist_), 0u);
    hold_ = 0; holdAge_ = 0; bufFill_ = 0; specSinceFft_ = 0;
    pPeak_ = pHold_ = 0; pClip_ = pN_ = 0; pPow_ = 0; std::fill(std::begin(pHist_), std::end(pHist_), 0u);
    pSpecPow_.assign(kFft, 0.f); pSpecN_ = 0; lastSpec_.clear();
}

void GainMeter::process(const std::complex<float>* x, size_t n) {
    constexpr float kClip = 0.995f;
    for (size_t i = 0; i < n; i++) {
        const float re = std::fabs(x[i].real()), im = std::fabs(x[i].imag());
        const float m = std::max(re, im);
        if (m > wPeak_) wPeak_ = m;
        if (m >= kClip) wClip_++;
        wPow_ += (double)re * re + (double)im * im;
        wHist_[std::min(kHist - 1, (int)(re * (float)kHist))]++;
        wHist_[std::min(kHist - 1, (int)(im * (float)kHist))]++;
        wN_++;
        // the 10 Hz spectrum: a contiguous kFft block every specEvery_ samples
        if (bufFill_ < (size_t)kFft) {
            if (++specSinceFft_ >= specEvery_ || bufFill_ > 0) buf_[bufFill_++] = x[i];
            if (bufFill_ == (size_t)kFft) { specSinceFft_ = 0; spectrumBlock(); bufFill_ = 0; }
        }
        if (wN_ >= wTarget_) { window(); }
    }
    total_.fetch_add(n, std::memory_order_relaxed);
}

void GainMeter::spectrumBlock() {
    for (int i = 0; i < kFft; i++) { re_[(size_t)i] = buf_[(size_t)i].real() * win_[(size_t)i]; im_[(size_t)i] = buf_[(size_t)i].imag() * win_[(size_t)i]; }
    fftSplit(re_.data(), im_.data(), 11, false);
    std::lock_guard<std::mutex> lk(mu_);
    if (pSpecPow_.size() != (size_t)kFft) pSpecPow_.assign(kFft, 0.f);
    for (int k = 0; k < kFft; k++) {
        const int src = (k + kFft / 2) % kFft;   // DC-centred
        pSpecPow_[(size_t)k] += (re_[(size_t)src] * re_[(size_t)src] + im_[(size_t)src] * im_[(size_t)src]) * winNorm_;
    }
    pSpecN_++;
}

void GainMeter::window() {
    // the peak-hold: a 2 s tail (a window is 50 ms: 40 windows)
    if (wPeak_ >= hold_) { hold_ = wPeak_; holdAge_ = 0; }
    else if (++holdAge_ > 40) { hold_ = wPeak_; holdAge_ = 0; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        pPeak_ = std::max(pPeak_, wPeak_);
        pHold_ = std::max(pHold_, hold_);
        pClip_ += wClip_; pN_ += wN_; pPow_ += wPow_;
        for (int k = 0; k < kHist; k++) pHist_[k] += wHist_[k];
    }
    wPeak_ = 0; wClip_ = wN_ = 0; wPow_ = 0; std::fill(std::begin(wHist_), std::end(wHist_), 0u);
}

static BlockStats makeStats(float peak, float hold, uint64_t clip, uint64_t n, double pow, const uint32_t* hist) {
    BlockStats b;
    b.n = (size_t)n;
    if (!n) return b;
    b.peak = std::min(1.0f, peak);
    b.rmsDbfs = 10.0 * std::log10(std::max(pow / (double)n, 1e-12));
    b.clipFraction = (double)clip / (double)n;
    // the robust peak: 99.99 % of |I| and |Q| (the old one was 99.9 % of every fourth sample)
    const double want = 0.9999 * 2.0 * (double)n;
    double cum = 0;
    for (int k = 0; k < GainMeter::kHist; k++) { cum += hist[k]; if (cum >= want) { b.p999 = (k + 1.0) / GainMeter::kHist; break; } }
    if (b.p999 <= 0) b.p999 = b.peak;
    b.p999 = std::min(b.p999, b.peak > 0 ? (double)b.peak : 1.0);
    b.crestDb = 20 * std::log10(std::max(b.p999, 1e-9)) - b.rmsDbfs;
    b.headroomDb = -20 * std::log10(std::max(b.p999, 1e-9));
    b.holdHeadroomDb = holdHeadroomDb(hold);
    return b;
}

bool GainMeter::take(BlockStats& adc, std::vector<float>& spec) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!pN_) return false;
    adc = makeStats(pPeak_, pHold_, pClip_, pN_, pPow_, pHist_);
    if (pSpecN_) {
        spec.resize((size_t)kFft);
        for (int k = 0; k < kFft; k++) spec[(size_t)k] = 10.f * std::log10(std::max(pSpecPow_[(size_t)k] / (float)pSpecN_, 1e-14f));
        lastSpec_ = spec;
    } else if (!lastSpec_.empty()) spec = lastSpec_;
    else spec.clear();
    pPeak_ = pHold_ = 0; pClip_ = pN_ = 0; pPow_ = 0; std::fill(std::begin(pHist_), std::end(pHist_), 0u);
    std::fill(pSpecPow_.begin(), pSpecPow_.end(), 0.f); pSpecN_ = 0;
    return true;
}

BlockStats GainMeter::peek() const {
    std::lock_guard<std::mutex> lk(mu_);
    return makeStats(pPeak_, pHold_, pClip_, pN_, pPow_, pHist_);
}

} // namespace dect2::ag
