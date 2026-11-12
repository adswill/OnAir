// FM SINAD meter: a carrier frequency-modulated with a test tone (normally 1 kHz) through the radio: the FM-demodulated audio (24 ksps) is analysed in windows of 2^15 samples (1.4 s, Blackman-Harris).
// The tone is the strongest line between 300 and 3000 Hz (20 dB over the noise). SINAD = (signal + noise + distortion) / (noise + distortion) = P(300 - 3000 Hz) / (P(300 - 3000 Hz) - P(tone)),
// in dB, flat-weighted (no psophometric or C-message filter, no de-emphasis). THD = the RMS sum of harmonics 2 to 5 (below 8 kHz) over the tone, in %. Peak deviation = the tone's amplitude in Hz
// (the demodulated audio is in Hz), carrier offset = the mean of the demodulated signal. The 12 dB SINAD crossing is logged (the usual sensitivity figure). It measures the transmitter, the path and
// this SDR together (the SDR's own demodulator and phase noise included); a deviation set badly into the radio changes the SINAD (the noise falls as f^2, so a high tone helps). FM thresholding
// below about 10 dB carrier to noise makes the number fall faster than theory. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 24000, kCut = 8000, kBandLo = 300, kBandHi = 3000;
constexpr int kN = 1 << 15;

class SinadDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kN; n_ = kN;
        win_.resize(kN); double s2 = 0; for (int i = 0; i < kN; i++) { const double a = 2 * kPi * i / kN; win_[i] = (float)(0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a)); s2 += (double)win_[i] * win_[i]; }
        sw2_ = s2; reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); prev_ = cf32(1, 0); skip_ = (size_t)(0.2 * rate_); have_ = false; res_ = Res(); below12_ = true; nWin_ = 0; hist_.clear(); }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            const cf32 d = a * std::conj(prev_); prev_ = a;
            if (skip_) { skip_--; return; }
            buf_.push_back((float)(std::atan2(d.imag(), d.real()) * rate_ / (2 * kPi)));
            if ((int)buf_.size() >= n_) { window(); buf_.clear(); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = "no test tone in 300 - 3000 Hz (20 dB over the noise needed)"; return; }
        t.headline = fmt("SINAD %.1f dB, tone %.0f Hz, deviation %.2f kHz, THD %.1f %%", res_.sinad, res_.tone, res_.devHz / 1000, res_.thd);
        t.facts.push_back({"SINAD", fmt("%.1f dB (300 - 3000 Hz, flat weighting)%s", res_.sinad, res_.sinad >= 12 ? "" : ", under the 12 dB figure")});
        t.facts.push_back({"test tone", fmt("%.1f Hz", res_.tone)});
        t.facts.push_back({"peak deviation", fmt("%.2f kHz", res_.devHz / 1000)});
        t.facts.push_back({"THD", fmt("%.2f %% (harmonics 2 to 5)", res_.thd)});
        t.facts.push_back({"THD + noise", fmt("%.1f %% (everything in 300 - 3000 Hz but the tone)", res_.thdn)});
        t.facts.push_back({"signal to noise (tone to everything else in the band)", fmt("%.1f dB", res_.sinad)});
        t.facts.push_back({"carrier offset", fmt("%+.1f Hz from the tuned frequency", res_.off)});
        t.facts.push_back({"window", fmt("%d windows of %.1f s", nWin_, kN / rate_)});
    }
private:
    struct Res { bool ok = false; double sinad = 0, tone = 0, devHz = 0, thd = 0, thdn = 0, off = 0; };
    void window() {
        have_ = true; nWin_++;
        double mean = 0; for (float v : buf_) mean += v; mean /= kN;
        std::vector<cf32> a((size_t)kN); for (int i = 0; i < kN; i++) a[i] = cf32((float)((buf_[i] - mean) * win_[i]), 0);
        fftx(a);
        std::vector<double> p((size_t)kN / 2); for (int k = 0; k < kN / 2; k++) p[k] = std::norm(a[k]) * 2.0 / ((double)kN * sw2_);   // power of the real signal per bin (sum over bins = mean square)
        const int kLo = (int)std::ceil(kBandLo / binHz_), kHi = (int)std::floor(kBandHi / binHz_);
        std::vector<double> band(p.begin() + kLo, p.begin() + kHi); std::vector<double> sorted(band); std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const double med = std::max(sorted[sorted.size() / 2], 1e-30);
        int kp = kLo; for (int k = kLo; k < kHi; k++) if (p[k] > p[kp]) kp = k;
        double S = 0; for (int d = -3; d <= 3; d++) S += p[kp + d];
        Res r;
        if (p[kp] < 100.0 * med) { res_ = r; out->energy(); return; }
        double tot = 0; for (int k = kLo; k <= kHi; k++) tot += p[k];
        // the tone's frequency (parabola), the harmonics
        const double la = std::log(p[kp - 1] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[kp + 1] + 1e-30), den = la - 2 * lb + lc;
        r.tone = (kp + (den < -1e-12 ? 0.5 * (la - lc) / den : 0.0)) * binHz_;
        r.ok = true; r.sinad = 10 * std::log10(tot / std::max(tot - S, 1e-30)); r.devHz = std::sqrt(2 * S); r.off = mean;
        double h = 0; for (int n = 2; n <= 5; n++) { const double f = n * r.tone; if (f > 8000) break; const int k = (int)std::lround(f / binHz_); double pw = 0; for (int d = -3; d <= 3; d++) if (k + d < kN / 2) pw += p[k + d]; h += pw; }
        r.thd = 100 * std::sqrt(h / S); r.thdn = 100 * std::sqrt(std::max(tot - S, 0.0) / S);
        res_ = r; out->good((float)r.sinad, r.off);
        const bool nowBelow = r.sinad < 12.0;
        if (nWin_ == 1 || nowBelow != below12_ ) { out->row("SINAD", fmt("%.1f dB", r.sinad), nowBelow ? fmt("under 12 dB (tone %.0f Hz, deviation %.2f kHz)", r.tone, r.devHz / 1000) : fmt("12 dB or better (tone %.0f Hz, deviation %.2f kHz, THD %.1f %%)", r.tone, r.devHz / 1000, r.thd), (float)r.sinad); below12_ = nowBelow; }
        else if (nWin_ % 8 == 0) out->row("SINAD", fmt("%.1f dB", r.sinad), fmt("tone %.0f Hz, deviation %.2f kHz, THD %.1f %%", r.tone, r.devHz / 1000, r.thd), (float)r.sinad);
    }
    Decimator dec_; double rate_ = 1, binHz_ = 1, sw2_ = 1; int n_ = 1, nWin_ = 0; cf32 prev_{1, 0}; size_t skip_ = 0; bool have_ = false, below12_ = true;
    std::vector<float> win_, buf_, hist_; Res res_;
};
}

std::unique_ptr<Decoder> makeSinad() { return std::make_unique<SinadDec>(); }

} // namespace dect2::x40
