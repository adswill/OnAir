// Power-line and impulse noise analyser (LF to VHF, any tuned frequency): the interference that arcing insulators, faulty street-lamp ballasts, dimmers and switch-mode supplies put
// on a radio band. Impulses are found in the envelope (a robust level and spread follow the noise, an impulse is a peak over level + 6.5 spreads), and the spacing between impulses
// is searched for a repetition: a rate near twice the mains frequency (100 / 120 Hz) is the signature of a power-line fault (an arc at every half-cycle), a rate near the mains
// frequency (50 / 60 Hz) a half-wave one, any other steady rate a switching supply or a clock. Impulses without a rate are static or lightning. The mode measures the impulse
// rate, the level over the noise and the repetition rate; it cannot tell the pole from the lamp: for that walk with an antenna and watch the level. Receive only.
#include "x40_int.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 48000, kCut = 10000, kReportSec = 2.0, kWindowSec = 10.0;
constexpr float kSigmas = 6.5f;
constexpr double kMaxLag = 0.060, kBin = 0.0001;

class PlAnalyser : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); reset(); return true;
    }
    void reset() override {
        dec_.reset(); n_ = 0; warm_ = 0; mean_ = 0; var_ = 0; init_ = false; in_ = false; imp_.clear(); nextReport_ = kReportSec; haveRep_ = false;
        pattern_ = false; fHz_ = 0; cls_.clear(); cnt_ = 0; lost_ = 0; rate10_ = 0; medDb_ = 0; spreadDb_ = 0; nPeaks_ = 0; strength_ = 0; hits_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { step(std::abs(a)); });
    }
    void fill(X40Telemetry& t) const override {
        if (!haveRep_) { t.headline = "waiting for data"; return; }
        t.headline = pattern_ ? fmt("%s at %.2f Hz, %.1f impulses/s, %.0f dB over the noise", cls_.c_str(), fHz_, rate10_, medDb_)
                              : (rate10_ >= 0.5 ? fmt("impulses without a repetition (%.1f /s, %.0f dB): static or lightning", rate10_, medDb_) : "no impulses");
        t.facts.push_back({"impulse rate", fmt("%.1f per second (last %.0f s)", rate10_, kWindowSec)});
        t.facts.push_back({"impulse level", rate10_ > 0 ? fmt("%.1f dB over the noise (median), spread %.1f dB", medDb_, spreadDb_) : std::string("none")});
        t.facts.push_back({"repetition", pattern_ ? fmt("%.2f Hz (%s), %.0f %% of the impulses fall on it", fHz_, cls_.c_str(), strength_ * 100) : std::string("none found")});
        t.facts.push_back({"impulses counted", fmt("%llu", (unsigned long long)nPeaks_)});
    }
private:
    struct Imp { double t; float db; };
    void step(float e) {
        n_++;
        const double t = n_ / rate_;
        if (!init_) { init_ = true; mean_ = e; var_ = 0; return; }
        if (t < 0.2) { mean_ = e; var_ = 0; return; }       // filter start-up
        if (warm_ < (uint64_t)(0.3 * rate_)) { warm_++; const float a0 = 1.f / (float)warm_; const float d0 = e - mean_; mean_ += a0 * d0; var_ += a0 * (d0 * d0 - var_); return; }   // learn the noise first
        const float thr = mean_ + kSigmas * std::sqrt(std::max(var_, 0.f)) + 1e-12f;
        if (e > thr) {
            if (!in_) { in_ = true; pkT_ = t; pk_ = e; } else if (e > pk_) { pk_ = e; pkT_ = t; }
            return;
        }
        if (in_) {                                           // the impulse ended
            in_ = false; nPeaks_++;
            imp_.push_back({pkT_, 20.f * std::log10(std::max(pk_, 1e-12f) / std::max(mean_, 1e-12f))});
            out->energy();
        }
        warm_++;
        const float a = std::max(1.f / (float)(0.5 * rate_), 1.f / (float)warm_);      // noise level and spread, about 0.5 s (faster while it is first learned)
        const float d = e - mean_; mean_ += a * d; var_ += a * (d * d - var_);
        while (!imp_.empty() && imp_.front().t < t - kWindowSec) imp_.erase(imp_.begin());
        if (t >= nextReport_) { nextReport_ = t + kReportSec; report(t); }
    }
    void report(double t) {
        haveRep_ = true;
        const double span = std::min(kWindowSec, t - 0.2);
        const size_t N = imp_.size();
        rate10_ = (float)(N / span);
        std::vector<float> lv; for (auto& i : imp_) lv.push_back(i.db);
        medDb_ = 0; spreadDb_ = 0;
        if (!lv.empty()) { std::sort(lv.begin(), lv.end()); medDb_ = lv[lv.size() / 2]; spreadDb_ = lv[lv.size() * 3 / 4] - lv[lv.size() / 4]; }
        // histogram of all spacings up to 60 ms
        bool found = false; double f = 0, strength = 0;
        if (N >= 30) {
            const int NB = (int)(kMaxLag / kBin);
            std::vector<int> h(NB + 1, 0); long pairs = 0;
            for (size_t i = 0; i < N; i++) for (size_t j = i + 1; j < N; j++) { const double dt = imp_[j].t - imp_[i].t; if (dt > kMaxLag) break; if (dt >= 0.0015) { h[(int)(dt / kBin)]++; pairs++; } }
            if (pairs > 0) {
                // smooth over three bins, find the strongest, then the smallest lag with at least 60 % of it
                std::vector<double> s(NB + 1, 0); for (int i = 1; i < NB; i++) s[i] = (h[i - 1] + 2.0 * h[i] + h[i + 1]) / 4.0;
                const double meanBin = (double)pairs / NB; double best = 0; int bi = 0;
                for (int i = 15; i < NB; i++) if (s[i] > best) { best = s[i]; bi = i; }
                int pick = bi;
                for (int i = 30; i < bi; i++) if (s[i] >= 0.6 * best && s[i] >= s[i - 1] && s[i] >= s[i + 1]) { pick = i; break; }   // fundamental (>= 3 ms: the highest rate read is 330 Hz)
                if (best > 4.0 * meanBin && best >= 12) {
                    // refine: the mean of the spacings within 4 % of the pick
                    const double c = (pick + 0.5) * kBin; double sum = 0; long k = 0;
                    for (size_t i = 0; i < N; i++) for (size_t j = i + 1; j < N; j++) { const double dt = imp_[j].t - imp_[i].t; if (dt > c * 1.04) break; if (dt > c * 0.96) { sum += dt; k++; } }
                    if (k > 0) { f = 1.0 / (sum / k); found = true; strength = std::min(1.0, (double)k / (double)N); }
                }
            }
        }
        strength_ = (float)strength;
        std::string cls;
        if (found) {
            auto nearL = [&](double v, double c) { return std::fabs(v - c) < 0.02 * c + 0.3; };
            if (nearL(f, 100) || nearL(f, 120)) cls = fmt("power-line noise (twice the %.0f Hz mains)", f > 110 ? 60.0 : 50.0);
            else if (nearL(f, 50) || nearL(f, 60)) cls = fmt("half-wave power-line noise (the %.0f Hz mains)", f > 55 ? 60.0 : 50.0);
            else cls = fmt("steady impulses (a switching supply or a clock)");
        }
        // a pattern must hold for two reports before it is logged; it is lost after three without it
        if (found) {
            lost_ = 0;
            const bool same = pattern_ && std::fabs(f - fHz_) < 0.02 * fHz_ + 0.3;
            if (!pattern_ || !same) {
                if (cand_ > 0 && std::fabs(f - candF_) < 0.02 * candF_ + 0.3) cnt_++; else { cnt_ = 1; candF_ = f; }
                cand_ = 1;
                if (cnt_ >= 2) {
                    pattern_ = true; fHz_ = (float)f; cls_ = cls; cnt_ = 0; cand_ = 0;
                    out->good(medDb_, 0);
                    out->row("Pattern", fmt("%.2f Hz", f), fmt("%s: %.1f impulses/s, %.0f dB over the noise (median), %.0f %% of the impulses on this rate", cls.c_str(), rate10_, medDb_, strength * 100), medDb_);
                }
            } else { fHz_ += 0.2f * ((float)f - fHz_); cls_ = cls; out->good(medDb_, 0); hits_++; }
        } else {
            cnt_ = 0; cand_ = 0;
            if (pattern_ && ++lost_ >= 3) { pattern_ = false; out->row("Pattern", "gone", fmt("the repetition at %.2f Hz is no longer there (%.1f impulses/s)", fHz_, rate10_), medDb_); }
        }
    }
    Decimator dec_; double rate_ = 1, pkT_ = 0, nextReport_ = kReportSec, candF_ = 0; uint64_t n_ = 0, nPeaks_ = 0, warm_ = 0; float mean_ = 0, var_ = 0, pk_ = 0, medDb_ = 0, spreadDb_ = 0, rate10_ = 0, fHz_ = 0, strength_ = 0;
    bool init_ = false, in_ = false, haveRep_ = false, pattern_ = false; int cnt_ = 0, lost_ = 0, cand_ = 0, hits_ = 0; std::string cls_; std::vector<Imp> imp_;
};
}

std::unique_ptr<Decoder> makePlNoise() { return std::make_unique<PlAnalyser>(); }

} // namespace dect2::x40
