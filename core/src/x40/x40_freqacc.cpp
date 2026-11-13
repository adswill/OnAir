// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Carrier frequency accuracy meter: tune a carrier of a known frequency (a time-signal or broadcast station, a GPS-locked beacon, a calibrator) and read the error of the radio
// in Hz and in parts per million of the tuned frequency. Every window the carrier is found in a +-2.8 kHz window (a Hann-windowed FFT of 1 to 2 s, a power of two of samples, log-parabolic peak), its offset from the
// tuned frequency and its carrier-to-noise-density ratio are taken. From the readings: the average and the scatter of the last ten, the drift (straight-line fit over up to four minutes,
// in ppm per hour) and the Allan deviation at 1 to 32 s. A step in the frequency and the loss and return of the carrier are logged. The reading is the sum of the radio's error and
// the transmitter's own: a reference that is itself off by a part in 10^7 limits what a ppm reading means. The tuned frequency is the one the radio reports (the Allan deviation and ppm
// need it; without it the numbers are in Hz). Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 16000, kCut = 3000, kWinSec = 2.0, kSearchHz = 2800;
constexpr int kKeep = 120;

class FreqAccDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); n_ = 1; while (n_ * 2 <= kWinSec * rate_) n_ *= 2;
        winSec_ = n_ / rate_;
        win_.resize(n_); for (int i = 0; i < n_; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n_));
        reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); skip_ = (size_t)(0.2 * rate_); rd_.clear(); lost_ = true; have_ = false; t_ = 0; nMiss_ = 0; nRead_ = 0; cn0_ = 0; lastRow_ = -1e9; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            t_ += 1.0 / rate_;
            if (skip_) { skip_--; return; }
            buf_.push_back(a);
            if ((int)buf_.size() >= n_) { analyse(); buf_.clear(); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (lost_) { t.headline = "no carrier in +-2.8 kHz of the tuned frequency"; return; }
        const int m = (int)rd_.size(), k = std::min(m, 10);
        double mean = 0; for (int i = m - k; i < m; i++) mean += rd_[i].hz; mean /= k;
        double var = 0; for (int i = m - k; i < m; i++) var += (rd_[i].hz - mean) * (rd_[i].hz - mean); const double sd = k > 1 ? std::sqrt(var / (k - 1)) : 0;
        t.headline = centre_ > 0 ? fmt("carrier %+.2f Hz = %+.3f ppm from the tuned frequency (C/N0 %.0f dB-Hz)", mean, mean / centre_ * 1e6, cn0_) : fmt("carrier %+.2f Hz from the tuned frequency (C/N0 %.0f dB-Hz)", mean, cn0_);
        t.facts.push_back({"offset", fmt("%+.2f Hz (mean of the last %d readings, scatter %.2f Hz)", mean, k, sd)});
        if (centre_ > 0) t.facts.push_back({"error", fmt("%+.3f ppm of %.6f MHz", mean / centre_ * 1e6, centre_ / 1e6)});
        t.facts.push_back({"carrier to noise density", fmt("%.1f dB-Hz", cn0_)});
        if (m >= 8) {
            double st = 0, sy = 0, stt = 0, sty = 0; const int j0 = std::max(0, m - kKeep); const int cnt = m - j0;
            for (int i = j0; i < m; i++) { const double tt = rd_[i].t - rd_[j0].t; st += tt; sy += rd_[i].hz; stt += tt * tt; sty += tt * rd_[i].hz; }
            const double den = cnt * stt - st * st; const double slope = den > 0 ? (cnt * sty - st * sy) / den : 0;   // Hz per second
            t.facts.push_back({"drift", centre_ > 0 ? fmt("%+.3f ppm per hour (%+.4f Hz/s)", slope * 3600 / centre_ * 1e6, slope) : fmt("%+.4f Hz/s", slope)});
        }
        std::string av;
        for (int tau = 1; tau <= 16; tau *= 2) {
            // Allan deviation of the offset at tau = 2 * tau seconds: non-overlapping averages of `tau` readings
            const int groups = m / tau; if (groups < 4) break;
            std::vector<double> y; for (int g = 0; g < groups; g++) { double s = 0; for (int i = 0; i < tau; i++) s += rd_[g * tau + i].hz; y.push_back(s / tau); }
            double s2 = 0; for (size_t g = 1; g < y.size(); g++) s2 += (y[g] - y[g - 1]) * (y[g] - y[g - 1]);
            const double adevHz = std::sqrt(s2 / (2.0 * (y.size() - 1)));
            if (!av.empty()) av += ", ";
            av += centre_ > 0 ? fmt("%.1f s: %.2e", tau * winSec_, adevHz / centre_) : fmt("%.1f s: %.3f Hz", tau * winSec_, adevHz);
        }
        t.facts.push_back({"Allan deviation", av.empty() ? std::string("after about 15 readings") : av});
        t.facts.push_back({"readings", fmt("%d (one every %.2f s)", nRead_, winSec_)});
    }
private:
    struct Rd { double t, hz; };
    void analyse() {
        have_ = true;
        std::vector<cf32> a((size_t)n_);
        for (int i = 0; i < n_; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        const double bh = rate_ / n_;
        std::vector<float> p(n_); for (int k = 0; k < n_; k++) p[k] = std::norm(a[k]);
        const int hw = (int)(kSearchHz / bh);
        int kp = 0; float pk = 0;
        for (int d = -hw; d <= hw; d++) { const int k = (d + n_) % n_; if (p[k] > pk) { pk = p[k]; kp = k; } }
        // noise: the median of the bins in the filter band outside the carrier
        std::vector<float> nb; for (int d = -(int)(kCut / bh); d <= (int)(kCut / bh); d++) { const int k = (d + n_) % n_; int dk = std::abs(((k - kp + n_ / 2) % n_ + n_) % n_ - n_ / 2); if (dk > 8) nb.push_back(p[k]); }
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end()); const double med = std::max((double)nb[nb.size() / 2], 1e-30), noise = med / 0.693;   // mean of an exponential
        const bool present = pk > 40.0 * noise;
        if (!present) { if (!lost_ && ++nMiss_ >= 2) { lost_ = true; out->row("Carrier", "lost", "no carrier in the window", -120.f); } else if (lost_) out->energy(); return; }
        nMiss_ = 0;
        const double la = std::log(p[(kp + n_ - 1) % n_] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[(kp + 1) % n_] + 1e-30);
        const double den = la - 2 * lb + lc; const double dl = den < -1e-12 ? 0.5 * (la - lc) / den : 0.0;
        double fb = (kp + dl) * bh; if (fb > rate_ / 2) fb -= rate_;
        const double lpk = lb - 0.25 * (la - lc) * dl;                          // log of the peak power at the refined position
        const double ratio = std::exp(lpk) / noise;
        const double cn0 = ratio * 1.5 * rate_ / n_;                              // A^2 / (sigma^2 / fs) for a Hann window, see the notes
        cn0_ = (float)(10 * std::log10(std::max(cn0, 1e-9)));
        const bool wasLost = lost_;
        if (!rd_.empty() && !wasLost) {
            // a step: more than 8 scatter widths from the median of the last five (at least 0.5 Hz)
            std::vector<double> last; for (size_t i = rd_.size() >= 5 ? rd_.size() - 5 : 0; i < rd_.size(); i++) last.push_back(rd_[i].hz);
            std::sort(last.begin(), last.end()); const double mdn = last[last.size() / 2];
            const double thr = std::max(0.5, 8 * sigma());
            if (std::fabs(fb - mdn) > thr && last.size() >= 5) out->row("Step", fmt("%+.2f Hz", fb - mdn), fmt("the carrier moved from %+.2f to %+.2f Hz", mdn, fb), cn0_);
        }
        lost_ = false;
        if (wasLost && !rd_.empty()) out->row("Carrier", "returned", fmt("%+.2f Hz, C/N0 %.0f dB-Hz", fb, cn0_), cn0_);
        rd_.push_back({t_, fb}); nRead_++;
        if ((int)rd_.size() > 4000) rd_.erase(rd_.begin(), rd_.begin() + 1000);
        out->good(cn0_, fb);
        if (t_ - lastRow_ >= 10.0 || nRead_ == 1) { lastRow_ = t_; out->row("Frequency", fmt("%+.2f Hz", fb), centre_ > 0 ? fmt("%+.3f ppm, C/N0 %.0f dB-Hz", fb / centre_ * 1e6, cn0_) : fmt("C/N0 %.0f dB-Hz", cn0_), cn0_); }
    }
    double sigma() const {
        const size_t m = rd_.size(), k = std::min<size_t>(m, 10); if (k < 3) return 0.1;
        double mean = 0; for (size_t i = m - k; i < m; i++) mean += rd_[i].hz; mean /= (double)k;
        double v = 0; for (size_t i = m - k; i < m; i++) v += (rd_[i].hz - mean) * (rd_[i].hz - mean);
        return std::sqrt(v / (double)(k - 1));
    }
    Decimator dec_; double rate_ = 1, t_ = 0, centre_ = 0, lastRow_ = -1e9, winSec_ = 1; int n_ = 1, nMiss_ = 0, nRead_ = 0; size_t skip_ = 0; float cn0_ = 0;
    std::vector<float> win_; std::vector<cf32> buf_; std::vector<Rd> rd_; bool lost_ = true, have_ = false;
};
}

std::unique_ptr<Decoder> makeFreqAcc() { return std::make_unique<FreqAccDec>(); }

} // namespace dect2::x40
