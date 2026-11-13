// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Narrowband FM deviation and CTCSS meter for two-way radio (a handheld, a repeater, a base): the FM-demodulated audio (24 ksps, in Hz) analysed in windows of 2^15 samples (1.4 s). Peak deviation of the
// voice band (300 - 3000 Hz, a raised-cosine filter made in the frequency domain, the 99.9th percentile of the absolute value so a noise spike is not a peak), the CTCSS (sub-audible) tone if there is one:
// its frequency (parabola on a Blackman-Harris spectrum), the nearest of the 50 standard tones (EIA / TIA-603) and the error against it, its deviation in Hz and as a share of 2.5 kHz and of 5.0 kHz (the
// usual maximum deviations of 12.5 and 25 kHz channels; CTCSS is commonly set to 10 - 20 % of it), the carrier offset (the mean of the demodulated signal) and the number of windows over 2.5 and over 5 kHz.
// A tone appearing, changing or disappearing is logged. Not a type-approval instrument: the SDR's own channel filter and demodulator are in the path; DCS (digital coded squelch) is not measured. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 24000, kCut = 8000;
constexpr int kN = 1 << 15;
constexpr double kTones[50] = {67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1};

class NfmDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kN;
        win_.resize(kN); double s2 = 0; for (int i = 0; i < kN; i++) { const double a = 2 * kPi * i / kN; win_[i] = (float)(0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a)); s2 += (double)win_[i] * win_[i]; }
        sw2_ = s2; reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); prev_ = cf32(1, 0); skip_ = (size_t)(0.2 * rate_); have_ = false; res_ = Res(); nWin_ = 0; lastTone_ = -1; over25_ = over5_ = 0; peakMax_ = 0; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { const cf32 d = a * std::conj(prev_); prev_ = a; if (skip_) { skip_--; return; } buf_.push_back((float)(std::atan2(d.imag(), d.real()) * rate_ / (2 * kPi))); if ((int)buf_.size() >= kN) { window(); buf_.clear(); } });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        t.headline = fmt("voice peak %.2f kHz, %s, carrier %+.0f Hz", res_.voicePeak / 1000, res_.tone > 0 ? fmt("CTCSS %.1f Hz at %.2f kHz", res_.toneNear, res_.toneDev / 1000).c_str() : "no CTCSS tone", res_.off);
        t.facts.push_back({"voice peak deviation", fmt("%.2f kHz (300 - 3000 Hz, 99.9th percentile of the window; largest so far %.2f kHz)", res_.voicePeak / 1000, peakMax_ / 1000)});
        t.facts.push_back({"over-deviation", fmt("%d windows over 2.5 kHz, %d over 5.0 kHz (of %d)", over25_, over5_, nWin_)});
        if (res_.tone > 0) {
            t.facts.push_back({"CTCSS tone", fmt("%.2f Hz (nearest standard tone %.1f Hz, error %+.2f Hz)", res_.tone, res_.toneNear, res_.tone - res_.toneNear)});
            t.facts.push_back({"CTCSS deviation", fmt("%.0f Hz peak (%.0f %% of 2.5 kHz, %.0f %% of 5.0 kHz)", res_.toneDev, 100 * res_.toneDev / 2500, 100 * res_.toneDev / 5000)});
        } else t.facts.push_back({"CTCSS tone", "none seen (60 - 260 Hz)"});
        t.facts.push_back({"carrier offset", fmt("%+.1f Hz from the tuned frequency", res_.off)});
        t.facts.push_back({"window", fmt("%d windows of %.1f s", nWin_, kN / rate_)});
    }
private:
    struct Res { double voicePeak = 0, tone = 0, toneNear = 0, toneDev = 0, off = 0; };
    void window() {
        have_ = true; nWin_++;
        double mean = 0; for (float v : buf_) mean += v; mean /= kN;
        std::vector<cf32> a((size_t)kN), w((size_t)kN);
        for (int i = 0; i < kN; i++) { a[i] = cf32((float)((buf_[i] - mean) * win_[i]), 0); w[i] = cf32((float)(buf_[i] - mean), 0); }
        fftx(a); fftx(w);
        // the voice band: a raised-cosine pass 300 - 3000 Hz (50 Hz skirts), back to the time domain
        std::vector<cf32> fb(w);
        for (int k = 0; k < kN; k++) { const double f = std::fabs((k < kN / 2 ? (double)k : (double)(k - kN)) * binHz_); double g = 0; if (f >= 300 && f <= 3000) g = 1; else if (f >= 250 && f < 300) g = 0.5 - 0.5 * std::cos(kPi * (f - 250) / 50); else if (f > 3000 && f <= 3050) g = 0.5 + 0.5 * std::cos(kPi * (f - 3000) / 50); fb[k] *= (float)g; }
        for (auto& v : fb) v = std::conj(v); fftx(fb);
        std::vector<float> ab((size_t)kN); for (int i = 0; i < kN; i++) ab[i] = std::fabs(fb[i].real()) / (float)kN;
        std::vector<float> s(ab); std::nth_element(s.begin(), s.begin() + (long)(0.999 * kN), s.end());
        Res r; r.voicePeak = s[(size_t)(0.999 * kN)]; r.off = mean;
        // the CTCSS tone: the strongest line between 60 and 260 Hz, 15 dB over the median of the 100 - 400 Hz bins around it
        std::vector<double> p((size_t)kN / 2); for (int k = 0; k < kN / 2; k++) p[k] = std::norm(a[k]) * 2.0 / ((double)kN * sw2_);
        const int k0 = (int)std::ceil(60 / binHz_), k1 = (int)std::floor(260 / binHz_); int kp = k0; for (int k = k0; k <= k1; k++) if (p[k] > p[kp]) kp = k;
        std::vector<double> nb; for (int k = (int)(40 / binHz_); k <= (int)(500 / binHz_); k += 3) if (std::abs(k - kp) > 12) nb.push_back(p[k]);
        std::nth_element(nb.begin(), nb.begin() + (long)(nb.size() / 2), nb.end()); const double med = std::max(nb[nb.size() / 2], 1e-30);
        double S = 0; for (int d = -3; d <= 3; d++) S += p[kp + d];
        if (p[kp] > 30.0 * med && S > 0) {
            const double la = std::log(p[kp - 1] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[kp + 1] + 1e-30), den = la - 2 * lb + lc;
            r.tone = (kp + (den < -1e-12 ? 0.5 * (la - lc) / den : 0.0)) * binHz_; r.toneDev = std::sqrt(2 * S);
            double bd = 1e9; for (double c : kTones) if (std::fabs(c - r.tone) < bd) { bd = std::fabs(c - r.tone); r.toneNear = c; }
        }
        res_ = r; peakMax_ = std::max(peakMax_, (double)r.voicePeak);
        if (r.voicePeak > 2500) over25_++; if (r.voicePeak > 5000) over5_++;
        out->good((float)(r.voicePeak / 100), r.off);
        const int tn = r.tone > 0 ? (int)std::lround(r.toneNear * 10) : 0;
        if (tn != lastTone_ && (lastTone_ >= 0 || tn != 0)) {
            if (tn) out->row("CTCSS", fmt("%.1f Hz", r.toneNear), fmt("%.2f Hz measured (%+.2f Hz from the standard tone), deviation %.0f Hz = %.0f %% of 2.5 kHz", r.tone, r.tone - r.toneNear, r.toneDev, 100 * r.toneDev / 2500), (float)r.toneDev);
            else out->row("CTCSS", "gone", "no tone any more", 0.f);
        }
        lastTone_ = tn;
        if (r.voicePeak > 5000) out->row("Over-deviation", fmt("%.2f kHz", r.voicePeak / 1000), fmt("voice peak %.2f kHz, over 5.0 kHz", r.voicePeak / 1000), (float)r.voicePeak);
        else if (r.voicePeak > 2500 && nWin_ % 2 == 0) out->row("Over-deviation", fmt("%.2f kHz", r.voicePeak / 1000), fmt("voice peak %.2f kHz, over 2.5 kHz (12.5 kHz channel limit)", r.voicePeak / 1000), (float)r.voicePeak);
    }
    Decimator dec_; double rate_ = 1, binHz_ = 1, sw2_ = 1, peakMax_ = 0; int nWin_ = 0, lastTone_ = -1, over25_ = 0, over5_ = 0; cf32 prev_{1, 0}; size_t skip_ = 0; bool have_ = false;
    std::vector<float> win_, buf_; Res res_;
};
}

std::unique_ptr<Decoder> makeNfmDev() { return std::make_unique<NfmDec>(); }

} // namespace dect2::x40
