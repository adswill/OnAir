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
    // Armed by take(): nothing runs (and nothing costs) until a consumer has asked for a measurement in the last 3 s
    const uint64_t done = total_.load(std::memory_order_relaxed);
    total_.fetch_add(n, std::memory_order_relaxed);
    if (done >= wantUntil_.load(std::memory_order_relaxed)) { wN_ = 0; bufFill_ = 0; return; }
    constexpr float kClip = 0.995f;
    size_t i = 0;
    while (i < n) {
        // work in chunks that never straddle a window end, so the loops below stay simple (and vectorise)
        const size_t room = wTarget_ > wN_ ? wTarget_ - wN_ : 1;
        const size_t len = std::min(n - i, std::min<size_t>(room, 4096));
        float pk = wPeak_, pw = 0.f;
        uint64_t clip = 0;
        for (size_t k = 0; k < len; k++) {   // every sample: peak, clipped count, power
            const float re = std::fabs(x[i + k].real()), im = std::fabs(x[i + k].imag());
            const float m = std::max(re, im);
            pk = std::max(pk, m);
            clip += (m >= kClip);
            pw += re * re + im * im;
        }
        for (size_t k = 0; k < len; k += 4) {   // the 99.99 % point from every fourth sample (a statistic: 10^4 samples are enough)
            wHist_[std::min(kHist - 1, (int)(std::fabs(x[i + k].real()) * (float)kHist))]++;
            wHist_[std::min(kHist - 1, (int)(std::fabs(x[i + k].imag()) * (float)kHist))]++;
        }
        wPeak_ = pk; wClip_ += clip; wPow_ += pw; wN_ += len; wHistN_ += (len + 3) / 4;
        // the 10 Hz spectrum: a contiguous kFft block every specEvery_ samples
        size_t k = 0;
        if (bufFill_ == 0) {
            const size_t until = specEvery_ > specSinceFft_ ? specEvery_ - specSinceFft_ : 0;
            if (until >= len) { specSinceFft_ += len; k = len; }
            else { specSinceFft_ = specEvery_; k = until; }
        }
        for (; k < len; k++) {
            buf_[bufFill_++] = x[i + k];
            if (bufFill_ == (size_t)kFft) { spectrumBlock(); bufFill_ = 0; specSinceFft_ = len - 1 - k; break; }
        }
        i += len;
        if (wN_ >= wTarget_) window();
    }
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
        pHistN_ += wHistN_;
        pHold_ = std::max(pHold_, hold_);
        pClip_ += wClip_; pN_ += wN_; pPow_ += wPow_;
        for (int k = 0; k < kHist; k++) pHist_[k] += wHist_[k];
    }
    wPeak_ = 0; wClip_ = wN_ = 0; wPow_ = 0; wHistN_ = 0; std::fill(std::begin(wHist_), std::end(wHist_), 0u);
}

static BlockStats makeStats(float peak, float hold, uint64_t clip, uint64_t n, double pow, const uint32_t* hist, uint64_t histN) {
    BlockStats b;
    b.n = (size_t)n;
    if (!n) return b;
    b.peak = std::min(1.0f, peak);
    b.rmsDbfs = 10.0 * std::log10(std::max(pow / (double)n, 1e-12));
    b.clipFraction = (double)clip / (double)n;
    // the robust peak: 99.99 % of |I| and |Q| (the old one was 99.9 % of every fourth sample)
    const double want = 0.9999 * 2.0 * (double)histN;
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
    wantUntil_.store(total_.load(std::memory_order_relaxed) + (uint64_t)(3.0 * rate_), std::memory_order_relaxed);   // arm / keep armed for 3 s of samples
    std::lock_guard<std::mutex> lk(mu_);
    if (!pN_) return false;
    adc = makeStats(pPeak_, pHold_, pClip_, pN_, pPow_, pHist_, pHistN_);
    if (pSpecN_) {
        spec.resize((size_t)kFft);
        for (int k = 0; k < kFft; k++) spec[(size_t)k] = 10.f * std::log10(std::max(pSpecPow_[(size_t)k] / (float)pSpecN_, 1e-14f));
        lastSpec_ = spec;
    } else if (!lastSpec_.empty()) spec = lastSpec_;
    else spec.clear();
    pPeak_ = pHold_ = 0; pClip_ = pN_ = 0; pHistN_ = 0; pPow_ = 0; std::fill(std::begin(pHist_), std::end(pHist_), 0u);
    std::fill(pSpecPow_.begin(), pSpecPow_.end(), 0.f); pSpecN_ = 0;
    return true;
}

BlockStats GainMeter::peek() const {
    std::lock_guard<std::mutex> lk(mu_);
    return makeStats(pPeak_, pHold_, pClip_, pN_, pPow_, pHist_, pHistN_);
}

} // namespace dect2::ag
