// Fading analyser for one steady carrier (a beacon, a time-signal or broadcast carrier, a bench source) within +-2.8 kHz of the tuned frequency: what the path does to it. The carrier is
// followed (1-2 s FFT windows), shifted to 0 Hz and low-passed at 220 Hz, and the envelope is watched over the last 60 s (first reading after 30 s): the level distribution against the mean power
// (1, 10, 50, 90, 99 % levels, the fade depth), the scintillation index S4 (the square root of var(P) / mean(P)^2), the Rician K-factor by the moment method (K = ((1-g) + sqrt(1-g)) / g with g = var(P) / mean(P)^2, 0 for g >= 1; infinite for a clean carrier), the rate
// at which the level falls through -10 dB under the RMS and the average length of such a fade, and the Doppler spread (RMS width of the spectrum of the complex envelope around its centre of gravity).
// Fades deeper than 20 dB, or longer than 0.5 s, are logged. Rayleigh theory gives a crossing rate of 0.72 fd and a fade length of 0.133 / fd at -10 dB (fd the maximum Doppler); the readings can be
// compared with it. Limits: a fading bandwidth above about 200 Hz is cut off (fast mobile paths at UHF); the carrier must stay inside the window; noise adds its own fluctuations (the readings are
// good to a few dB of K above about 25 dB-Hz per Hz of fading, see the tests). Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 16000, kCut = 3000, kSearchHz = 2800, kEnvCut = 220, kWinSec = 60, kMinSec = 30, kEvalSec = 5;

class FadingDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec1_.configure(fs, d, kCut)) return false;
        r1_ = dec1_.outRate(); n1_ = 1; while (n1_ * 2 <= r1_) n1_ *= 2;
        win_.resize(n1_); for (int i = 0; i < n1_; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n1_));
        const int d2 = std::max(1, (int)std::floor(r1_ / 1000.0));
        if (!dec2_.configure(r1_, d2, kEnvCut)) return false;
        r2_ = dec2_.outRate();
        reset(); return true;
    }
    void reset() override {
        dec1_.reset(); dec2_.reset(); buf_.clear(); z_.clear(); fc_ = 0; haveFc_ = false; ph_ = 0; t_ = 0; skip_ = (size_t)(0.2 * r1_); nEnv_ = 0; fadeOn_ = false; fadeMin_ = 0; fadeStart_ = 0;
        lastEval_ = -1e9; lastRow_ = -1.0; have_ = false; res_ = Res(); lost_ = true; cn0_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        dec1_.process(x, n, [&](cf32 a) {
            t_ += 1.0 / r1_;
            if (skip_) { skip_--; return; }
            buf_.push_back(a); if ((int)buf_.size() >= n1_) { track(); buf_.clear(); }
            if (!haveFc_) return;
            ph_ -= 2 * kPi * fc_ / r1_; if (ph_ < -kPi) ph_ += 2 * kPi;
            const cf32 m = a * cf32((float)std::cos(ph_), (float)std::sin(ph_));
            dec2_.process(&m, 1, [&](cf32 e) { envelope(e); });
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (lost_) { t.headline = "no carrier in +-2.8 kHz of the tuned frequency"; return; }
        if (!res_.ok) { t.headline = fmt("collecting: %.0f of %.0f s", std::min((double)z_.size() / r2_, kMinSec), kMinSec); return; }
        t.headline = fmt("%s, 10 %% of the time below %.1f dB, fade rate %.2f per s, Doppler spread %.1f Hz", res_.kdb >= 39.9 ? "steady carrier" : fmt("K %.1f dB", res_.kdb).c_str(), res_.p10, res_.lcr, res_.dopp);
        t.facts.push_back({"Rician K-factor", res_.kdb >= 39.9 ? std::string("above 40 dB (no fading to speak of)") : res_.k <= 0 ? std::string("0 (Rayleigh: no steady component)") : fmt("%.1f dB (K = %.2f)", res_.kdb, res_.k)});
        t.facts.push_back({"level against the mean power", fmt("1 %%: %.1f, 10 %%: %.1f, 50 %%: %.1f, 90 %%: %.1f, 99 %%: %.1f dB", res_.p1, res_.p10, res_.p50, res_.p90, res_.p99)});
        t.facts.push_back({"scintillation index S4", fmt("%.3f (square root of var(P) / mean(P)^2; 0 steady, 1 Rayleigh; the ionospheric and GNSS scintillation measure)", res_.s4)});
        t.facts.push_back({"fade depth", fmt("%.1f dB (median to 1 %%)", res_.p50 - res_.p1)});
        t.facts.push_back({"fade rate at -10 dB", fmt("%.2f per second", res_.lcr)});
        t.facts.push_back({"average fade length at -10 dB", res_.lcr > 0 ? fmt("%.0f ms", res_.afd * 1000) : std::string("-")});
        t.facts.push_back({"Doppler spread", fmt("%.1f Hz RMS", res_.dopp)});
        t.facts.push_back({"carrier to noise density", fmt("%.1f dB-Hz", cn0_)});
        t.facts.push_back({"window", fmt("%.0f s, envelope at %.0f Hz", std::min((double)z_.size() / r2_, kWinSec), r2_)});
    }
private:
    struct Res { bool ok = false; double s4 = 0, k = 0, kdb = 0, p1 = 0, p10 = 0, p50 = 0, p90 = 0, p99 = 0, lcr = 0, afd = 0, dopp = 0; };
    void track() {
        have_ = true;
        std::vector<cf32> a((size_t)n1_); for (int i = 0; i < n1_; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        const double bh = r1_ / n1_; const int hw = (int)(kSearchHz / bh);
        std::vector<float> p((size_t)n1_); for (int k = 0; k < n1_; k++) p[k] = std::norm(a[k]);
        int kp = 0; float pk = 0; for (int d = -hw; d <= hw; d++) { const int k = (d + n1_) % n1_; if (p[k] > pk) { pk = p[k]; kp = k; } }
        std::vector<float> nb; for (int d = -(int)(kCut / bh); d <= (int)(kCut / bh); d++) { const int k = (d + n1_) % n1_; if (std::abs(((k - kp + n1_ / 2) % n1_ + n1_) % n1_ - n1_ / 2) > 400) nb.push_back(p[k]); }
        if (nb.empty()) return;
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end()); const double noise = std::max((double)nb[nb.size() / 2], 1e-30) / 0.693;
        // the carrier and its scatter sidebands: the power within +-250 Hz of the peak against the noise there
        double pw = 0; const int sw = (int)(250 / bh); for (int d = -sw; d <= sw; d++) pw += p[(kp + d + n1_) % n1_];
        const bool present = pw > 30.0 * noise * (2 * sw + 1) * 0.1 && pk > 20.0 * noise;
        if (!present) { if (!lost_) { lost_ = true; res_ = Res(); z_.clear(); out->row("Carrier", "lost", "no carrier in the window", -120.f); } out->energy(); return; }
        // the centre of gravity of the power within +-250 Hz (the carrier with its scatter) is where to mix to; smoothed
        double sf = 0, sp = 0; for (int d = -sw; d <= sw; d++) { const double q = p[(kp + d + n1_) % n1_]; sf += q * d * bh; sp += q; }
        const double f = kp * bh > r1_ / 2 ? (kp - n1_) * bh + sf / sp : kp * bh + sf / sp;
        const double fnew = (kp > n1_ / 2 ? (double)(kp - n1_) * bh : (double)kp * bh) + sf / sp;
        (void)f;
        fc_ = haveFc_ ? 0.5 * fc_ + 0.5 * fnew : fnew; haveFc_ = true;
        cn0_ = (float)(10 * std::log10(std::max(pw / noise / (2 * sw + 1) * 1.5 * r1_ / n1_ * (2 * sw + 1), 1e-9)));
        if (lost_) { lost_ = false; out->row("Carrier", "found", fmt("%+.1f Hz from the tuned frequency", fc_), cn0_); }
        out->good(cn0_, fc_);
    }
    void envelope(cf32 e) {
        if (nEnv_ < (size_t)(3 * r2_)) { nEnv_++; return; }          // the filters settle
        z_.push_back(e);
        if (z_.size() > (size_t)(kWinSec * r2_)) z_.pop_front();
        // fades: below -10 dB of the mean power of the window (once there is a window)
        if (res_.ok) {
            const double lv = 10 * std::log10((double)std::norm(e) / meanP_ + 1e-12), tt = t_;
            if (!fadeOn_ && lv < -10.0) { fadeOn_ = true; fadeMin_ = lv; fadeStart_ = tt; }
            else if (fadeOn_) { fadeMin_ = std::min(fadeMin_, lv); if (lv > -8.0) { fadeOn_ = false; const double d = tt - fadeStart_; if (fadeMin_ < -20.0 || d > 0.5) out->row("Fade", fmt("%.0f dB", fadeMin_), fmt("%.0f ms below -10 dB, deepest %.1f dB under the mean power", d * 1000, fadeMin_), (float)lv); } }
        }
        if (t_ - lastEval_ >= kEvalSec && z_.size() >= (size_t)(kMinSec * r2_)) { lastEval_ = t_; evaluate(); }
    }
    void evaluate() {
        const size_t N = z_.size();
        std::vector<float> P(N); double mp = 0; for (size_t i = 0; i < N; i++) { P[i] = std::norm(z_[i]); mp += P[i]; } mp /= (double)N; meanP_ = mp;
        double m2 = 0; for (float v : P) m2 += ((double)v - mp) * ((double)v - mp); m2 /= (double)N;
        Res r; const double g = m2 / (mp * mp); r.s4 = std::sqrt(std::max(0.0, g));
        r.k = g >= 1 ? 0 : g < 1e-4 ? 1e4 : ((1 - g) + std::sqrt(1 - g)) / g; r.kdb = r.k > 0 ? 10 * std::log10(r.k) : -99;
        std::vector<float> s(P); std::sort(s.begin(), s.end());
        auto q = [&](double f) { return 10 * std::log10((double)s[std::min(N - 1, (size_t)(f * (double)N))] / mp + 1e-12); };
        r.p1 = q(0.01); r.p10 = q(0.10); r.p50 = q(0.50); r.p90 = q(0.90); r.p99 = q(0.99);
        // crossings downwards through -10 dB under the mean power, and the time spent below
        const double thr = 0.1 * mp; long cross = 0, below = 0; bool lo = P[0] < thr;
        for (size_t i = 1; i < N; i++) { const bool b = P[i] < thr; if (b) below++; if (b && !lo) cross++; lo = b; }
        const double T = (double)N / r2_; r.lcr = cross / T; r.afd = cross ? (below / r2_) / cross : 0;
        // Doppler spread: Welch of the complex envelope, segments of 2048 with 50 % overlap
        const int L = 2048; std::vector<double> ps((size_t)L, 0.0); int ns = 0;
        std::vector<cf32> w((size_t)L);
        for (size_t o = 0; o + (size_t)L <= N; o += (size_t)L / 2) { for (int i = 0; i < L; i++) w[i] = z_[o + (size_t)i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / L)); fftx(w); for (int k = 0; k < L; k++) ps[k] += std::norm(w[k]); ns++; }
        double sw = 0, sf = 0, sff = 0; const double bh = r2_ / L;
        for (int k = 0; k < L; k++) { const double f = (k < L / 2 ? k : k - L) * bh; if (std::fabs(f) > kEnvCut * 0.9) continue; sw += ps[k]; sf += ps[k] * f; sff += ps[k] * f * f; }
        if (sw > 0 && ns > 0) { const double mean = sf / sw; r.dopp = std::sqrt(std::max(0.0, sff / sw - mean * mean)); }
        r.ok = true; res_ = r;
        if (t_ - lastRow_ >= 30.0 || lastRow_ < 0) { lastRow_ = t_;
        out->row("Fading", fmt("K %.1f dB", r.kdb > 40 ? 40.0 : r.kdb), fmt("10 %% of the time below %.1f dB, fade rate %.2f per s, fade length %.0f ms, Doppler spread %.1f Hz RMS", r.p10, r.lcr, r.afd * 1000, r.dopp), cn0_); }
    }
    Decimator dec1_, dec2_; double r1_ = 1, r2_ = 1, fc_ = 0, ph_ = 0, t_ = 0, lastEval_ = -1e9, lastRow_ = -1.0, fadeMin_ = 0, fadeStart_ = 0, meanP_ = 1; int n1_ = 1; size_t skip_ = 0, nEnv_ = 0; float cn0_ = 0;
    std::vector<float> win_; std::vector<cf32> buf_; std::deque<cf32> z_; bool haveFc_ = false, have_ = false, lost_ = true, fadeOn_ = false; Res res_;
};
}

std::unique_ptr<Decoder> makeFading() { return std::make_unique<FadingDec>(); }

} // namespace dect2::x40
