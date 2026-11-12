// CW keying quality meter: what a keyed carrier's timing and shape look like, for the person who sent it or the person who wants to know why it sounds wrong. The carrier (within +-1.5 kHz of the tuned
// frequency) is found and followed; its envelope (2 kHz rate, 800 Hz bandwidth) is cut into marks and spaces at the 50 % points with hysteresis (on above 50 % of the recent peak, off under 35 %).
// From the last 60 elements: the dit and dah lengths by two-means clustering of the marks (and the speed: PARIS standard, words per minute = 1200 / T in ms with T = (dit + element gap) / 2), the dah to dit ratio
// (ideal 3), the spaces sorted into element gaps (under 2 dits), character gaps (2 - 5) and word gaps (over 5) as multiples of the dit (ideal 1, 3, 7), the weight (dit length over element gap length,
// ideal 1), the scatter of the dit lengths (coefficient of variation: a keyer is under 2 %, a hand over 10 %), the rise and fall times (10 - 90 % of the plateau, from 0.5 ms resolution up), and the chirp
// (the carrier frequency 5 - 25 ms after key-down against the second half of the element, for elements of 50 ms or more). The envelope filter limits what it can see: rise times under about 1.5 ms read 1.5 ms.
// It does not copy the text (the CW mode does). Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 12000, kCut = 2100, kSearchHz = 1500, kEnvCut = 800, kKeep = 60;

struct Mark { double t0 = 0, dur = 0, rise = -1, fall = -1, chirp = -999, level = 0; };

class CwDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec1_.configure(fs, d, kCut)) return false;
        r1_ = dec1_.outRate(); n1_ = 1; while (n1_ * 2 <= r1_ * 0.7) n1_ *= 2;
        win_.resize(n1_); for (int i = 0; i < n1_; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n1_));
        const int d2 = std::max(1, (int)std::floor(r1_ / 2000.0));
        if (!dec2_.configure(r1_, d2, kEnvCut)) return false;
        r2_ = dec2_.outRate();
        reset(); return true;
    }
    void reset() override {
        dec1_.reset(); dec2_.reset(); buf_.clear(); fc_ = 0; haveFc_ = false; ph_ = 0; t_ = 0; skip_ = (size_t)(0.2 * r1_); nEnv_ = 0; marks_.clear(); gaps_.clear(); down_ = false; pk_ = 0; nf_ = 0;
        recent_.clear(); pre_.clear(); ea_.clear(); ef_.clear(); pend_ = false; prevZ_ = cf32(1, 0); have_ = false; nMarks_ = 0; lastRow_ = -1e9; lastWpm_ = 0; res_ = Res(); tEnd_ = 0; tStart_ = 0; sinceEval_ = 0;
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
        if (!res_.ok) { t.headline = haveFc_ ? "listening for keyed elements" : "no carrier in +-1.5 kHz of the tuned frequency"; return; }
        t.headline = fmt("%.1f wpm, dah %.2f x dit, character gap %.1f, word gap %.1f dits, weight %.2f, rise %s, fall %s", res_.wpm, res_.dahRatio, res_.charGap, res_.wordGap, res_.weight, ms(res_.rise).c_str(), ms(res_.fall).c_str());
        t.facts.push_back({"speed", fmt("%.1f wpm (PARIS; T = (dit + element gap) / 2 = %.1f ms), dit %.1f ms", res_.wpm, 1200.0 / res_.wpm, res_.dit)});
        t.facts.push_back({"dit and dah", res_.dahs ? fmt("dit %.1f ms, dah %.1f ms, dah / dit %.2f (ideal 3.00)", res_.dit, res_.dah, res_.dahRatio) : fmt("dit %.1f ms (no dahs seen in the last %d elements)", res_.dit, (int)marks_.size())});
        t.facts.push_back({"gaps (multiples of the dit)", fmt("element %.2f (ideal 1), character %s (ideal 3), word %s (ideal 7)", res_.elGap, res_.nChar ? fmt("%.2f", res_.charGap).c_str() : "-", res_.nWord ? fmt("%.2f", res_.wordGap).c_str() : "-")});
        t.facts.push_back({"weight", fmt("%.2f (dit length over element gap length, ideal 1.00)", res_.weight)});
        t.facts.push_back({"dit scatter", fmt("%.1f %% (coefficient of variation of the dit length)", res_.cv)});
        t.facts.push_back({"rise time (10 - 90 %)", ms(res_.rise)});
        t.facts.push_back({"fall time (90 - 10 %)", ms(res_.fall)});
        t.facts.push_back({"chirp", res_.chirpN ? fmt("%+.1f Hz (carrier 5 - 25 ms after key-down against the second half of the element)", res_.chirp) : std::string("-")});
        t.facts.push_back({"elements", fmt("%d marks and %zu spaces in the window", (int)marks_.size(), gaps_.size())});
    }
private:
    struct Res { bool ok = false, dahs = false; double wpm = 0, dit = 0, dah = 0, dahRatio = 0, elGap = 0, charGap = 0, wordGap = 0, weight = 0, cv = 0, rise = -1, fall = -1, chirp = 0; int nChar = 0, nWord = 0, chirpN = 0; };
    static std::string ms(double v) { return v < 0 ? std::string("-") : fmt("%.1f ms", v); }
    void track() {
        have_ = true;
        std::vector<cf32> a((size_t)n1_); for (int i = 0; i < n1_; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        const double bh = r1_ / n1_; const int hw = (int)(kSearchHz / bh);
        std::vector<float> p((size_t)n1_); for (int k = 0; k < n1_; k++) p[k] = std::norm(a[k]);
        int kp = 0; float pk = 0; for (int d = -hw; d <= hw; d++) { const int k = (d + n1_) % n1_; if (p[k] > pk) { pk = p[k]; kp = k; } }
        std::vector<float> nb; for (int d = -(int)(kCut / bh); d <= (int)(kCut / bh); d += 2) { const int k = (d + n1_) % n1_; if (std::abs(((k - kp + n1_ / 2) % n1_ + n1_) % n1_ - n1_ / 2) > 30) nb.push_back(p[k]); }
        if (nb.empty()) return;
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end()); const double noise = std::max((double)nb[nb.size() / 2], 1e-30) / 0.693;
        if (pk < 60.0 * noise) return;                                              // a keyed carrier in a window may be only partly on: the peak must still stand well out
        const double la = std::log(p[(kp + n1_ - 1) % n1_] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[(kp + 1) % n1_] + 1e-30), den = la - 2 * lb + lc;
        double f = (kp + (den < -1e-12 ? 0.5 * (la - lc) / den : 0.0)) * bh; if (f > r1_ / 2) f -= r1_;
        fc_ = haveFc_ ? 0.7 * fc_ + 0.3 * f : f; haveFc_ = true;
    }
    void envelope(cf32 z) {
        if (nEnv_ < (size_t)(0.5 * r2_)) { nEnv_++; prevZ_ = z; return; }
        const double now = t_;
        const float amp = std::abs(z);
        // frequency from the phase step (Hz), only meaningful while the carrier is up
        const cf32 d = z * std::conj(prevZ_); prevZ_ = z; const float finst = (float)(std::atan2(d.imag(), d.real()) * r2_ / (2 * kPi));
        recent_.push_back(det_); if (recent_.size() > (size_t)(4 * r2_)) recent_.pop_front();
        det_ += (amp - det_) * (float)(1.0 - std::exp(-1.0 / (0.004 * r2_)));      // the edges are judged on a 4 ms smoothing of the envelope (noise), measured on the raw one
        pk_ = std::max((double)det_, pk_ * (1.0 - 0.3 / r2_));                      // the peak decays with a time constant of about 3 s
        if (++sinceNf_ >= (int)(0.5 * r2_)) { sinceNf_ = 0; std::vector<float> v(recent_.begin(), recent_.end()); if (v.size() > 100) { std::nth_element(v.begin(), v.begin() + v.size() / 5, v.end()); nf_ = v[v.size() / 5]; } }
        pre_.push_back(amp); preF_.push_back(finst); if (pre_.size() > (size_t)(0.04 * r2_)) { pre_.pop_front(); preF_.pop_front(); }
        const bool signal = pk_ > 6.0 * std::max(nf_, 1e-9);
        if (!down_) {
            if (signal && det_ > 0.5 * pk_) { if (pend_) finishFall(); down_ = true; tStart_ = now; ea_.assign(pre_.begin(), pre_.end()); ef_.assign(preF_.begin(), preF_.end()); startIdx_ = (int)ea_.size() - 1; level_ = pk_;
                if (tEnd_ > 0 && tStart_ - tEnd_ > 0 && tStart_ - tEnd_ < 3.0) { gaps_.push_back(tStart_ - tEnd_); if (gaps_.size() > (size_t)kKeep) gaps_.pop_front(); } }
        } else {
            ea_.push_back(amp); ef_.push_back(finst);
            if (det_ < 0.35 * level_ || (now - tStart_) > 4.0) { down_ = false; tEnd_ = now; endMark(now); }
        }
        if (pend_ && (now - tEnd_) >= 0.03) { finishFall(); }
        // after the mark ended, keep collecting 30 ms for the fall time
        if (pend_ && !down_) { ea_.push_back(amp); }
    }
    void endMark(double now) {
        Mark m; m.t0 = tStart_; m.dur = (now - tStart_); pendMark_ = m; pend_ = true; endIdx_ = (int)ea_.size();
        if (pendMark_.dur < 0.004) { pend_ = false; return; }                       // a glitch
    }
    void finishFall() {
        pend_ = false; Mark m = pendMark_;
        const int n = (int)ea_.size();
        // the plateau: the median of the middle third of the mark
        const int a0 = startIdx_ + (endIdx_ - startIdx_) / 3, a1 = startIdx_ + 2 * (endIdx_ - startIdx_) / 3;
        if (a1 - a0 < 3) { pushMark(m); return; }
        std::vector<float> mid(ea_.begin() + a0, ea_.begin() + a1); std::nth_element(mid.begin(), mid.begin() + mid.size() / 2, mid.end()); const float L = mid[mid.size() / 2]; m.level = L;
        // rise: from the start of the stored envelope, first sample over 10 % to first over 90 % (with linear interpolation)
        auto cross = [&](int from, int to, float th, bool up) -> double { const int dir = to >= from ? 1 : -1; for (int i = from; i != to; i += dir) { if (i < 1 || i >= n) break; const float v0 = ea_[(size_t)i - 1], v1 = ea_[(size_t)i]; if (up ? (v0 < th && v1 >= th) : (v0 >= th && v1 < th)) return (double)i - 1 + (th - v0) / (v1 - v0 + 1e-12f); } return -1.0; };
        const double r10 = cross(1, a0, 0.1f * L, true), r90 = cross(1, a0, 0.9f * L, true);
        if (r10 >= 0 && r90 >= r10) m.rise = std::max(1.5e-3, (r90 - r10) / r2_);
        const double f90 = cross(std::min(n - 1, endIdx_ - 1), a1, 0.9f * L, false), f10 = f90 >= 0 ? cross((int)f90 + 1, n, 0.1f * L, false) : -1.0;
        if (f90 >= 0 && f10 >= f90) m.fall = std::max(1.5e-3, (f10 - f90) / r2_);
        if (m.dur >= 0.05) {
            const int sIdx = startIdx_ + (int)(0.005 * r2_), eIdx = startIdx_ + (int)(0.025 * r2_);
            double s1 = 0, s2 = 0; int c1 = 0, c2 = 0;
            for (int i = sIdx; i < eIdx && i < (int)ef_.size(); i++) if (ea_[(size_t)i] > 0.7f * L) { s1 += ef_[(size_t)i]; c1++; }
            for (int i = startIdx_ + (endIdx_ - startIdx_) / 2; i < (int)ef_.size() - (int)(0.003 * r2_) && i < endIdx_ - (int)(0.004 * r2_); i++) if (ea_[(size_t)i] > 0.7f * L) { s2 += ef_[(size_t)i]; c2++; }
            if (c1 >= 4 && c2 >= 8) m.chirp = s1 / c1 - s2 / c2;
        }
        pushMark(m);
    }
    void pushMark(const Mark& m) {
        marks_.push_back(m); if (marks_.size() > (size_t)kKeep) marks_.pop_front(); nMarks_++;
        out->good(0, fc_);
        if (marks_.size() >= 10 && ++sinceEval_ >= 5) { sinceEval_ = 0; evaluate(); }
    }
    static void kmeans2(const std::vector<double>& v, double& c0, double& c1) { std::vector<double> s(v); std::sort(s.begin(), s.end()); c0 = s[s.size() / 6]; c1 = s[s.size() * 5 / 6]; for (int it = 0; it < 30; it++) { double s0 = 0, s1 = 0; int n0 = 0, n1 = 0; for (double x : v) { if (std::fabs(x - c0) <= std::fabs(x - c1)) { s0 += x; n0++; } else { s1 += x; n1++; } } if (n0) c0 = s0 / n0; if (n1) c1 = s1 / n1; } }
    void evaluate() {
        Res r; std::vector<double> d; for (auto& m : marks_) d.push_back(m.dur * 1000);
        double c0, c1; kmeans2(d, c0, c1);
        const bool two = c1 > 1.8 * c0 && c1 < 4.5 * c0;
        std::vector<double> dits, dahs; for (double x : d) { if (!two || std::fabs(x - c0) <= std::fabs(x - c1)) dits.push_back(x); else dahs.push_back(x); }
        if (!two) { dits = d; }
        if (dits.size() < 3) { res_ = Res(); return; }
        auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return s / (double)v.size(); };
        r.dit = mean(dits); r.dahs = two && dahs.size() >= 2; r.dah = r.dahs ? mean(dahs) : 0; r.dahRatio = r.dahs ? r.dah / r.dit : 0; r.wpm = 1200.0 / r.dit;      // refined below when the element gaps are known
        double sd = 0; for (double x : dits) sd += (x - r.dit) * (x - r.dit); r.cv = 100 * std::sqrt(sd / (double)dits.size()) / r.dit;
        std::vector<double> el, ch, wd; for (double g : gaps_) { const double q = g * 1000 / r.dit; if (q < 2.0) el.push_back(q); else if (q < 5.0) ch.push_back(q); else wd.push_back(q); }
        r.elGap = el.empty() ? 0 : mean(el);
        { std::vector<double> eg; for (double g : gaps_) { const double q = g * 1000 / r.dit; if (q < 2.0) eg.push_back(g * 1000); } if (eg.size() >= 3) r.wpm = 1200.0 / (0.5 * (r.dit + mean(eg))); }   // T = (dit + element gap) / 2, so a heavy or light weight does not change the speed r.charGap = ch.empty() ? 0 : mean(ch); r.wordGap = wd.empty() ? 0 : mean(wd); r.nChar = (int)ch.size(); r.nWord = (int)wd.size();
        r.weight = r.elGap > 0 ? 1.0 / r.elGap : 0;
        std::vector<double> ri, fa, chv; for (auto& m : marks_) { if (m.rise > 0) ri.push_back(m.rise * 1000); if (m.fall > 0) fa.push_back(m.fall * 1000); if (m.chirp > -900) chv.push_back(m.chirp); }
        r.rise = ri.empty() ? -1 : mean(ri); r.fall = fa.empty() ? -1 : mean(fa); r.chirp = chv.empty() ? 0 : mean(chv); r.chirpN = (int)chv.size();
        r.ok = true; res_ = r;
        if (std::fabs(r.wpm - lastWpm_) > 0.05 * lastWpm_ || t_ - lastRow_ > 30) {
            lastWpm_ = r.wpm; lastRow_ = t_;
            out->row("Keying", fmt("%.1f wpm", r.wpm), fmt("dit %.1f ms, dah %.2f dits, gaps %.2f / %.2f / %.2f dits, weight %.2f, scatter %.1f %%, rise %s, fall %s, chirp %+.1f Hz", r.dit, r.dahRatio, r.elGap, r.charGap, r.wordGap, r.weight, r.cv, ms(r.rise).c_str(), ms(r.fall).c_str(), r.chirp), 0.f);
        }
    }
    Decimator dec1_, dec2_; double r1_ = 1, r2_ = 1, fc_ = 0, ph_ = 0, t_ = 0, pk_ = 0, nf_ = 0, tEnd_ = 0, tStart_ = 0, level_ = 0, lastRow_ = -1e9, lastWpm_ = 0; float det_ = 0; int n1_ = 1, sinceNf_ = 0, startIdx_ = 0, endIdx_ = 0, sinceEval_ = 0, nMarks_ = 0; size_t skip_ = 0, nEnv_ = 0;
    std::vector<float> win_; std::vector<cf32> buf_; std::deque<float> recent_, pre_, preF_; std::vector<float> ea_, ef_; std::deque<Mark> marks_; std::deque<double> gaps_; Mark pendMark_; cf32 prevZ_{1, 0};
    bool haveFc_ = false, down_ = false, pend_ = false, have_ = false; Res res_;
};
}

std::unique_ptr<Decoder> makeCwKeying() { return std::make_unique<CwDec>(); }

} // namespace dect2::x40
