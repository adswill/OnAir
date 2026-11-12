// Shared signal processing of the HF modes taken over from Rivet (docs/RIVET-MODES-4.0.md): XPA, XPA2, CROWD36, FSK200/500, FSK200/1000 and GW FSK.
// The protocol tables and the message logic of x40_rivet_*.cpp are ported from Rivet (Java):
//   Rivet Copyright (C) 2011 Ian Wraith, GNU General Public License v3 or later, https://github.com/priyom/Rivet
//   This program comes with ABSOLUTELY NO WARRANTY.
// The signal processing in this file is new code: Rivet looks for the tones with one FFT per symbol at 8 or 11.025 kHz; here the channel is
// searched for the whole tone set on a long averaged spectrum (any offset, any sample rate), the last seconds are replayed once the tones are
// found, every tone has its own sliding matched filter, and the symbol clock follows the peak of the filter outputs.
// Every decoder of these modes is fed with the complex channel; upper sideband reception puts an audio tone f at +f Hz.
#pragma once
#include "x40_dsp.h"
#include <complex>
#include <deque>

namespace dect2::x40::rivet {

constexpr double kWork = 8000, kCut = 3400;

// The channel brought down to about 8 ksps (+-3.4 kHz)
class Front {
public:
    bool configure(double fs) {
        const int d = std::max(1, (int)std::floor(fs / kWork));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate();
        return true;
    }
    void reset() { dec_.reset(); }
    double rate() const { return rate_; }
    template <class F> void process(const cf32* x, size_t n, F&& emit) { dec_.process(x, n, emit); }
private:
    Decimator dec_; double rate_ = kWork;
};

// Averaged power spectrum of about 1 Hz resolution (Hann, half overlap, exponential forgetting). p is in power units: a tone of amplitude A
// sums to A^2 over its bins, white noise of power s2 per sample gives s2 / n per bin.
class Psd {
public:
    void configure(double rate, double alpha = 1.0 / 16, double seconds = 1.0) {
        rate_ = rate; a_ = alpha; n_ = 256;
        while (n_ < rate * seconds && n_ < 65536) n_ *= 2;
        w_.resize((size_t)n_); double s2 = 0;
        for (int i = 0; i < n_; i++) { w_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / n_)); s2 += (double)w_[(size_t)i] * w_[(size_t)i]; }
        norm_ = 1.0 / ((double)n_ * s2);
        reset();
    }
    void reset() { p_.assign((size_t)n_, 0.0); cum_.assign((size_t)n_ + 1, 0.0); buf_.clear(); segs_ = 0; }
    // true when a segment was added
    bool push(cf32 x) {
        buf_.push_back(x);
        if ((int)buf_.size() < n_) return false;
        segment();
        buf_.erase(buf_.begin(), buf_.begin() + n_ / 2);
        return true;
    }
    int segments() const { return segs_; }
    double binHz() const { return rate_ / n_; }
    double rate() const { return rate_; }
    int bins() const { return n_; }
    // independent averages behind each bin (forgetting and the overlap of the segments)
    double effAverages() const { return 0.7 * std::min((double)segs_, (2.0 - a_) / a_); }
    // power between f0 and f1 Hz (bins whose centre lies inside), and the number of bins
    double band(double f0, double f1, int* nb = nullptr) const {
        const double bh = binHz();
        int i0 = (int)std::ceil(f0 / bh) + n_ / 2, i1 = (int)std::floor(f1 / bh) + n_ / 2;
        i0 = std::max(i0, 0); i1 = std::min(i1, n_ - 1);
        if (nb) *nb = std::max(0, i1 - i0 + 1);
        return i1 < i0 ? 0.0 : cum_[(size_t)i1 + 1] - cum_[(size_t)i0];
    }
    // the mean noise power of one bin: the median of the bins inside +-edge Hz, corrected for the skew of an average of a few segments
    double noiseBin(double edge) const {
        std::vector<double> v; v.reserve((size_t)n_);
        const double bh = binHz();
        for (int i = 0; i < n_; i++) { const double f = (i - n_ / 2) * bh; if (std::fabs(f) <= edge) v.push_back(cum_[(size_t)i + 1] - cum_[(size_t)i]); }
        if (v.empty()) return 1e-30;
        std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
        const double k = std::max(1.0, effAverages() / 0.7 * 1.0), c = std::pow(1.0 - 1.0 / (9.0 * k), 3.0);   // median of a gamma(k) / its mean
        return std::max(v[v.size() / 2] / std::max(c, 0.5), 1e-30);
    }
private:
    void segment() {
        std::vector<cf32> a((size_t)n_);
        for (int i = 0; i < n_; i++) a[(size_t)i] = buf_[(size_t)i] * w_[(size_t)i];
        fftx(a);
        segs_++;
        const double al = std::max(a_, 1.0 / segs_);
        for (int k = 0; k < n_; k++) { const double q = std::norm(a[(size_t)k]) * norm_; p_[(size_t)k] = (1 - al) * p_[(size_t)k] + al * q; }
        // frequency order: index i holds (i - n / 2) bins
        cum_[0] = 0;
        for (int i = 0; i < n_; i++) { const int k = (i - n_ / 2 + n_) % n_; cum_[(size_t)i + 1] = cum_[(size_t)i] + p_[(size_t)k]; }
    }
    double rate_ = kWork, a_ = 1.0 / 16, norm_ = 1; int n_ = 1024, segs_ = 0;
    std::vector<float> w_; std::vector<double> p_, cum_; std::vector<cf32> buf_;
};

// Where is the tone set? Every offset df (one bin steps) between dfLo and dfHi is tried: the score is the power around the tones minus the
// power around the guard tones (the comb continued by guardStep beyond both ends), so a comb shifted by one tone loses. ratio: the power
// around the tones over the noise expected there; z: how many standard deviations of that noise sum it stands out.
struct Fit { bool found = false; double df = 0, ratio = 0, z = 0, snr3k = 0; };
inline Fit fitComb(const Psd& p, const std::vector<double>& tones, double halfW, double guardStep, int guards, double minZ, double dfLo, double dfHi, int minSegs = 4) {
    Fit best;
    if (p.segments() < minSegs || tones.empty()) return best;
    const double bh = p.binHz(), edge = 0.44 * p.rate();
    const double nb = p.noiseBin(edge);
    double fmin = tones[0], fmax = tones[0];
    for (double f : tones) { fmin = std::min(fmin, f); fmax = std::max(fmax, f); }
    double bestScore = -1e300, bestIn = 0; int binsIn = 0;
    for (double df = dfLo; df <= dfHi; df += bh) {
        if (fmin + df - halfW < -edge || fmax + df + halfW > edge) continue;
        double in = 0; int nIn = 0;
        for (double f : tones) { int c = 0; in += p.band(f + df - halfW, f + df + halfW, &c); nIn += c; }
        double gout = 0;   // guard tones outside the band count as empty
        for (int g = 1; g <= guards; g++) {
            const double a = fmin + df - g * guardStep, b = fmax + df + g * guardStep;
            if (a - halfW >= -edge) gout += p.band(a - halfW, a + halfW);
            if (b + halfW <= edge) gout += p.band(b - halfW, b + halfW);
        }
        const double score = in - gout;
        if (score > bestScore) { bestScore = score; best.df = df; bestIn = in; binsIn = nIn; }
    }
    if (binsIn <= 0) return best;
    const double expected = nb * binsIn;
    best.ratio = bestIn / expected;
    const double sigma = 1.0 / std::sqrt(std::max(1.0, p.effAverages() * binsIn / 1.5));
    best.z = (best.ratio - 1.0) / sigma;
    best.snr3k = std::max(1e-6, (bestIn - expected) / (nb * 3000.0 / bh));
    best.found = best.z >= minZ;
    return best;
}

// One sliding matched filter per tone: the energy of the last len samples at each tone (a tone of amplitude A fully inside gives A^2)
class ToneBank {
public:
    void configure(double rate, const std::vector<double>& f, int len) {
        k_ = (int)f.size(); L_ = std::max(2, len); inv_ = 1.0 / ((double)L_ * L_);
        rot_.assign((size_t)k_, cf32(1, 0)); step_.resize((size_t)k_);
        for (int i = 0; i < k_; i++) { const double w = -2 * kPi * f[(size_t)i] / rate; step_[(size_t)i] = cf32((float)std::cos(w), (float)std::sin(w)); }
        reset();
    }
    void reset() {
        ring_.assign((size_t)k_ * L_, cf32(0, 0)); sum_.assign((size_t)k_, std::complex<double>(0, 0)); e_.assign((size_t)k_, 0.f); pos_ = 0; n_ = 0;
        for (auto& r : rot_) r = cf32(1, 0);
    }
    void push(cf32 x) {
        for (int i = 0; i < k_; i++) {
            const cf32 y = x * rot_[(size_t)i];
            rot_[(size_t)i] *= step_[(size_t)i];
            cf32& old = ring_[(size_t)i * L_ + pos_];
            sum_[(size_t)i] += std::complex<double>((double)y.real() - old.real(), (double)y.imag() - old.imag());
            old = y;
            e_[(size_t)i] = (float)(std::norm(sum_[(size_t)i]) * inv_);
        }
        if (++pos_ == L_) pos_ = 0;
        if ((++n_ & 1023) == 0) for (auto& r : rot_) r /= std::abs(r);
    }
    int size() const { return k_; }
    int len() const { return L_; }
    float e(int i) const { return e_[(size_t)i]; }
    int best() const { int b = 0; for (int i = 1; i < k_; i++) if (e_[(size_t)i] > e_[(size_t)b]) b = i; return b; }
    float maxE() const { float m = 0; for (float v : e_) m = std::max(m, v); return m; }
private:
    int k_ = 0, L_ = 2, pos_ = 0; uint64_t n_ = 0; double inv_ = 1;
    std::vector<cf32> rot_, step_, ring_; std::vector<std::complex<double>> sum_; std::vector<float> e_;
};

// Symbol clock: a histogram of the timing metric (the largest tone energy) over the phase of a free-running symbol period, averaged over
// about tau symbols; a strobe falls once per symbol and is pulled towards the phase where the metric peaks (the filters then hold exactly
// one symbol). Small clock errors are followed by the averaging.
class SymbolClock {
public:
    void configure(double T, int bins = 32, double tau = 12) {
        T_ = std::max(2.0, T); nb_ = bins; a_ = 1.0 / tau;
        reset();
    }
    void reset() { h_.assign((size_t)nb_, 0.0); acc_.assign((size_t)nb_, 0.0); cnt_.assign((size_t)nb_, 0); ph_ = 0; since_ = 0; next_ = T_; periods_ = 0; best_ = 0; }
    bool push(double m) {
        const double cur = ph_;
        const int b = std::min(nb_ - 1, (int)(cur / T_ * nb_));
        acc_[(size_t)b] += m; cnt_[(size_t)b]++;
        ph_ += 1; if (ph_ >= T_) { ph_ -= T_; endPeriod(); }
        since_ += 1;
        if (since_ < next_) return false;
        since_ -= next_;
        double err = target() - cur;
        while (err > T_ / 2) err -= T_;
        while (err <= -T_ / 2) err += T_;
        next_ = T_ + std::clamp(err, -T_ / 4, T_ / 4) * (periods_ < 4 ? 1.0 : 0.5);
        return periods_ >= 2;
    }
    double period() const { return T_; }
private:
    void endPeriod() {
        periods_++;
        for (int b = 0; b < nb_; b++) {
            const double v = cnt_[(size_t)b] ? acc_[(size_t)b] / cnt_[(size_t)b] : 0.0;
            h_[(size_t)b] = periods_ == 1 ? v : (1 - a_) * h_[(size_t)b] + a_ * v;
            acc_[(size_t)b] = 0; cnt_[(size_t)b] = 0;
        }
        best_ = 0; for (int b = 1; b < nb_; b++) if (h_[(size_t)b] > h_[(size_t)best_]) best_ = b;
    }
    double target() const {
        const double hm = h_[(size_t)((best_ + nb_ - 1) % nb_)], h0 = h_[(size_t)best_], hp = h_[(size_t)((best_ + 1) % nb_)];
        const double den = hm - 2 * h0 + hp;
        const double d = den < 0 ? std::clamp(0.5 * (hm - hp) / den, -0.5, 0.5) : 0.0;
        return (best_ + 0.5 + d) * T_ / nb_;
    }
    double T_ = 2, a_ = 1.0 / 12, ph_ = 0, since_ = 0, next_ = 2; int nb_ = 32, periods_ = 0, best_ = 0;
    std::vector<double> h_, acc_; std::vector<int> cnt_;
};

// A running mean over the last n values
class RunMean {
public:
    explicit RunMean(int n = 10) : n_(n) {}
    void push(double v) { q_.push_back(v); s_ += v; if ((int)q_.size() > n_) { s_ -= q_.front(); q_.pop_front(); } }
    double mean() const { return q_.empty() ? 0.0 : s_ / q_.size(); }
    bool full() const { return (int)q_.size() >= n_; }
    void clear() { q_.clear(); s_ = 0; }
private:
    int n_; std::deque<double> q_; double s_ = 0;
};

// The frame of the continuous modes: the channel at about 8 ksps, the long spectrum searched for the tone set about once a second, and once
// it is found the last replaySec seconds and everything after go through the demodulator of the mode, mixed so that the tones sit at their
// nominal audio frequencies. The tones are followed as they drift; when they are gone for a few searches the mode is told the signal is lost.
class Continuous : public Decoder {
public:
    struct Search { std::vector<double> tones; double halfW = 10, guardStep = 0, minZ = 9, dfLo = -3000, dfHi = 3000, track = 10, replaySec = 16; bool periodic = false; double spacing = 0; int guards = 0; };
    bool configure(double fs) override {
        if (!front_.configure(fs)) return false;
        rate_ = front_.rate();
        psd_.configure(rate_);
        setup(rate_);
        s_ = search();
        reset();
        return true;
    }
    void reset() override {
        front_.reset(); psd_.reset(); hist_.clear(); locked_ = false; df_ = 0; ph_ = 0; misses_ = 0; segCount_ = 0; last_ = Fit(); t_ = 0;
        restart();
    }
    void feed(const cf32* x, size_t n) override {
        front_.process(x, n, [&](cf32 a) { step(a); });
    }
protected:
    virtual void setup(double rate) = 0;          // after configure: the mode builds its filters for this rate
    virtual void restart() = 0;                   // forget the demodulator state
    virtual void sample(cf32 y) = 0;              // one sample, the tones at their nominal frequencies
    virtual void lost() = 0;                      // the tones are gone: finish what is open
    virtual Search search() const = 0;
    double rate() const { return rate_; }
    bool locked() const { return locked_; }
    double offsetHz() const { return df_; }
    const Fit& lastFit() const { return last_; }
    double seconds() const { return t_; }
private:
    void step(cf32 a) {
        t_ += 1.0 / rate_;
        hist_.push_back(a);
        const Search& s = s_;
        const size_t cap = (size_t)(s.replaySec * rate_);
        while (hist_.size() > cap) hist_.pop_front();
        if (locked_) mixOut(a);
        if (!psd_.push(a)) return;
        segCount_++;
        if (locked_ && (segCount_ & 1)) return;
        const Fit f = fitComb(psd_, s.tones, s.halfW, s.guardStep, s.guards, s.minZ, s.dfLo, s.dfHi);
        last_ = f;
        if (!locked_) {
            if (f.found) { locked_ = true; df_ = f.df; ph_ = 0; misses_ = 0; restart(); out->energy(); for (const cf32& v : hist_) mixOut(v); }
            return;
        }
        if (!f.found) { if (++misses_ >= 4) { lost(); locked_ = false; restart(); } return; }
        misses_ = 0;
        double d = f.df - df_;
        if (s.periodic && s.spacing > 0) d -= s.spacing * std::round(d / s.spacing);   // a comb moved by whole tones is the same signal
        if (std::fabs(d) <= s.track) df_ += 0.5 * d;
        // a larger jump of the best offset is noise on a broad maximum, or another signal: the lock stays until the tones are gone
        // (a relock with a replay would read the same traffic twice)
    }
    void mixOut(cf32 v) {
        const cf32 lo((float)std::cos(ph_), (float)-std::sin(ph_));
        ph_ += 2 * kPi * df_ / rate_;
        if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
        sample(v * lo);
    }
    Front front_; Psd psd_; std::deque<cf32> hist_; Fit last_; Search s_;
    double rate_ = kWork, df_ = 0, ph_ = 0, t_ = 0; bool locked_ = false; int misses_ = 0; long segCount_ = 0;
};

// ITA2 (Baudot) as Rivet prints it: letters and the US figures case; index 0, 27 (figures) and 31 (letters) print nothing
inline const char* baudotLetter(int c) {
    static const char* k[32] = {"", "E", "\n", "A", " ", "S", "I", "U", "\r", "D", "R", "J", "N", "F", "C", "K", "T", "Z", "L", "W", "H", "Y", "P", "Q", "O", "B", "G", "", "M", "X", "V", ""};
    return k[c & 31];
}
inline const char* baudotFigure(int c) {
    static const char* k[32] = {"", "3", "\n", "-", " ", "<BELL>", "8", "7", "\r", "$", "4", "'", ",", "!", ":", "(", "5", "+", ")", "2", "#", "6", "0", "1", "9", "?", "&", "", ".", "/", "=", ""};
    return k[c & 31];
}

} // namespace dect2::x40::rivet
