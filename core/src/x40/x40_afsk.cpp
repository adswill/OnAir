// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AFSK (TNC) tone tuner for 1200 baud packet (Bell 202: mark 1200 Hz, space 2200 Hz; APRS, AX.25) and similar audio FSK on FM: what a transmitter's audio level and tones look like, for setting the deviation of a
// TNC or a radio's data input. The FM-demodulated audio (24 ksps, in Hz, flat) is searched for bursts of tone energy (1000 - 2500 Hz at 6 times the noise, 21 ms frames); each burst of 0.1 s or more is analysed:
// the mark and space tones (the top of the spectral hump, 120 Hz boxcar, in 900 - 1600 and 1800 - 2600 Hz), their error against 1200 and 2200 Hz, the twist (space level over mark level, in dB: a TNC aimed at an
// FM radio's pre-emphasis wants a few dB of mark-heavy audio, about -3 to -6 dB measured at the demodulator of a de-emphasised receiver, 0 to +3 dB flat; the meter says what it sees), the peak deviation of the
// burst (99.9th percentile of the absolute value of the demodulated audio, in kHz; 3 kHz is the usual aim on a 25 kHz channel), its length and the gap since the last burst. Totals: bursts, the
// range of the peak deviation. The tone level assumes the data keep both tones in use (as AX.25 flags and bit stuffing do); a burst of one tone only reads a twist that is not the radio's. It does not decode the packets (the packet mode does).
// Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 24000, kCut = 8000;
constexpr int kFrame = 512;

class AfskDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kFrame;
        win_.resize(kFrame); for (int i = 0; i < kFrame; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFrame));
        reset(); return true;
    }
    void reset() override { dec_.reset(); fr_.clear(); prev_ = cf32(1, 0); skip_ = (size_t)(0.2 * rate_); have_ = false; hist_.clear(); inBurst_ = false; burst_.clear(); nBursts_ = 0; t_ = 0; lastEnd_ = -1; pkMin_ = 1e9; pkMax_ = 0; res_ = Res(); onCnt_ = offCnt_ = 0; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { const cf32 d = a * std::conj(prev_); prev_ = a; if (skip_) { skip_--; return; } fr_.push_back((float)(std::atan2(d.imag(), d.real()) * rate_ / (2 * kPi))); if ((int)fr_.size() >= kFrame) { frame(); fr_.clear(); } });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = nBursts_ ? fmt("%d bursts so far, none of 0.1 s or more analysed", nBursts_) : "no AFSK burst yet"; return; }
        t.headline = fmt("last burst: mark %.0f Hz, space %.0f Hz, twist %+.1f dB, peak deviation %.2f kHz, %.2f s", res_.mark, res_.space, res_.twist, res_.peak / 1000, res_.len);
        t.facts.push_back({"mark tone", fmt("%.1f Hz (%+.1f Hz from 1200)", res_.mark, res_.mark - 1200)});
        t.facts.push_back({"space tone", fmt("%.1f Hz (%+.1f Hz from 2200)", res_.space, res_.space - 2200)});
        t.facts.push_back({"twist (space over mark)", fmt("%+.1f dB", res_.twist)});
        t.facts.push_back({"peak deviation of the burst", fmt("%.2f kHz", res_.peak / 1000)});
        t.facts.push_back({"deviation seen so far", fmt("%.2f to %.2f kHz over %d bursts", pkMin_ / 1000, pkMax_ / 1000, nBursts_)});
        t.facts.push_back({"last burst", fmt("%.2f s long, %.1f s after the one before", res_.len, res_.gap)});
    }
private:
    struct Res { bool ok = false; double mark = 0, space = 0, twist = 0, peak = 0, len = 0, gap = 0; };
    void frame() {
        have_ = true; t_ += (double)kFrame / rate_;
        std::vector<cf32> a(kFrame); double mean = 0; for (float v : fr_) mean += v; mean /= kFrame;
        for (int i = 0; i < kFrame; i++) a[i] = cf32((float)((fr_[i] - mean) * win_[i]), 0);
        fftx(a);
        double pb = 0; for (int k = (int)(1000 / binHz_); k <= (int)(2500 / binHz_); k++) pb += std::norm(a[k]);
        hist_.push_back((float)pb); if (hist_.size() > (size_t)(5 * rate_ / kFrame)) hist_.pop_front();
        std::vector<float> s(hist_.begin(), hist_.end()); std::nth_element(s.begin(), s.begin() + (long)(s.size() / 5), s.end()); const double floorP = std::max((double)s[s.size() / 5], 1e-12);
        const bool on = pb > 6.0 * floorP, off = pb < 3.0 * floorP;
        if (!inBurst_) {
            if (on) { if (++onCnt_ >= 2) { inBurst_ = true; burst_.clear(); burst_.insert(burst_.end(), prevFr_.begin(), prevFr_.end()); startT_ = t_ - 2.0 * kFrame / rate_; offCnt_ = 0; } } else onCnt_ = 0;
        }
        if (inBurst_) {
            for (float v : fr_) burst_.push_back(v - (float)mean);
            if (off) { if (++offCnt_ >= 3) endBurst(); } else offCnt_ = 0;
            if (burst_.size() > (size_t)(6 * rate_)) endBurst();
        }
        prevFr_.assign(fr_.size(), 0.f); for (size_t i = 0; i < fr_.size(); i++) prevFr_[i] = fr_[i] - (float)mean;
        if (!inBurst_ && !hist_.empty() && pb < 3.0 * floorP) out->energy();
    }
    void endBurst() {
        inBurst_ = false; onCnt_ = offCnt_ = 0;
        const double len = (double)burst_.size() / rate_ - 3.0 * kFrame / rate_;
        if (len < 0.1) { burst_.clear(); return; }
        nBursts_++;
        int n = 1; while ((size_t)n * 2 <= burst_.size()) n *= 2; n = std::min(n, 1 << 15);
        const double bh = rate_ / n; std::vector<cf32> a((size_t)n);
        for (int i = 0; i < n; i++) a[i] = cf32(burst_[i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n)), 0);
        fftx(a);
        // each tone: the power of its band, and the top of the hump of that band's spectrum (a boxcar of 120 Hz), the data spread the tone over about the baud rate on each side
        std::vector<double> ps((size_t)n / 2 + 1); for (int k = 0; k <= n / 2; k++) ps[k] = std::norm(a[k]);
        const int hw = std::max(1, (int)(60 / bh));
        auto band = [&](double f0, double f1, double& top) { double s = 0; for (int k = (int)(f0 / bh); k <= (int)(f1 / bh); k++) s += ps[k]; double best = -1; int kb = (int)(f0 / bh);
            for (int k = (int)(f0 / bh) + hw; k <= (int)(f1 / bh) - hw; k++) { double v = 0; for (int d = -hw; d <= hw; d += std::max(1, hw / 8)) v += ps[k + d]; if (v > best) { best = v; kb = k; } }
            top = kb * bh; return s; };
        double cm, cs; const double Em = band(900, 1600, cm), Es = band(1800, 2600, cs);
        // the peak deviation: the burst's audio limited to 800 - 2700 Hz (the noise outside the tones does not count), the 99.5th percentile of the absolute value
        std::vector<cf32> fb(a); for (int k = 0; k < n; k++) { const double f = std::fabs((k < n / 2 ? (double)k : (double)(k - n)) * bh); if (f < 800 || f > 2700) fb[k] = cf32(0, 0); }
        for (int i = 0; i < n; i++) { cf32 v = cf32(burst_[i], 0); (void)v; }
        std::vector<cf32> raw((size_t)n); for (int i = 0; i < n; i++) raw[i] = cf32(burst_[i], 0); fftx(raw);
        for (int k = 0; k < n; k++) { const double f = std::fabs((k < n / 2 ? (double)k : (double)(k - n)) * bh); if (f < 800 || f > 2700) raw[k] = cf32(0, 0); }
        for (auto& v : raw) v = std::conj(v); fftx(raw);
        std::vector<float> ab((size_t)n); for (int i = 0; i < n; i++) ab[i] = std::fabs(raw[i].real()) / (float)n;
        std::nth_element(ab.begin(), ab.begin() + (long)(0.995 * n), ab.end());
        // the tone frequencies: the frequency of every full period of the limited audio (upward zero crossings), the 20th percentile of those near 1200 Hz and the 70th of those near 2200 Hz (a data spectrum is too broad to read a tone from)
        { std::vector<double> zc; for (int i = 1; i < n; i++) { const double v0 = raw[i - 1].real() / n, v1 = raw[i].real() / n; if (v0 < 0 && v1 >= 0) zc.push_back((i - 1) + (-v0) / (v1 - v0)); }
          std::vector<double> fm, fs2; for (size_t i = 1; i < zc.size(); i++) { const double f = rate_ / (zc[i] - zc[i - 1]); if (f > 950 && f < 1550) fm.push_back(f); else if (f > 1850 && f < 2550) fs2.push_back(f); }
          // a period that straddles a tone change is a mixture of the two tones (higher than the mark, lower than the space): the pure ones are the ends, the 20th and 70th percentiles
          auto pct = [](std::vector<double>& v, double q, double dflt) { if (v.size() < 8) return dflt; std::nth_element(v.begin(), v.begin() + (long)(q * (double)v.size()), v.end()); return v[(size_t)(q * (double)v.size())]; };
          cm = pct(fm, 0.2, cm); cs = pct(fs2, 0.7, cs); }
        Res r; r.ok = Em > 0 && Es > 0; r.mark = cm; r.space = cs; r.twist = 10 * std::log10(Es / std::max(Em, 1e-30)); r.peak = ab[(size_t)(0.995 * n)]; r.len = len; r.gap = lastEnd_ >= 0 ? startT_ - lastEnd_ : 0;
        lastEnd_ = t_ - 3.0 * kFrame / rate_;
        pkMin_ = std::min(pkMin_, r.peak); pkMax_ = std::max(pkMax_, r.peak);
        res_ = r; burst_.clear();
        out->good((float)(r.peak / 100), 0);
        out->row("Burst", fmt("%.2f kHz", r.peak / 1000), fmt("%.2f s: mark %.0f Hz, space %.0f Hz, twist %+.1f dB, peak deviation %.2f kHz%s", len, r.mark, r.space, r.twist, r.peak / 1000, r.gap > 0 ? fmt(", %.1f s after the last", r.gap).c_str() : ""), (float)r.peak / 100);
    }
    Decimator dec_; double rate_ = 1, binHz_ = 1, t_ = 0, startT_ = 0, lastEnd_ = -1, pkMin_ = 1e9, pkMax_ = 0; int nBursts_ = 0, onCnt_ = 0, offCnt_ = 0; cf32 prev_{1, 0}; size_t skip_ = 0; bool have_ = false, inBurst_ = false;
    std::vector<float> win_, fr_, burst_, prevFr_; std::deque<float> hist_; Res res_;
};
}

std::unique_ptr<Decoder> makeAfsk() { return std::make_unique<AfskDec>(); }

} // namespace dect2::x40
