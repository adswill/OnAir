// Beacon propagation monitor (50 / 144 / 432 MHz and 28 MHz beacons, CW or a steady carrier): one narrow beacon is followed and what the path does to it is
// classified from three measurements per spectrum: the level over the noise, the Doppler spread of the line (aurora scatter smears a CW line over tens to hundreds
// of Hz, 'a buzz') and the slow swing of the level (sporadic-E and tropospheric fading). Classes: steady, fading, aurora-like, none. A change of class is logged.
// The class is a reading of the signal, not a forecast, and a transmitter that is not steady (keyed beacons, drifting oscillators) will read as something else.
#include "x40_int.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr int kN = 2048;
constexpr double kWorkTarget = 2400, kCut = 1000;
constexpr int kEval = 5;                 // spectra per class decision (about 4 s)

void fft(std::vector<cf32>& a) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++) { int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / len;
        const cf32 wl((float)std::cos(ang), (float)std::sin(ang));
        for (int i = 0; i < n; i += len) {
            cf32 w(1, 0);
            for (int k = 0; k < len / 2; k++) { const cf32 u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; }
        }
    }
}

class BeaconDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kN;
        win_.resize(kN);
        for (int i = 0; i < kN; i++) { const double x = 2 * kPi * i / (kN - 1); win_[i] = (float)(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x)); }
        reset();
        return true;
    }
    void reset() override {
        dec_.reset(); buf_.assign(kN, cf32(0, 0)); fillN_ = 0; hist_.clear(); levels_.clear(); spreads_.clear(); cls_ = pending_ = 0; pendN_ = 0; frames_ = 0; peakBin_ = -1; peakHz_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { buf_[fillN_++] = a; if (fillN_ == kN) { frame(); fillN_ = 0; } });
    }
    void fill(X40Telemetry& t) const override {
        t.cfoHz = peakHz_;
        t.headline = !frames_ ? "waiting for data" : cls_ == 0 ? "no beacon" : fmt("%s, %.0f dB over the noise", className(cls_), lastDb_);
        if (frames_) {
            t.facts.push_back({"path", className(cls_)});
            t.facts.push_back({"beacon level", fmt("%.1f dB over the noise in a bin", lastDb_)});
            t.facts.push_back({"Doppler spread", fmt("%.1f Hz", lastSpread_)});
            t.facts.push_back({"level swing", fmt("%.1f dB", swing_)});
            t.facts.push_back({"line offset", fmt("%+.1f Hz from the tuned frequency", peakHz_)});
            t.facts.push_back({"class changes", fmt("%d", changes_)});
        }
    }
private:
    static const char* className(int c) { return c == 1 ? "steady" : c == 2 ? "fading" : c == 3 ? "aurora-like" : "none"; }
    void frame() {
        std::vector<cf32> a(kN);
        for (int i = 0; i < kN; i++) a[i] = buf_[i] * win_[i];
        fft(a);
        std::vector<float> p(kN);
        for (int i = 0; i < kN; i++) p[i] = std::norm(a[i]);
        frames_++;
        std::vector<float> s(p); std::nth_element(s.begin(), s.begin() + kN / 2, s.end());
        const float noise = std::max(s[kN / 2], 1e-20f) / 0.693f;
        // the line: the strongest bin, searched near the last one first (a line that drifts keeps its track)
        int pb = 0;
        if (peakBin_ >= 0) {
            float best = 0; for (int d = -40; d <= 40; d++) { const int i = (peakBin_ + d + kN) % kN; if (p[i] > best) { best = p[i]; pb = i; } }
        }
        int gb = 0; for (int i = 1; i < kN; i++) if (p[i] > p[gb]) gb = i;
        if (peakBin_ < 0 || p[gb] > 4.f * p[pb]) pb = gb;   // a much stronger line elsewhere: take it
        peakBin_ = pb;
        const float db = toDb(p[pb] / noise);
        lastDb_ = db;
        const int sb = pb >= kN / 2 ? pb - kN : pb; peakHz_ = sb * binHz_;
        if (db < 10.f) { push(db, 0.f, false); return; }
        // second moment of the excess power within +-300 Hz of the line
        const int w = (int)(300.0 / binHz_);
        // the line's own region only, on the spectrum smoothed over 9 bins (a smeared line is weak in every single bin): walk out from the peak until
        // twelve bins in a row are near the noise, so isolated noise peaks further out are not taken for the line
        std::vector<float> q(kN);
        for (int i = 0; i < kN; i++) { float v = 0; for (int d = -4; d <= 4; d++) v += p[(i + d + kN) % kN]; q[i] = v / 9.f; }
        double sum = 0, m1 = 0, m2 = 0;
        for (int dir = -1; dir <= 1; dir += 2) {
            int gap = 0;
            for (int d = (dir < 0 ? 0 : 1); std::abs(d) <= w && gap < 12; d += dir) {
                const int i = (pb + d + kN) % kN;
                if (q[i] > 2.2f * noise) { gap = 0; const float e = q[i] - noise; sum += e; m1 += e * d; m2 += e * (double)d * d; } else gap++;
            }
        }
        const double mean = sum > 0 ? m1 / sum : 0, var = sum > 0 ? std::max(0.0, m2 / sum - mean * mean) : 0;
        lastSpread_ = (float)(2.355 * std::sqrt(var) * binHz_);   // a steady line reads about 8 Hz after the smoothing
        out->energy();
        push(db, lastSpread_, true);
    }
    void push(float db, float spread, bool present) {
        levels_.push_back(present ? db : -1.f); spreads_.push_back(spread);
        if ((int)levels_.size() < kEval) return;
        std::vector<float> lv(levels_.begin(), levels_.end()), sp(spreads_.begin(), spreads_.end());
        levels_.clear(); spreads_.clear();
        int pres = 0; for (float v : lv) if (v >= 0) pres++;
        int c = 0; float swing = 0;
        if (pres >= 3) {
            std::vector<float> l2; for (float v : lv) if (v >= 0) l2.push_back(v);
            const float mx = *std::max_element(l2.begin(), l2.end()), mn = *std::min_element(l2.begin(), l2.end());
            swing = mx - mn;
            std::sort(sp.begin(), sp.end()); const float ms = sp[sp.size() / 2];
            hist_.push_back(mx); if (hist_.size() > 8) hist_.pop_front();
            swing_ = swing;
            c = ms > 40.f ? 3 : swing > 6.f ? 2 : 1;
        } else swing_ = 0;
        if (c == cls_) { pending_ = c; pendN_ = 0; return; }
        if (c == pending_) pendN_++; else { pending_ = c; pendN_ = 1; }
        if (pendN_ >= 2) {
            const int old = cls_; cls_ = c; pendN_ = 0; changes_++;
            if (c) out->good(lastDb_, peakHz_); else out->bad();
            out->row("Path change", fmt("%s", className(c)), fmt("%s to %s: %.0f dB over the noise, spread %.0f Hz, swing %.1f dB", className(old), className(c), lastDb_, lastSpread_, swing_), lastDb_);
        }
    }
    Decimator dec_;
    std::vector<cf32> buf_; std::vector<float> win_;
    std::deque<float> hist_; std::vector<float> levels_, spreads_;
    int fillN_ = 0, cls_ = 0, pending_ = 0, pendN_ = 0, changes_ = 0, peakBin_ = -1; uint64_t frames_ = 0;
    double rate_ = 1, binHz_ = 1; float lastDb_ = 0, lastSpread_ = 0, swing_ = 0, peakHz_ = 0;
};
}

std::unique_ptr<Decoder> makeBeacon() { return std::make_unique<BeaconDec>(); }

} // namespace dect2::x40
