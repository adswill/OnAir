// GNSS interference detector (L1 1575.42 MHz, L2 1227.6 MHz, B1, G1 and others): looks at a wide capture of a navigation band and says whether something other than the satellites'
// own noise-like signals is in it, and what kind: a continuous tone (CW), narrowband interference, a swept tone (the sawtooth chirp of a car-mounted jammer: sweep range, period, rate),
// or a wideband noise-like jammer. The satellites are 20 dB under the thermal noise, so a clean band shows a flat spectrum and a steady envelope. Once a second, 65536 samples are
// taken: a Welch spectrum (2048 points) gives the interference-to-noise ratio, how much of the band is raised, and the width of the strongest line; the instantaneous frequency
// of the capture, when something dominates it, gives the sweep. It detects and describes interference; it cannot say where it is from, whether it is intended, or who sent it.
// Receive only: no navigation message is decoded and no position is computed. Needs a GNSS-capable radio with at least 2 MHz of bandwidth.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr int kN = 2048, kBlock = 65536;
constexpr double kReportSec = 1.0;

class JamDec : public Decoder {
public:
    bool configure(double fs) override { rate_ = fs; wl_.assign(kN, 0.f); for (int i = 0; i < kN; i++) wl_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kN)); reset(); return true; }
    void reset() override {
        buf_.clear(); base_ = 0; n_ = 0; collecting_ = false; nextAt_ = 0.2; have_ = false; cls_ = "clean"; cand_.clear(); cnt_ = 0; inrDb_ = 0; spanHz_ = 0; periodUs_ = 0; freqHz_ = 0; widthHz_ = 0; rated_ = 0; fracUp_ = 0; seconds_ = 0; events_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            n_++;
            if (collecting_) { buf_.push_back(x[i]); if (buf_.size() >= (size_t)kBlock) { analyse(); buf_.clear(); collecting_ = false; nextAt_ = n_ / rate_ + kReportSec - kBlock / rate_; } }
            else if (n_ / rate_ >= nextAt_) { collecting_ = true; buf_.push_back(x[i]); }
        }
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        t.headline = cls_ == "clean" ? "no interference" : fmt("%s, %.0f dB over the noise", cls_.c_str(), inrDb_);
        t.facts.push_back({"state", cls_});
        t.facts.push_back({"interference over the noise", fmt("%.1f dB", inrDb_)});
        t.facts.push_back({"band raised", fmt("%.0f %% of the band is more than 6 dB over the floor", fracUp_ * 100)});
        if (cls_ == "continuous tone" || cls_ == "narrowband interference") t.facts.push_back({"strongest line", fmt("%+.3f MHz from the centre, width %.1f kHz", freqHz_ / 1e6, widthHz_ / 1e3)});
        if (cls_.rfind("swept", 0) == 0) {
            t.facts.push_back({"sweep range", fmt("%.2f MHz (%+.2f to %+.2f MHz from the centre)", spanHz_ / 1e6, (freqHz_ - spanHz_ / 2) / 1e6, (freqHz_ + spanHz_ / 2) / 1e6)});
            t.facts.push_back({"sweep period", fmt("%.1f us (%.0f kHz repetition)", periodUs_, 1e3 / std::max(periodUs_, 1e-9))});
            t.facts.push_back({"sweep rate", fmt("%.2f MHz per us", rated_ / 1e12)});
        }
        t.facts.push_back({"interference seconds", fmt("%d of %d", events_, seconds_)});
    }
private:
    void analyse() {
        have_ = true; seconds_++;
        // Welch spectrum of the block
        std::vector<double> psd(kN, 0.0); std::vector<cf32> a(kN); int segs = 0;
        for (size_t o = 0; o + kN <= buf_.size(); o += kN / 2) {
            for (int i = 0; i < kN; i++) a[i] = buf_[o + i] * wl_[i];
            fftx(a); for (int k = 0; k < kN; k++) psd[k] += std::norm(a[k]); segs++;
        }
        for (auto& v : psd) v /= segs;
        std::vector<double> s(psd); std::sort(s.begin(), s.end());
        // the noise floor from the 20th percentile bin (a chirp or a band of noise raises up to 80 % of the bins; the averaged noise bins have a relative spread of 1 / sqrt(0.55 segments))
        const double sd = 1.0 / std::sqrt(0.55 * segs);
        const double floorP = std::max(s[kN / 5] / (1.0 - 0.84 * sd), 1e-30);       // mean noise bin
        double total = 0; for (double v : psd) total += v;
        const double noiseTotal = floorP * kN;
        const double inr = 10 * std::log10(std::max(total / noiseTotal - 1.0, 1e-6));
        // the lowest noise level seen so far (slowly forgotten): a flat rise of it is a wideband jammer, or a change of the radio's gain
        if (base_ <= 0 || noiseTotal < base_) base_ = noiseTotal; else base_ *= 1.0023;      // 0.01 dB per second
        const double rise = 10 * std::log10(noiseTotal / base_);
        int up6 = 0; for (double v : psd) if (v > floorP * 2.5) up6++;
        fracUp_ = (float)up6 / kN;
        int pk = 0; for (int k = 1; k < kN; k++) if (psd[k] > psd[pk]) pk = k;
        // width of the strongest line: contiguous bins over a quarter of its power
        int lo = pk, hi = pk; while (lo > 0 && psd[(lo - 1 + kN) % kN] > 0.25 * psd[pk] && pk - lo < 200) lo--; while (hi < kN - 1 && psd[hi + 1] > 0.25 * psd[pk] && hi - pk < 200) hi++;
        const double bin = rate_ / kN; const int kc = pk < kN / 2 ? pk : pk - kN;
        const double pkOver = 10 * std::log10(std::max(psd[pk] / floorP, 1e-9));
        const bool floorUp = rise > 3.0 && fracUp_ < 0.5;
        std::string cls = "clean"; double span = 0, period = 0, center = kc * bin, width = (hi - lo + 1) * bin, rate = 0;
        double wideInr = 0;
        if (inr > 3.0 || pkOver > 15.0 || floorUp) {
            const bool tone = pkOver > 15.0 && (hi - lo + 1) <= 5 && fracUp_ < 0.05;
            if (tone) cls = "continuous tone";
            else {
                // is the capture dominated by one swept signal? the instantaneous frequency autocorrelates
                if (sweep(span, period, center)) { cls = "swept tone (chirp jammer)"; rate = span / (period * 1e-6); }
                else if (floorUp) { cls = "wideband noise-like interference"; wideInr = 10 * std::log10(std::max(std::pow(10.0, rise / 10) - 1.0, 1e-6)); }
                else if (fracUp_ > 0.6) cls = "wideband noise-like interference";
                else if (width < 0.1 * rate_) cls = "narrowband interference";
                else cls = "wideband noise-like interference";
            }
        }
        // a class must hold for two seconds to be logged
        inrDb_ = (float)std::max(cls == "wideband noise-like interference" && wideInr > 0 ? wideInr : inr, 0.0); spanHz_ = span; periodUs_ = period; freqHz_ = center; widthHz_ = width; rated_ = rate;
        if (cls != "clean") events_++;
        if (cls == cls_) { cnt_ = 0; cand_.clear(); if (cls != "clean") out->good(inrDb_, 0); }
        else {
            if (cand_ == cls) cnt_++; else { cand_ = cls; cnt_ = 1; }
            if (cnt_ >= 2 || (cls_ == "clean" && cls != "clean" && inr > 10 && cnt_ >= 1 && false)) {
                const std::string prev = cls_; cls_ = cls; cnt_ = 0; cand_.clear();
                if (cls == "clean") out->row("State", "clean", "the interference has stopped", -120.f);
                else {
                    out->good(inrDb_, 0);
                    std::string text;
                    if (cls == "continuous tone") text = fmt("a tone at %+.3f MHz from the centre, %.1f dB over the noise", center / 1e6, inrDb_);
                    else if (cls == "narrowband interference") text = fmt("%.0f kHz wide at %+.3f MHz from the centre, %.1f dB over the noise", width / 1e3, center / 1e6, inrDb_);
                    else if (cls[0] == 's') text = fmt("sweeping %.2f MHz around %+.2f MHz every %.1f us (%.2f MHz per us), %.1f dB over the noise", span / 1e6, center / 1e6, period, rate / 1e12, inrDb_);
                    else text = fmt("raises %.0f %% of the band, %.1f dB over the noise", fracUp_ * 100, inrDb_);
                    out->row("Interference", cls, text, inrDb_);
                }
            }
        }
    }
    // the instantaneous frequency of the block, its sawtooth period and range
    bool sweep(double& span, double& periodUs, double& center) {
        const size_t N = buf_.size();
        std::vector<float> f(N - 1); const double k = rate_ / (2 * kPi);
        for (size_t i = 1; i < N; i++) f[i - 1] = (float)(std::arg(buf_[i] * std::conj(buf_[i - 1])) * k);
        // a 3-point median takes the clicks of the phase out
        std::vector<float> m(f.size()); for (size_t i = 1; i + 1 < f.size(); i++) { float a = f[i - 1], b = f[i], c = f[i + 1]; m[i] = std::max(std::min(a, b), std::min(std::max(a, b), c)); } m[0] = f[0]; m.back() = f.back();
        double mu = 0; for (float v : m) mu += v; mu /= (double)m.size();
        std::vector<float> d(m.size()); double e0 = 0; for (size_t i = 0; i < m.size(); i++) { d[i] = (float)(m[i] - mu); e0 += (double)d[i] * d[i]; }
        if (e0 <= 0) return false;
        const int lmax = std::min<int>(8192, (int)(m.size() / 4));
        std::vector<double> r(lmax + 1, 0.0);
        for (int l = 0; l <= lmax; l += 1) { double a = 0; for (size_t i = 0; i + l < d.size(); i += (l > 64 ? 2 : 1)) a += (double)d[i] * d[i + l]; r[l] = a; }
        const double r0 = r[0]; if (r0 <= 0) return false;
        for (auto& v : r) v /= r0;
        int l0 = 1; while (l0 < lmax && r[l0] > 0.0) l0++;                      // first zero crossing: the sweep has decorrelated
        if (l0 >= lmax - 4) return false;
        int bl = l0; double best = -1; for (int l = l0; l <= lmax; l++) if (r[l] > best) { best = r[l]; bl = l; }
        if (best < 0.3) return false;          // noise alone, or a wide jammer, gives under 0.03 at any lag
        // the fundamental: the first peak with at least 85 % of the best
        for (int l = l0 + 1; l < bl; l++) if (r[l] >= 0.85 * best && r[l] >= r[l - 1] && r[l] >= r[l + 1]) { bl = l; break; }
        std::vector<float> sorted(m); std::sort(sorted.begin(), sorted.end());
        const double p5 = sorted[sorted.size() / 20], p95 = sorted[sorted.size() * 19 / 20];
        span = (p95 - p5) / 0.9; center = (p95 + p5) / 2; periodUs = bl / rate_ * 1e6;
        return span > 0.15 * rate_ && span > 0.5e6;
    }
    std::vector<cf32> buf_; std::vector<float> wl_; double rate_ = 1, nextAt_ = 0.2; uint64_t n_ = 0; bool collecting_ = false, have_ = false;
    std::string cls_ = "clean", cand_; int cnt_ = 0, seconds_ = 0, events_ = 0;
    double base_ = 0; float inrDb_ = 0, fracUp_ = 0; double spanHz_ = 0, periodUs_ = 0, freqHz_ = 0, widthHz_ = 0, rated_ = 0;
};
}

std::unique_ptr<Decoder> makeGnssJam() { return std::make_unique<JamDec>(); }

} // namespace dect2::x40
