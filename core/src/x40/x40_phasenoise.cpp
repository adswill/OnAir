// Phase noise meter: the spectrum of a carrier (within the +-100 kHz channel) next to the carrier, read as single-sideband phase noise L(f) in dBc/Hz at 10 Hz to 100 kHz offsets, plus the
// spurs (mains and supply lines, reference spurs) and the integrated phase jitter from 100 Hz to 100 kHz. Blocks of 2^18 samples (about one second at 250 ksps) are shifted so the carrier sits at 0 Hz
// (it is followed from block to block), windowed with a four-term Blackman-Harris window (-92 dB sidelobes), their power spectra averaged (up to 16 blocks), the carrier power C taken as all the power in the channel (carrier plus skirts),
// and L(f) = (power per Hz at +f and -f averaged) / C. It includes amplitude noise and the additive noise of the radio (the measurement floor is the noise floor of the radio at this signal level:
// a carrier 60 dB over the noise in 100 kHz cannot show better than about -110 dBc/Hz at far offsets), and the phase noise of the **radio's own oscillator**: tuning a good generator on a poor SDR measures
// the SDR. The close-in values need a carrier that does not move more than about a Hz in a block. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 250000, kCut = 100000;
constexpr int kN = 1 << 18, kAvg = 16, kMinBlocks = 4;
constexpr double kOffsets[5] = {10, 100, 1000, 10000, 100000};

class PhaseNoiseDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kN;
        win_.resize(kN); double s1 = 0, s2 = 0;
        for (int i = 0; i < kN; i++) { const double a = 2 * kPi * i / kN; win_[i] = (float)(0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a)); s1 += win_[i]; s2 += (double)win_[i] * win_[i]; }
        sw2_ = s2; enbwBins_ = kN * s2 / (s1 * s1);
        reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); skip_ = (size_t)(0.3 * rate_); acc_.assign(kN, 0.0); nAvg_ = 0; fc_ = 0; haveFc_ = false; ph_ = 0; have_ = false; res_ = Res(); lost_ = true; lastRow_ = -1e9; t_ = 0; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            t_ += 1.0 / rate_;
            if (skip_) { skip_--; return; }
            if (haveFc_) { ph_ -= 2 * kPi * fc_ / rate_; if (ph_ < -kPi) ph_ += 2 * kPi; a *= cf32((float)std::cos(ph_), (float)std::sin(ph_)); }
            buf_.push_back(a);
            if ((int)buf_.size() >= kN) { block(); buf_.clear(); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (lost_) { t.headline = "no carrier in the channel"; return; }
        if (!res_.ok) { t.headline = fmt("averaging: %d of %d blocks", nAvg_, kMinBlocks); return; }
        t.headline = fmt("L(1 kHz) %.1f, L(10 kHz) %.1f, L(100 kHz) %.1f dBc/Hz; jitter %.2f ps RMS", res_.L[2], res_.L[3], res_.L[4], res_.jitterPs);
        const char* nm[5] = {"10 Hz", "100 Hz", "1 kHz", "10 kHz", "100 kHz"};
        for (int i = 0; i < 5; i++) t.facts.push_back({fmt("phase noise at %s", nm[i]), std::isnan(res_.L[i]) ? std::string("-") : fmt("%.1f dBc/Hz", res_.L[i])});
        t.facts.push_back({"integrated phase jitter, 100 Hz - 100 kHz", fmt("%.2f mrad RMS, %.2f ps at the carrier frequency%s", res_.jitterRad * 1000, res_.jitterPs, centre_ > 0 ? "" : " (no tuned frequency: ps not valid)")});
        for (size_t i = 0; i < res_.spurs.size() && i < 4; i++) t.facts.push_back({fmt("spur %zu", i + 1), fmt("%+.0f Hz from the carrier, %.1f dBc", res_.spurs[i].first, res_.spurs[i].second)});
        t.facts.push_back({"carrier", fmt("%+.2f Hz from the tuned frequency, %.1f dB over the noise in the channel", fc_, res_.cnr)});
        t.facts.push_back({"measurement", fmt("%d blocks of %.2f s averaged, bins of %.2f Hz, window ENBW %.2f bins", nAvg_, kN / rate_, binHz_, enbwBins_)});
    }
    void setCenterHz(double hz) override { centre_ = hz; }
private:
    struct Res { bool ok = false; double L[5] = {0, 0, 0, 0, 0}; double jitterRad = 0, jitterPs = 0, cnr = 0; std::vector<std::pair<double, double>> spurs; };
    void block() {
        have_ = true;
        std::vector<cf32> a((size_t)kN); for (int i = 0; i < kN; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        std::vector<double> p((size_t)kN); for (int k = 0; k < kN; k++) p[k] = std::norm(a[k]);
        // the carrier: the peak (anywhere on the first block; near 0 Hz afterwards)
        int kp = 0; double pk = 0; const int range = haveFc_ ? (int)(300 / binHz_) : kN / 2;
        for (int d = -range; d <= range; d++) { const int k = (d + kN) % kN; if (p[k] > pk) { pk = p[k]; kp = k; } }
        std::vector<double> sorted(p.begin(), p.begin() + kN); std::nth_element(sorted.begin(), sorted.begin() + kN / 2, sorted.end());
        const double med = std::max(sorted[kN / 2], 1e-30);
        if (pk < 1e5 * med) { if (!lost_) { lost_ = true; res_ = Res(); nAvg_ = 0; std::fill(acc_.begin(), acc_.end(), 0.0); out->row("Carrier", "lost", "no carrier in the channel", -120.f); } haveFc_ = false; out->energy(); return; }
        const double la = std::log(p[(kp + kN - 1) % kN] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[(kp + 1) % kN] + 1e-30), den = la - 2 * lb + lc;
        const double dl = den < -1e-12 ? 0.5 * (la - lc) / den : 0.0;
        const double off = ((kp < kN / 2 ? kp : kp - kN) + dl) * binHz_;
        if (!haveFc_) { fc_ = off; haveFc_ = true; ph_ = 0; buf_.clear(); return; }       // the first block only finds the carrier; the next ones are mixed to it
        fc_ += 0.5 * off; lost_ = false;
        // shift the spectrum so that the peak bin is at 0, and add it
        const int sh = kp;
        for (int k = 0; k < kN; k++) acc_[k] = acc_[k] * (nAvg_ >= kAvg ? 1.0 - 1.0 / kAvg : (double)nAvg_ / (nAvg_ + 1)) + p[(k + sh) % kN] * (nAvg_ >= kAvg ? 1.0 / kAvg : 1.0 / (nAvg_ + 1));
        if (nAvg_ < kAvg) nAvg_++;
        if (nAvg_ >= kMinBlocks) evaluate();
        out->good(0, fc_);
    }
    double dbcPerHz(const std::vector<double>& S, double C, int k) const { return 10 * std::log10(std::max(S[k], 1e-30) / (kN * sw2_) / binHz_ / C); }
    void evaluate() {
        const std::vector<double>& S = acc_;
        double C = 0; { const int lim = (int)(0.45 * rate_ / binHz_); for (int d = -lim; d <= lim; d++) C += S[(d + kN) % kN]; } C /= (kN * sw2_);   // all the power in the channel: the carrier with its phase-noise skirts (the noise in it is a part per thousand or less)
        Res r; r.ok = true;
        // L at the offsets: the mean of the power in +-15 % around f, both sides
        for (int i = 0; i < 5; i++) {
            const double f = kOffsets[i]; if (f > 0.8 * rate_ / 2 || f < 3 * binHz_) { r.L[i] = std::nan(""); continue; }
            const int k0 = (int)std::lround(f * 0.85 / binHz_), k1 = std::max(k0 + 1, (int)std::lround(f * 1.15 / binHz_)); double s = 0; int c = 0;
            for (int k = k0; k <= k1; k++) { s += S[k] + S[kN - k]; c += 2; }
            r.L[i] = 10 * std::log10(std::max(s / c, 1e-30) / (kN * sw2_) / binHz_ / C);
        }
        // integrated jitter 100 Hz - 100 kHz: sigma^2 = 2 integral L(f) df (L single sideband)
        double integ = 0; const int kLo = (int)std::lround(100 / binHz_), kHi = std::min(kN / 2 - 1, (int)std::lround(std::min(100000.0, 0.4 * rate_) / binHz_));
        for (int k = kLo; k <= kHi; k++) integ += 0.5 * (S[k] + S[kN - k]) / (kN * sw2_) / binHz_ / C * binHz_;
        r.jitterRad = std::sqrt(2 * integ); r.jitterPs = centre_ > 0 ? r.jitterRad / (2 * kPi * centre_) * 1e12 : 0;
        // spurs: peaks over 6 x the median of +-60 bins, outside +-30 Hz and not within a few bins of one another
        std::vector<std::pair<double, double>> sp;
        for (int k = std::max(kLo / 2, (int)(30 / binHz_)); k < kN / 2 - 1; k++) {
            if (S[k] < S[k - 1] || S[k] < S[k + 1]) continue;
            std::vector<double> nb; for (int d = -60; d <= 60; d += 4) { if (std::abs(d) <= 8) continue; const int kk = k + d; if (kk > 0 && kk < kN) nb.push_back(S[kk]); }
            std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
            if (S[k] > 8.0 * nb[nb.size() / 2]) { double pw = 0; for (int d = -4; d <= 4; d++) pw += S[(k + d + kN) % kN]; sp.push_back({k * binHz_, 10 * std::log10(pw / (kN * sw2_) / C)}); k += 6; }
        }
        std::sort(sp.begin(), sp.end(), [](const auto& a, const auto& b) { return a.second > b.second; }); if (sp.size() > 6) sp.resize(6);
        r.spurs = sp;
        // carrier to noise in the channel
        r.cnr = cnrOf(S, C);
        res_ = r;
        if (t_ - lastRow_ >= 8.0) { lastRow_ = t_; out->row("Phase noise", fmt("L(1 kHz) %.1f dBc/Hz", r.L[2]), fmt("10 Hz %.1f, 100 Hz %.1f, 1 kHz %.1f, 10 kHz %.1f, 100 kHz %.1f dBc/Hz; jitter %.2f mrad; %zu spurs", r.L[0], r.L[1], r.L[2], r.L[3], r.L[4], r.jitterRad * 1000, r.spurs.size()), (float)r.L[2]); }
    }
    static double sorted_med(const std::vector<double>& S) { std::vector<double> v(S.begin(), S.end()); std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }
    double cnrOf(const std::vector<double>& S, double C) const { const double nPerHz = sorted_med(S) / 0.693 / (kN * sw2_) / binHz_; return 10 * std::log10(std::max(C / (nPerHz * 200000.0), 1e-9)); }
    Decimator dec_; double rate_ = 1, binHz_ = 1, sw2_ = 1, enbwBins_ = 1, fc_ = 0, ph_ = 0, t_ = 0, lastRow_ = -1e9, centre_ = 0; size_t skip_ = 0; int nAvg_ = 0;
    std::vector<float> win_; std::vector<cf32> buf_; std::vector<double> acc_; bool haveFc_ = false, have_ = false, lost_ = true; Res res_;
};
}

std::unique_ptr<Decoder> makePhaseNoise() { return std::make_unique<PhaseNoiseDec>(); }

} // namespace dect2::x40
