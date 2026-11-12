// UVB-76 ("The Buzzer", 4625 kHz AM) monitor. The station sends a harsh buzz tone in a regular train (about 25 buzzes a minute) day and night; from time to time
// the train stops and a voice message follows. This mode times what is on the air: the audio energy is gated into buzz events, the buzz rate, length and
// regularity are measured, a break in the train and any long transmission are logged. It does not decode or interpret the voice and makes no claim on
// what a message means. The AM audio is the carrier envelope with the carrier (mean) removed, so a tuning error of a few hundred Hz does not matter.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 12000, kCut = 4500, kBlock = 0.01;   // audio rate, passband, energy block
class Uvb76Dec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); gate_.configure(rate_); lp_ = (float)(1 - std::exp(-2 * kPi * 1500.0 / rate_)); blockN_ = std::max(8, (int)(rate_ * kBlock));
        reset();
        return true;
    }
    void reset() override {
        dec_.reset(); gate_.reset(); lp1_ = lp2_ = 0; dc_ = 0; acc_ = 0; cnt_ = 0; t_ = 0; floor_ = -1; on_ = false; onT_ = offT_ = 0; lastOn_ = -1;
        periods_.clear(); lens_.clear(); smoothDb_ = -200; locked_ = false; started_ = false;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            const float e = std::abs(a);
            gate_.push(a);
            dc_ += (e - dc_) * (float)(1.0 / (rate_ * 1.0));
            const float raw = e - dc_;
            lp1_ += lp_ * (raw - lp1_); lp2_ += lp_ * (lp1_ - lp2_);   // the buzz is low-pitched: keep about 1.5 kHz of audio and the noise stays out
            const float m = lp2_;
            acc_ += m * m;
            t_ += 1.0 / rate_;
            if (++cnt_ >= blockN_) { block(acc_ / (float)cnt_, dc_); acc_ = 0; cnt_ = 0; }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!total_) { t.headline = "no buzz heard"; return; }
        t.headline = locked_ ? fmt("buzz train %.1f per minute, buzz %.2f s", rate(), medianLen()) : fmt("%llu buzzes heard", (unsigned long long)total_);
        t.facts.push_back({"buzzes heard", fmt("%llu", (unsigned long long)total_)});
        if (locked_) {
            t.facts.push_back({"buzz rate", fmt("%.1f per minute", rate())});
            t.facts.push_back({"buzz length", fmt("%.2f s", medianLen())});
            t.facts.push_back({"period", fmt("%.2f s", medianPeriod())});
            t.facts.push_back({"period spread", fmt("%.0f ms", spreadMs())});
        }
        t.facts.push_back({"breaks logged", fmt("%d", breaks_)});
        t.facts.push_back({"long transmissions", fmt("%d", longTx_)});
    }
private:
    static double median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
    double medianPeriod() const { return median({periods_.begin(), periods_.end()}); }
    double medianLen() const { return median({lens_.begin(), lens_.end()}); }
    double rate() const { const double p = medianPeriod(); return p > 0 ? 60.0 / p : 0; }
    double spreadMs() const {
        const double m = medianPeriod(); std::vector<double> d; for (double p : periods_) d.push_back(std::fabs(p - m));
        return median(d) * 1e3;
    }
    void block(float p, float carrier) {
        if (!gate_.present()) { on_ = false; return; }   // no carrier on the channel: noise makes no buzz
        // modulation power relative to the carrier, in dB; the floor follows the quiet level (slowly up, fast down)
        const float db = toDb(p / std::max(carrier * carrier, 1e-12f));
        if (smoothDb_ < -150) smoothDb_ = db; else smoothDb_ += 0.5f * (db - smoothDb_);
        if (floor_ < -150) floor_ = smoothDb_;
        else if (smoothDb_ < floor_) floor_ += 0.5f * (smoothDb_ - floor_);
        else if (!on_) floor_ += 0.0005f * (smoothDb_ - floor_);
        const float thr = std::max(floor_, -45.f) + (on_ ? 6.f : 9.f);   // no decision on modulation weaker than -45 dB of the carrier
        const double now = t_;
        if (!on_ && smoothDb_ > thr) { on_ = true; onT_ = now; }
        else if (on_ && smoothDb_ < thr) {
            on_ = false; offT_ = now;
            event(onT_, now);
        }
        if (on_) out->energy();
        // a break in the train is noticed while it is still going on
        if (locked_ && !on_ && lastOn_ >= 0 && now - lastOn_ > 3.0 * medianPeriod() && !breakLogged_) {
            breakLogged_ = true; breaks_++;
            out->row("Train break", fmt("%.0f s", now - lastOn_), "the buzz train stopped", smoothDb_);
        }
    }
    void event(double on, double off) {
        const double len = off - on;
        if (len < 0.15) return;                                   // a click, not a buzz
        if (len > 4.0 && (!locked_ || len > 3.0 * medianLen())) {   // much longer than a buzz: something else is being sent
            longTx_++; out->good(floor_ >= -150 ? smoothDb_ - floor_ : 0, 0);
            out->row("Long transmission", fmt("%.1f s", len), "not a buzz (a voice message is possible; not decoded)", smoothDb_);
            lastOn_ = off; breakLogged_ = false; periods_.clear(); locked_ = false; return;
        }
        total_++;
        if (lastOn_ >= 0 && started_) {
            const double per = on - lastOnStart_;
            if (per > 0.5 && per < 15) { periods_.push_back(per); if (periods_.size() > 12) periods_.pop_front(); }
        }
        lens_.push_back(len); if (lens_.size() > 12) lens_.pop_front();
        started_ = true; lastOnStart_ = on; lastOn_ = off; breakLogged_ = false;
        out->good(smoothDb_ - floor_, 0);
        if (!locked_ && periods_.size() >= 4) {
            const double m = medianPeriod();
            if (spreadMs() < 0.15 * m * 1e3) { locked_ = true; out->row("Buzz train", fmt("%.1f /min", rate()), fmt("period %.2f s, buzz %.2f s", m, medianLen()), smoothDb_); }
        }
    }
    Decimator dec_; CarrierGate gate_;
    double rate_ = 1, t_ = 0, onT_ = 0, offT_ = 0, lastOn_ = -1, lastOnStart_ = 0;
    int blockN_ = 1, cnt_ = 0, breaks_ = 0, longTx_ = 0;
    float lp1_ = 0, lp2_ = 0, lp_ = 0.5f;
    float dc_ = 0, acc_ = 0, smoothDb_ = -200, floor_ = -1;
    bool on_ = false, locked_ = false, started_ = false, breakLogged_ = false;
    uint64_t total_ = 0;
    std::deque<double> periods_, lens_;
};
}

std::unique_ptr<Decoder> makeUvb76() { return std::make_unique<Uvb76Dec>(); }

} // namespace dect2::x40
