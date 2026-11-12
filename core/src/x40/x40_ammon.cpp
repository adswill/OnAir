// AM broadcast modulation monitor (LW / MW / SW, 148 kHz - 30 MHz): the envelope of one station read like a modulation monitor does. Once a second: carrier offset from the
// carrier's own phase, the carrier level, the modulation depth (RMS equivalent of a sine, positive and negative peaks, asymmetry), mains hum (50 / 60 Hz and 100 / 120 Hz,
// in dB below the carrier), the audio bandwidth (the frequency below which 99 % of the audio power lies) and the share of the time the negative peaks reach zero carrier (over-modulation).
// Over-modulation and the loss or return of the carrier are logged. A reading of what arrives after the radio and the path (fading and noise are in it), not a type-approval
// instrument; it does not decide whether a station breaks a regulation. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 48000, kCut = 12000, kReportSec = 1.0;

class AmMonitor : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate();
        n_ = 1; while (n_ * 2 <= rate_ / 5.0) n_ *= 2;           // bins of 5 - 10 Hz
        wl_.configure(rate_, n_); gate_.configure(rate_); reset(); return true;
    }
    void reset() override {
        dec_.reset(); gate_.reset(); env_.clear(); t_ = 0; acc_ = cf32(0, 0); prev_ = cf32(1, 0); first_ = true; have_ = false; carrierOn_ = false; cCnt_ = 0; refDb_ = -200;
        carrierDb_ = depthRms_ = depthPos_ = depthNeg_ = humDb_ = -99; offHz_ = 0; bw_ = 0; overFrac_ = 0; nOver_ = 0; wl_.clear(); asym_ = 0; sec_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            t_ += 1.0 / rate_;
            if (t_ < 0.1) { prev_ = a; return; }                                    // filter start-up
            env_.push_back(std::abs(a)); gate_.push(a);
            acc_ += a * std::conj(prev_); prev_ = a;
            if (env_.size() >= (size_t)(kReportSec * rate_)) report();
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        t.headline = carrierOn_ ? fmt("carrier %+.1f Hz off, modulation %.0f %% (+%.0f / -%.0f %%), audio to %.1f kHz", offHz_, depthRms_ * 100, depthPos_ * 100, depthNeg_ * 100, bw_ / 1000) : "no carrier";
        t.facts.push_back({"carrier offset", fmt("%+.2f Hz (a tuning or station error)", offHz_)});
        t.facts.push_back({"carrier level", fmt("%.1f dB (relative)", carrierDb_)});
        t.facts.push_back({"modulation (RMS, as a sine)", fmt("%.0f %%", depthRms_ * 100)});
        t.facts.push_back({"positive / negative peaks", fmt("+%.0f %% / -%.0f %%", depthPos_ * 100, depthNeg_ * 100)});
        t.facts.push_back({"asymmetry", fmt("%+.0f %% (positive peaks over negative, against the mean level of the carrier)", asym_ * 100)});
        t.facts.push_back({"mains hum", humDb_ > -90 ? fmt("%.0f dB below the carrier at %s", -humDb_, humHz_) : std::string("none seen")});
        t.facts.push_back({"audio bandwidth", fmt("99 %% of the audio power below %.1f kHz", bw_ / 1000)});
        t.facts.push_back({"over-modulation", fmt("negative peaks at zero carrier for %.1f %% of the last second; %d seconds in all", overFrac_ * 100, nOver_)});
    }
private:
    void report() {
        const size_t N = env_.size();
        double mean = 0; for (float v : env_) mean += v; mean /= (double)N;
        const float lvl = toDb((float)(mean * mean));
        carrierDb_ = lvl;
        // the carrier is present when the mean envelope is well over the spread of the envelope's own noise floor: judged against the previous level
        const bool on = mean > 1e-6 && gate_.present();   // a line of the carrier's share, not just any level (noise has a mean envelope too)
        std::vector<float> a(env_); for (auto& v : a) v = (float)(v - mean);
        double ms = 0; for (float v : a) ms += (double)v * v;
        const double rms = std::sqrt(ms / (double)N);
        depthRms_ = on ? (float)(std::sqrt(2.0) * rms / mean) : 0;
        // peaks: third largest / smallest in blocks of 20 ms, then the largest of the blocks
        const size_t B = std::max<size_t>(8, (size_t)(0.02 * rate_));
        float pk = 0, tr = 0;
        double lowN = 0;
        for (size_t o = 0; o + B <= N; o += B) {
            float u[3] = {-1e30f, -1e30f, -1e30f}, l[3] = {1e30f, 1e30f, 1e30f};
            for (size_t i = o; i < o + B; i++) {
                const float v = env_[i];
                if (v > u[0]) { u[2] = u[1]; u[1] = u[0]; u[0] = v; } else if (v > u[1]) { u[2] = u[1]; u[1] = v; } else if (v > u[2]) u[2] = v;
                if (v < l[0]) { l[2] = l[1]; l[1] = l[0]; l[0] = v; } else if (v < l[1]) { l[2] = l[1]; l[1] = v; } else if (v < l[2]) l[2] = v;
            }
            pk = std::max(pk, u[2]); tr = (o == 0) ? l[2] : std::min(tr, l[2]);
            if (l[2] < 0.03 * mean) lowN += (double)B;
        }
        depthPos_ = on ? (float)((pk - mean) / mean) : 0;
        depthNeg_ = on ? (float)std::min(1.0, (mean - tr) / mean) : 0;
        asym_ = depthPos_ + depthNeg_ > 0 ? (depthPos_ - depthNeg_) / (depthPos_ + depthNeg_) : 0;
        overFrac_ = (float)(lowN / (double)N);
        // carrier offset from the mean phase step of the complex signal
        offHz_ = (float)(std::atan2(acc_.imag(), acc_.real()) * rate_ / (2 * kPi)); acc_ = cf32(0, 0);
        // spectrum of the audio part
        wl_.clear();
        for (size_t o = 0; o + n_ <= N; o += n_ / 2) wl_.add(a.data() + o);
        const double car2 = mean * mean / 2;   // "power" of a carrier of the mean amplitude, same units as the sine power from the spectrum
        const double tot = wl_.band(30, std::min(kCut, rate_ / 2 - 100));
        double h = 0, hz = 0; humHz_[0] = 0;
        const double cand[4] = {50, 100, 60, 120};
        for (double f : cand) { const double p = wl_.band(f - 3 * wl_.binHz(), f + 3 * wl_.binHz()); if (p > h) { h = p; hz = f; } }
        // hum only counts when it stands out of the audio around it
        const double around = std::max(1e-18, 0.5 * (wl_.band(std::max(20.0, hz - 40), hz - 15) + wl_.band(hz + 15, hz + 40)) * (6 * wl_.binHz()) / 25.0);
        if (hz > 0 && h > 8 * around && h > 1e-8 * car2) { humDb_ = (float)(10 * std::log10(h / car2)); snprintf(humHz_, sizeof humHz_, "%.0f Hz", hz); }
        else humDb_ = -99;
        // 99 % bandwidth
        double cum = 0, tail = 0; bw_ = 0;
        if (tot > 0) {
            const int kmax = std::min(wl_.bins() - 1, (int)(kCut / wl_.binHz()));
            for (int k = (int)(30 / wl_.binHz()); k <= kmax; k++) { cum += wl_.band(k * wl_.binHz(), k * wl_.binHz()); (void)tail; if (cum >= 0.99 * tot) { bw_ = (float)(k * wl_.binHz()); break; } }
        }
        // carrier found / lost: the level falls more than 12 dB under the best of the last minute (or comes back within 6 dB of it), held for two seconds
        if (first_) { first_ = false; refDb_ = lvl; carrierOn_ = on; if (on) row("Carrier", "present", fmt("%+.1f Hz, level %.1f dB", offHz_, lvl)); }
        else {
            const bool want = on && (carrierOn_ ? !(lvl < refDb_ - 12) : (lvl > refDb_ - 6));
            if (want != carrierOn_) { if (++cCnt_ >= 2) { carrierOn_ = want; cCnt_ = 0; row("Carrier", want ? "returned" : "lost", fmt("level %.1f dB", lvl)); } } else cCnt_ = 0;
            if (carrierOn_) refDb_ = std::max(lvl, refDb_ - 0.05f);                     // follows slowly downwards (about 1 dB in 20 s)
        }
        if (overFrac_ > 0.005f) { nOver_++; row("Over-modulation", fmt("%.1f %%", overFrac_ * 100), fmt("negative peaks reach zero carrier for %.1f %% of the second, positive peak +%.0f %%", overFrac_ * 100, depthPos_ * 100)); }
        if (carrierOn_) out->good(0, offHz_);
        have_ = true; sec_++;
        env_.clear();
    }
    void row(const char* kind, const std::string& id, const std::string& text) { out->row(kind, id, text, carrierDb_); }
    Decimator dec_; CarrierGate gate_; RealWelch wl_; double rate_ = 1, t_ = 0; int n_ = 1, cCnt_ = 0, nOver_ = 0, sec_ = 0; std::vector<float> env_; cf32 acc_{0, 0}, prev_{1, 0};
    bool first_ = true, have_ = false, carrierOn_ = false;
    float carrierDb_ = -99, depthRms_ = 0, depthPos_ = 0, depthNeg_ = 0, humDb_ = -99, offHz_ = 0, bw_ = 0, overFrac_ = 0, asym_ = 0, refDb_ = -200; char humHz_[16] = "";
};
}

std::unique_ptr<Decoder> makeAmMon() { return std::make_unique<AmMonitor>(); }

} // namespace dect2::x40
