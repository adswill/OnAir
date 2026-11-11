// Spectral kurtosis RFI finder (the estimator of Nita and Gary, used in radio astronomy): for a wide capture, which frequencies hold something that is not Gaussian noise, and whether it is a steady carrier or
// pulsed. A 1024-point Hann spectrum per frame; per bin, over M = 128 frames, SK = (M + 1) / (M - 1) x (M x S2 / S1^2 - 1) with S1 the sum of the bin powers and S2 the sum of their squares; averaged over 8 such blocks.
// For Gaussian noise SK = 1 with a standard deviation of about sqrt(4 / M) / sqrt(8) = 0.06; a bin is flagged outside 1 +- 4.5 standard deviations. SK well under 1 means the power of the bin hardly changes (a steady
// carrier: SK tends to 0 when the carrier dominates the bin), well over 1 means the power comes in bursts: for a pulsed signal much stronger than the noise during its pulses, SK is about 1 / q - 1 where q is the share of the 0.5 ms frames in which it is at full power (a pulse shorter than a frame counts as a frame, and the Hann window weakens the frames it only touches, so q is a little under (pulse + frame) / period). Its use is the
// class (pulsed or steady) of whatever is not noise: a radar, a TDMA burst, an arcing insulator, a switching supply. In the tests a pulsed signal 3.7 dB over the noise in its averaged bin was flagged and one 1.3 dB over was not. Output: the share of the band that is clean, and up to 8 flagged regions (adjacent
// flagged bins merged) with their kind, SK, the share of frames at full power implied for the pulsed ones (1 / (SK + 1)) and the mean level over the noise. The adjacent bins of a Hann spectrum are correlated, which is why the flags are judged on regions, not single bins.
// It does not say what the source is. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr int kFft = 1024, kM = 128, kBlocks = 8;
constexpr double kSigma = 4.5;

struct Region { double lo = 0, hi = 0, sk = 0, over = 0; int kind = 0; int stable = 0, reported = 0; };   // kind: 1 pulsed, -1 steady

class SpecKurtDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs; binHz_ = fs / kFft;
        win_.resize(kFft); for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        reset(); return true;
    }
    void reset() override { buf_.clear(); s1_.assign(kFft, 0.0); s2_.assign(kFft, 0.0); skAcc_.assign(kFft, 0.0); pAcc_.assign(kFft, 0.0); nFr_ = 0; nBl_ = 0; have_ = false; regs_.clear(); clean_ = 1; t_ = 0; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override { for (size_t i = 0; i < n; i++) { buf_.push_back(x[i]); if ((int)buf_.size() == kFft) { frame(); buf_.clear(); } } }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "collecting"; return; }
        t.headline = fmt("%.1f %% of the band is Gaussian-clean; %zu flagged region(s)", 100 * clean_, regs_.size());
        t.facts.push_back({"clean part of the band", fmt("%.1f %% of the bins within SK = 1 +- %.1f standard deviations (%.3f)", 100 * clean_, kSigma, kSigma * sigma())});
        for (size_t i = 0; i < regs_.size() && i < 8; i++) {
            const Region& r = regs_[i];
            t.facts.push_back({fmt("region %zu", i + 1), fmt("%s to %s: %s, SK %.2f%s, %.1f dB over the noise (mean)", f(r.lo).c_str(), f(r.hi).c_str(), r.kind > 0 ? "pulsed" : "steady carrier", r.sk, r.kind > 0 && r.sk > 1.5 ? fmt(" (strong in about %.0f %% of the 0.5 ms frames)", 100.0 / (r.sk + 1)).c_str() : "", r.over)});
        }
        t.facts.push_back({"estimator", fmt("M = %d frames of %d points, %d blocks averaged, %.0f ms per block", kM, kFft, kBlocks, 1000.0 * kM * kFft / fs_)});
    }
private:
    double sigma() const { return std::sqrt(4.0 / kM / kBlocks); }
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.4f MHz", (centre_ + hz) / 1e6) : fmt("%+.1f kHz", hz / 1000); }
    void frame() {
        std::vector<cf32> a(kFft); for (int i = 0; i < kFft; i++) a[i] = buf_[i] * win_[i]; fftx(a);
        for (int k = 0; k < kFft; k++) { const double p = std::norm(a[k]); s1_[k] += p; s2_[k] += p * p; }
        if (++nFr_ < kM) return;
        nFr_ = 0;
        for (int k = 0; k < kFft; k++) { const double sk = (kM + 1.0) / (kM - 1.0) * (kM * s2_[k] / std::max(s1_[k] * s1_[k], 1e-300) - 1.0); skAcc_[k] += sk; pAcc_[k] += s1_[k] / kM; s1_[k] = s2_[k] = 0; }
        if (++nBl_ < kBlocks) return;
        nBl_ = 0; have_ = true; t_ += (double)kBlocks * kM * kFft / fs_;
        evaluate();
        std::fill(skAcc_.begin(), skAcc_.end(), 0.0); std::fill(pAcc_.begin(), pAcc_.end(), 0.0);
    }
    void evaluate() {
        std::vector<double> sk((size_t)kFft), pw((size_t)kFft);
        for (int k = 0; k < kFft; k++) { sk[(size_t)k] = skAcc_[k] / kBlocks; pw[(size_t)k] = pAcc_[k] / kBlocks; }
        std::vector<double> m(pw); std::nth_element(m.begin(), m.begin() + kFft / 2, m.end()); const double noiseP = std::max(m[kFft / 2], 1e-30) / 0.693;
        const double sg = sigma(), lo = 1 - kSigma * sg, hi = 1 + kSigma * sg;
        // flag bins (the middle 90 % of the span, not the DC bins)
        std::vector<int> flag((size_t)kFft, 0); int used = 0, bad = 0;
        for (int k = 0; k < kFft; k++) { const int j = k < kFft / 2 ? k : k - kFft; if (std::abs(j) < 3 || std::abs(j) > (int)(0.45 * kFft)) continue; used++; if (sk[(size_t)k] > hi) { flag[(size_t)k] = 1; bad++; } else if (sk[(size_t)k] < lo) { flag[(size_t)k] = -1; bad++; } }
        clean_ = used ? 1.0 - (double)bad / used : 1.0;
        std::vector<Region> regs;
        for (int j = -(int)(0.45 * kFft); j <= (int)(0.45 * kFft);) {
            const int k = (j + kFft) % kFft; if (!flag[(size_t)k]) { j++; continue; }
            const int kind = flag[(size_t)k]; int e = j; double skm = 0, pm = 0; int c = 0; int gap = 0;
            for (int q = j; q <= (int)(0.45 * kFft); q++) { const int kk = (q + kFft) % kFft; if (flag[(size_t)kk] == kind) { e = q; gap = 0; skm += sk[(size_t)kk]; pm += pw[(size_t)kk]; c++; } else if (++gap > 2) break; }
            Region r; r.lo = (j - 0.5) * binHz_; r.hi = (e + 0.5) * binHz_; r.sk = skm / c; r.kind = kind; r.over = 10 * std::log10(std::max(pm / c / noiseP, 1e-9));
            if (c >= 2) regs.push_back(r); j = e + 1;
        }
        std::sort(regs.begin(), regs.end(), [](const Region& a, const Region& b) { return std::fabs(a.sk - 1) > std::fabs(b.sk - 1); });
        for (auto& r : regs) {
            for (auto& o : regs_) if (o.kind == r.kind && std::fabs(0.5 * (o.lo + o.hi) - 0.5 * (r.lo + r.hi)) < 4 * binHz_) { r.stable = o.stable + 1; r.reported = o.reported; }
            if (r.stable >= 2 && !r.reported) { r.reported = 1; out->row(r.kind > 0 ? "Pulsed" : "Steady", f(0.5 * (r.lo + r.hi)), fmt("%s to %s: SK %.2f%s, %.1f dB over the noise", f(r.lo).c_str(), f(r.hi).c_str(), r.sk, r.kind > 0 && r.sk > 1.5 ? fmt(", strong in about %.0f %% of the frames", 100.0 / (r.sk + 1)).c_str() : "", r.over), (float)r.over); }
        }
        for (auto& o : regs_) if (o.reported) { bool still = false; for (auto& r : regs) if (r.kind == o.kind && std::fabs(0.5 * (o.lo + o.hi) - 0.5 * (r.lo + r.hi)) < 4 * binHz_) still = true; if (!still) out->row("Gone", f(0.5 * (o.lo + o.hi)), "no longer flagged", 0.f); }
        regs_ = regs;
        if (!regs_.empty()) out->bad(); else out->good((float)(100 * clean_), 0);
    }
    double fs_ = 1, binHz_ = 1, centre_ = 0, t_ = 0, clean_ = 1; int nFr_ = 0, nBl_ = 0; bool have_ = false;
    std::vector<float> win_; std::vector<cf32> buf_; std::vector<double> s1_, s2_, skAcc_, pAcc_; std::vector<Region> regs_;
};
}

std::unique_ptr<Decoder> makeSpecKurt() { return std::make_unique<SpecKurtDec>(); }

} // namespace dect2::x40
