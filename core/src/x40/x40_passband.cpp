// Passband signal classifier (the 48 kHz around the tuned frequency: a satellite linear transponder, a band segment of 10 / 6 / 2 m, a repeater input): every signal that stands out of the noise is tracked
// in a 94 Hz-resolution spectrogram (10.7 ms frames) and, once it has lasted a second and a half, named by what it does: an unmodulated carrier, keyed CW, a narrow data signal
// (PSK / FSK of a few hundred Hz), an SSB or voice-like signal (1.5 - 3.8 kHz wide, the level moving with the syllables), or a wideband steady signal (FM or high-rate data). The class is a
// guess from width and level behaviour; nothing is demodulated here: tune the signal for that. Receive only.
#include "x40_dsp.h"
#include <array>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 48000, kCut = 22000;
constexpr int kFft = 512;
constexpr float kThrDb = 9.f;
constexpr int kGapFrames = 112, kClassFrames = 140;         // 0.6 s of gap keeps a track; 1.5 s before it is named

struct Track {
    double f = 0; int first = 0, last = 0, id = 0; int miss = 0;
    std::vector<float> lvl;           // dB per frame (the noise floor value while the signal is absent)
    std::vector<uint8_t> on;          // present flags
    std::vector<float> width;         // Hz when present
    std::vector<float> cen;           // centre of the cluster, Hz, when present
    std::array<double, 17> acc{}; std::string pend; int pendN = 0; std::string cls; int logged = 0; double fSum = 0; int fN = 0; float peak = -200;
};

class PassDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); win_.assign(kFft, 0.f);
        for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); frame_ = 0; tracks_.clear(); nextId_ = 1; have_ = false; floorDb_ = -100; seen_ = 0; counts_ = {}; }
    void feed(const cf32* x, size_t n) override { dec_.process(x, n, [&](cf32 a) { buf_.push_back(a); if ((int)buf_.size() == kFft) { doFrame(); buf_.clear(); } }); }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        int act = 0; for (auto& k : tracks_) if (k.miss < 20 && k.logged) act++;
        t.headline = fmt("%d signals now, %d named in all", act, seen_);
        t.facts.push_back({"noise floor", fmt("%.1f dB (relative, per bin)", floorDb_)});
        t.facts.push_back({"signals named", fmt("%d: %d carrier, %d CW, %d narrow data, %d SSB / voice, %d wideband", seen_, counts_[0], counts_[1], counts_[2], counts_[3], counts_[4])});
        t.facts.push_back({"resolution", fmt("%.0f Hz bins, %.1f ms frames, %.0f kHz of passband", rate_ / kFft, 1e3 * kFft / rate_, rate_ / 1e3)});
    }
private:
    struct Cl { int lo, hi; float pk; double pw; double ctr; };
    void doFrame() {
        frame_++; have_ = true;
        std::vector<cf32> a(kFft); for (int i = 0; i < kFft; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        std::vector<float> p(kFft); for (int k = 0; k < kFft; k++) p[k] = std::norm(a[(k + kFft / 2) % kFft]);   // fftshift: bin kFft/2 = tuned frequency
        std::vector<float> s(p); std::sort(s.begin(), s.end());
        const float fl = std::max(s[kFft / 2] / 0.693f, 1e-20f);       // mean noise bin from the median
        floorDb_ = toDb(fl);
        const float thr = fl * std::pow(10.f, kThrDb / 10);
        // clusters: seeds are bins over the threshold; a cluster grows over neighbours above a lower threshold (4.5 dB) and bridges gaps of up to four bins (a voice signal
        // or a data signal is a comb of lumps, not one block)
        const float thrLow = fl * std::pow(10.f, 4.5f / 10);
        std::vector<Cl> cl; int k = 1;
        while (k < kFft - 1) {
            if (p[k] > thr) {
                int lo = k, hi = k; float pk = p[k]; double pw = p[k], wk = (double)p[k] * k;
                int j = k + 1, gap = 0;
                while (j < kFft - 1 && gap <= 4) { if (p[j] > thrLow) { hi = j; pk = std::max(pk, p[j]); pw += p[j]; wk += (double)p[j] * j; gap = 0; } else gap++; j++; }
                for (int q = lo; q < hi; q++) if (!(p[q] > thrLow)) pw += fl;      // the bridged bins hold at least noise
                if (hi - lo + 1 >= 1) {
                    int pi = lo; for (int q = lo; q <= hi; q++) if (p[q] > p[pi]) pi = q;
                    const double w3 = (double)p[pi - 1] + p[pi] + p[pi + 1];
                    // the position of a narrow signal: the centroid of the three bins around the strongest; of a wide one: the whole cluster
                    const double ctr = hi - lo + 1 <= 8 ? ((double)p[pi - 1] * (pi - 1) + (double)p[pi] * pi + (double)p[pi + 1] * (pi + 1)) / std::max(w3, 1e-30) : wk / std::max(pw, 1e-30);
                    cl.push_back({lo, hi, pk, pw, ctr});
                }
                k = hi + 1;
            } else k++;
        }
        // match to tracks
        std::vector<char> used(cl.size(), 0);
        const double bin = rate_ / kFft;
        for (auto& t : tracks_) {
            int best = -1; double bd = 1e9;
            for (size_t c = 0; c < cl.size(); c++) {
                if (used[c]) continue;
                const double cf = (cl[c].ctr - kFft / 2) * bin;
                const double dist = std::fabs(cf - t.f); const double lim = 3 * bin + 0.5 * (cl[c].hi - cl[c].lo + 1) * bin;
                if (dist < lim && dist < bd) { bd = dist; best = (int)c; }
            }
            if (best >= 0) {
                used[best] = 1; const Cl& c = cl[best];
                const double cf = (c.ctr - kFft / 2) * bin;
                t.f += 0.3 * (cf - t.f); t.last = frame_; t.miss = 0;
                t.lvl.push_back(toDb((float)c.pw / fl)); t.on.push_back(1); t.width.push_back((float)((c.hi - c.lo + 1) * bin)); t.cen.push_back((float)cf);
                t.fSum += cf; t.fN++;
                { const int cb = (int)std::lround(t.fSum / t.fN / bin + kFft / 2); for (int d = -8; d <= 8; d++) if (cb + d > 0 && cb + d < kFft - 1) t.acc[d + 8] += p[cb + d]; }      // around the long-term centre, so that the two tones of an FSK signal stay two lines
                t.peak = std::max(t.peak, toDb(c.pk / fl));
            } else {
                t.miss++; t.lvl.push_back(0.f); t.on.push_back(0); t.width.push_back(0.f); t.cen.push_back(0.f);
            }
        }
        for (size_t c = 0; c < cl.size(); c++) if (!used[c]) {
            Track t; t.id = nextId_++; t.first = frame_; t.last = frame_; t.f = (cl[c].ctr - kFft / 2) * bin;
            { const int cb = (int)std::lround(cl[c].ctr); for (int d = -8; d <= 8; d++) if (cb + d > 0 && cb + d < kFft - 1) t.acc[d + 8] += p[cb + d]; }
            t.lvl.push_back(toDb((float)cl[c].pw / fl)); t.on.push_back(1); t.width.push_back((float)((cl[c].hi - cl[c].lo + 1) * bin)); t.cen.push_back((float)t.f); t.fSum = t.f; t.fN = 1; t.peak = toDb(cl[c].pk / fl);
            tracks_.push_back(std::move(t));
        }
        // evaluate and retire
        for (auto& t : tracks_) {
            const int age = frame_ - t.first + 1;
            if (age >= kClassFrames && (age % 20 == 0)) evaluate(t);
        }
        for (size_t i = 0; i < tracks_.size();) {
            if (tracks_[i].miss > kGapFrames) tracks_.erase(tracks_.begin() + (long)i); else i++;
        }
    }
    void evaluate(Track& t) {
        // the signal's own span: from the first frame that starts a steady appearance (6 of 8 frames present: a track may have been started by a noise lump) to the last present frame
        size_t n = t.on.size(); while (n > 0 && !t.on[n - 1]) n--;
        size_t b0 = 0; for (; b0 + 8 <= n; b0++) { int c = 0; for (size_t i = 0; i < 8; i++) c += t.on[b0 + i]; if (c >= 6) break; }
        if (b0 + 8 > n || n - b0 < (size_t)kClassFrames * 8 / 10) return;
        int present = 0; std::vector<float> w, l; int runs = 0, runOn = 0;
        for (size_t i = b0; i < n; i++) { if (t.on[i]) { present++; w.push_back(t.width[i]); l.push_back(t.lvl[i]); if (!runOn) { runs++; runOn = 1; } } else runOn = 0; }
        if (present < 60) return;
        std::sort(w.begin(), w.end()); const float wMed = w[w.size() / 2];
        double m = 0; for (float v : l) m += v; m /= (double)l.size(); double var = 0; for (float v : l) var += (v - m) * (v - m); const double sd = std::sqrt(var / (double)l.size());
        const double span = (double)(n - b0); const double duty = (double)present / span;
        // level swings: frame-to-frame changes over the present frames
        double sw = 0; int sn = 0; for (size_t i = b0 + 1; i < n; i++) if (t.on[i] && t.on[i - 1]) { sw += std::fabs(t.lvl[i] - t.lvl[i - 1]); sn++; } sw = sn ? sw / sn : 0;
        double cj = 0; int cn = 0; for (size_t i = b0 + 1; i < n; i++) if (t.on[i] && t.on[i - 1]) { cj += std::fabs(t.cen[i] - t.cen[i - 1]); cn++; } cj = cn ? cj / cn : 0;   // the tones of an FSK signal move the centre
        // the long-term spectrum of the signal: the bins within 6 dB of its peak; an FSK signal shows two lines, a carrier one
        int w6 = 0; { double mx = 0; for (double v : t.acc) mx = std::max(mx, v); for (double v : t.acc) if (v > 0.25 * mx) w6++; }
        std::string cls; int ci;
        if (wMed <= 700 && runs >= 5 && duty < 0.9 && duty > 0.1) { cls = "CW (keyed)"; ci = 1; }
        else if (wMed <= 420 && w6 <= 4 && duty >= 0.9 && sd < 2.5) { cls = "unmodulated carrier"; ci = 0; }   // the long-term spectrum of an FSK signal has two lines
        else if (wMed <= 450) {
            cls = "narrow data (PSK / FSK)"; ci = 2;
        } else if (wMed <= 1500) { cls = duty >= 0.85 && sd < 3.0 ? "narrow data (PSK / FSK)" : "narrow signal, keyed or fading"; ci = 2; }
        else if (wMed <= 3800) { if (sd > 2.5 || sw > 1.8 || duty < 0.9) { cls = "SSB / voice-like"; ci = 3; } else { cls = "steady 2 - 4 kHz signal (data or a carrier with tones)"; ci = 2; } }
        else { cls = "wideband steady signal (FM or high-rate data)"; ci = 4; }
        // a class must come out the same at two evaluations (every 0.2 s) before it is logged, the first time or as a change
        if (cls == t.cls) { t.pend.clear(); t.pendN = 0; return; }
        if (cls != t.pend) { t.pend = cls; t.pendN = 1; return; }
        if (++t.pendN < 2) return;
        t.pend.clear(); t.pendN = 0;
        const bool first = t.cls.empty();
        if (!first) { /* a class change is logged again */ }
        t.cls = cls; if (first) { seen_++; counts_[ci]++; } t.logged++;
        const double f = t.fN ? t.fSum / t.fN : t.f;
        out->good((float)m, f);
        out->row("Signal", fmt("%+.2f kHz", f / 1000),
                 fmt("%s: %.0f Hz wide, %.1f dB over the noise (peak bin %.1f dB), on %.0f %% of %.1f s, %d keyings, level swing %.1f dB", cls.c_str(), wMed, m, t.peak, duty * 100, span * kFft / rate_, std::max(0, runs - 1), sd), (float)m);
    }
    Decimator dec_; std::vector<cf32> buf_; std::vector<float> win_; double rate_ = 1; int frame_ = 0, nextId_ = 1, seen_ = 0; bool have_ = false; float floorDb_ = -100;
    std::vector<Track> tracks_; std::array<int, 5> counts_{};
};
}

std::unique_ptr<Decoder> makePassband() { return std::make_unique<PassDec>(); }

} // namespace dect2::x40
