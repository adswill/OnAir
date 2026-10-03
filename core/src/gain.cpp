#include "dect2/gain.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace dect2 {

GainSetting gainForTotal(int total) {
    total = std::max(0, std::min(total, 40 + 62 + 14));
    GainSetting g;
    g.amp = total >= 62;
    const int r = total - (g.amp ? 14 : 0);
    g.lna = std::max(0, std::min(40, (int)std::lround((r - 20) / 8.0) * 8));
    int v = r - g.lna;
    v = std::max(0, std::min(62, v));
    g.vga = v & ~1;
    return g;
}

GainSetting genericGain(int total, int maxDb) {
    GainSetting g;
    g.lna = 0; g.amp = false;
    g.vga = std::max(0, std::min(total, maxDb));
    return g;
}

AdcStatus classifyAdc(double rms, double peak, double clip) {
    if (clip > 0.002 || peak >= 0.99) return AdcStatus::Overload;
    if (rms < -50) return AdcStatus::NoSignal;
    if (rms < -22) return AdcStatus::Low;
    if (rms > -11) return AdcStatus::High;
    return AdcStatus::Good;
}

const char* adcStatusName(AdcStatus s) {
    switch (s) {
    case AdcStatus::NoSignal: return "no signal";
    case AdcStatus::Low: return "low";
    case AdcStatus::Good: return "good";
    case AdcStatus::High: return "high";
    case AdcStatus::Overload: return "OVERLOAD";
    }
    return "";
}

std::string adcAdvice(AdcStatus s) {
    switch (s) {
    case AdcStatus::NoSignal: return "ADC sees almost nothing: check the antenna, the frequency and the gains.";
    case AdcStatus::Low: return "ADC level is low (quantisation noise): raise the gain.";
    case AdcStatus::Good: return "ADC level is good.";
    case AdcStatus::High: return "ADC level is high: close to clipping, consider lowering the gain.";
    case AdcStatus::Overload: return "ADC is clipping (8-bit overload): reduce the gain, the decoder sees distorted samples.";
    }
    return "";
}

void AutoGain::reset() { init_ = false; changedAt_ = -1e9; last_ = -1; }

bool AutoGain::update(double now, const SignalStats& st, GainSetting& g) {
    if (!init_) { rms_ = st.rmsDbfs; peak_ = st.peak; clip_ = st.clipFraction; init_ = true; last_ = now; }
    const double dt = std::max(0.0, now - last_);
    last_ = now;
    const double k = 1.0 - std::exp(-dt / 0.35);
    rms_ += k * (st.rmsDbfs - rms_);
    // overload is judged on the worst of the recent history: let it rise fast and decay slowly
    peak_ = st.peak > peak_ ? st.peak : peak_ + k * (st.peak - peak_);
    clip_ = st.clipFraction > clip_ ? st.clipFraction : clip_ + k * (st.clipFraction - clip_);
    if (now - changedAt_ < cfg_.settleSec) return false;

    double delta = 0;
    if (clip_ > 0.002 || peak_ >= 0.99) delta = -6;
    else if (rms_ > cfg_.highDbfs) delta = std::max(-cfg_.maxStepDb, cfg_.targetDbfs - rms_);
    else if (rms_ < cfg_.lowDbfs && peak_ < 0.85 && (rms_ > -70 || g.total() < (cfg_.genericMaxDb > 0 ? cfg_.genericMaxDb * 6 / 10 : 62))) // an empty ADC at low gain: climb, but not blindly to the maximum
        delta = std::min(cfg_.maxStepDb, std::max(4.0, cfg_.targetDbfs - rms_));
    if (std::fabs(delta) < 1.0) return false;

    const int tot = g.total();
    int step = (int)std::lround(delta / 2.0) * 2;
    if (step == 0) step = delta > 0 ? 2 : -2;
    for (int tries = 0; tries < 6; tries++, step += delta > 0 ? 2 : -2) {
        GainSetting n = cfg_.genericMaxDb > 0 ? genericGain(tot + step, cfg_.genericMaxDb) : gainForTotal(tot + step);
        if (!(n == g) && (delta > 0 ? n.total() > tot : n.total() < tot)) {
            g = n;
            changedAt_ = now;
            // judge the new level from scratch
            init_ = false;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ sweep
void GainSweep::start(double now, int genericMaxDb) {
    e_.clear();
    if (genericMaxDb > 0) {
        for (int pct : {20, 32, 44, 56, 68, 80}) { Entry e; e.g = genericGain(genericMaxDb * pct / 100, genericMaxDb); e_.push_back(e); }
    } else {
    for (int lna : {24, 32, 40})
        for (int vga : {12, 20, 28}) { Entry e; e.g = {lna, vga, true}; e_.push_back(e); }
    for (int lna : {32, 40})
        for (int vga : {20, 32}) { Entry e; e.g = {lna, vga, false}; e_.push_back(e); }
    }
    idx_ = -1;
    active_ = true;
    t0_ = now;
    best_ = GainSetting();
}

void GainSweep::finish(GainSetting& g) {
    // best SNR among candidates that do not clip; without any lock fall back to the level closest to -16 dBFS
    int bi = -1;
    double bs = -1e9;
    for (size_t i = 0; i < e_.size(); i++) {
        const Entry& x = e_[i];
        if (!x.done || x.clip > 0.001f) continue;
        double s = x.n > 0 ? x.snrDb : -50 - std::fabs(x.rms + 16);
        if (s > bs) { bs = s; bi = (int)i; }
    }
    if (bi >= 0) { best_ = e_[bi].g; g = best_; }
    active_ = false;
}

bool GainSweep::update(double now, const Sample& s, GainSetting& g) {
    if (!active_) return false;
    const double settle = 1.0, measure = 2.0;
    if (idx_ < 0) { idx_ = 0; g = e_[0].g; t0_ = now; snrSum_ = rmsSum_ = clipMax_ = 0; nSnr_ = nAll_ = 0; return true; }
    const double el = now - t0_;
    if (el > settle) {
        if (s.locked) { snrSum_ += s.snrDb; nSnr_++; }
        rmsSum_ += s.rms; nAll_++;
        clipMax_ = std::max<double>(clipMax_, s.clip);
    }
    if (el < settle + measure) return false;
    Entry& e = e_[idx_];
    e.n = nSnr_;
    e.snrDb = nSnr_ ? snrSum_ / nSnr_ : -100;
    e.rms = nAll_ ? (float)(rmsSum_ / nAll_) : -120;
    e.clip = (float)clipMax_;
    e.done = true;
    if (++idx_ >= (int)e_.size()) { finish(g); return true; }
    g = e_[idx_].g; t0_ = now; snrSum_ = rmsSum_ = clipMax_ = 0; nSnr_ = nAll_ = 0;
    return true;
}

std::string GainSweep::summary() const {
    char b[160];
    snprintf(b, sizeof b, "best: LNA %d dB, VGA %d dB, amp %s", best_.lna, best_.vga, best_.amp ? "on" : "off");
    return b;
}

} // namespace dect2
