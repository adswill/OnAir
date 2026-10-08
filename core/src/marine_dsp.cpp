#include "dect2/marine_dsp.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace marine {

namespace {
const double kPi = 3.14159265358979323846;

double bessel0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}
double kaiser(double t, double halfLen, double beta) {
    const double r = t / halfLen;
    if (std::fabs(r) >= 1.0) return 0.0;
    return bessel0(beta * std::sqrt(1.0 - r * r)) / bessel0(beta);
}
double sinc(double x) { return std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// symmetric low-pass, cut-off fc (cycles per sample), odd length
std::vector<float> lowpass(int n, double fc, double beta) {
    std::vector<float> h((size_t)n);
    const int m = n / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) { const double t = i - m; h[(size_t)i] = (float)(2 * fc * sinc(2 * fc * t) * kaiser(t, m + 1, beta)); sum += h[(size_t)i]; }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
Front::Front() { reset(); }

void Front::configure(double inputRate) {
    fs_ = inputRate;
    dec_ = std::max(1, (int)std::floor(fs_ / 96000.0));
    r1_ = fs_ / dec_;
    step_ = r1_ / kBaseRate;
    cicScale_ = 1.0 / (std::pow((double)dec_, 4.0) * 1048576.0);
    const double fc = 9000.0 / r1_;
    table_.assign((size_t)(kPhases + 1) * kTaps, 0.f);
    for (int ph = 0; ph <= kPhases; ph++) {
        const double frac = (double)ph / kPhases;
        double sum = 0;
        float* h = &table_[(size_t)ph * kTaps];
        for (int k = 0; k < kTaps; k++) {
            const double t = (k - (kTaps / 2 - 1)) - frac;
            const double v = 2 * fc * sinc(2 * fc * t) * kaiser(t, kTaps / 2, 8.0);
            h[k] = (float)v; sum += v;
        }
        for (int k = 0; k < kTaps; k++) h[k] = (float)(h[k] / sum);
    }
    setOffsetHz(off_);
    reset();
}

void Front::setOffsetHz(double hz) {
    off_ = hz;
    if (fs_ > 0) { const double w = -2.0 * kPi * hz / fs_; rotStep_ = cf32((float)std::cos(w), (float)std::sin(w)); }
}

void Front::reset() {
    rot_ = cf32(1.f, 0.f); rotCount_ = 0;
    for (auto& a : integ_) for (auto& v : a) v = 0;
    for (auto& a : comb_) for (auto& v : a) v = 0;
    cnt_ = 0;
    buf_.assign(kTaps, cf32(0, 0));
    pos_ = kTaps;
}

void Front::process(const cf32* x, size_t n, std::vector<cf32>& out) {
    if (fs_ <= 0 || table_.empty()) return;
    const float SC = 1048576.f;
    float rr = rot_.real(), ri = rot_.imag();
    const float sr = rotStep_.real(), si = rotStep_.imag();
    for (size_t i = 0; i < n; i++) {
        const float xr = x[i].real(), xi = x[i].imag();
        float zr = xr * rr - xi * ri, zi = xr * ri + xi * rr;
        const float nr = rr * sr - ri * si; ri = rr * si + ri * sr; rr = nr;
        if (++rotCount_ >= 1024) { rotCount_ = 0; const float g = 1.f / std::sqrt(rr * rr + ri * ri); rr *= g; ri *= g; }
        zr = std::max(-8.f, std::min(8.f, zr)); zi = std::max(-8.f, std::min(8.f, zi));
        uint64_t a = (uint64_t)(int64_t)std::lrintf(zr * SC), b = (uint64_t)(int64_t)std::lrintf(zi * SC);
        integ_[0][0] += a; integ_[0][1] += integ_[0][0]; integ_[0][2] += integ_[0][1]; integ_[0][3] += integ_[0][2];
        integ_[1][0] += b; integ_[1][1] += integ_[1][0]; integ_[1][2] += integ_[1][1]; integ_[1][3] += integ_[1][2];
        if (++cnt_ >= dec_) {
            cnt_ = 0;
            float v[2];
            for (int ch = 0; ch < 2; ch++) {
                uint64_t y = integ_[ch][3];
                for (int s = 0; s < 4; s++) { const uint64_t t = y - comb_[ch][s]; comb_[ch][s] = y; y = t; }
                v[ch] = (float)((double)(int64_t)y * cicScale_);
            }
            buf_.push_back(cf32(v[0], v[1]));
        }
    }
    rot_ = cf32(rr, ri);
    resample(out);
}

void Front::resample(std::vector<cf32>& out) {
    const size_t half = kTaps / 2;
    while ((size_t)pos_ + half + 1 < buf_.size()) {
        const size_t i0 = (size_t)pos_;
        const double frac = pos_ - (double)i0;
        const double ph = frac * kPhases;
        const int p0 = (int)ph;
        const float a = (float)(ph - p0);
        const float* h0 = &table_[(size_t)p0 * kTaps];
        const float* h1 = h0 + kTaps;
        const cf32* b = &buf_[i0 - (half - 1)];
        float ar = 0, ai = 0;
        for (int k = 0; k < kTaps; k++) {
            const float h = h0[k] + a * (h1[k] - h0[k]);
            ar += h * b[k].real(); ai += h * b[k].imag();
        }
        out.push_back(cf32(ar, ai));
        pos_ += step_;
    }
    const size_t keep = (size_t)pos_ - (half - 1);
    if (keep > 0 && keep < buf_.size()) { buf_.erase(buf_.begin(), buf_.begin() + (long)keep); pos_ -= (double)keep; }
}

// ---------------------------------------------------------------------------------------------------------------------------------
ToneSlicer::ToneSlicer(double fs, double f1, double f2, int sps)
    : fs_(fs), sps_(sps), w1_(2 * kPi * f1 / fs), w2_(2 * kPi * f2 / fs), r1_((size_t)sps), r2_((size_t)sps), avg_((size_t)sps, 0.f) { period_ = sps; }

void ToneSlicer::reset() {
    ph1_ = ph2_ = 0;
    for (auto& v : r1_) v = cf32(0, 0);
    for (auto& v : r2_) v = cf32(0, 0);
    s1_ = s2_ = cf32(0, 0); idx_ = 0; n_ = 0; lastEmit_ = 0;
    for (auto& v : avg_) v = 0;
    cntSym_ = 0; period_ = sps_; emits_ = 0; rate_ = 0; rateN_ = 0; rateEmits_ = 0;
}

bool ToneSlicer::push(cf32 x, float& soft) {
    const cf32 m1((float)std::cos(ph1_), (float)-std::sin(ph1_)), m2((float)std::cos(ph2_), (float)-std::sin(ph2_));
    ph1_ += w1_; ph2_ += w2_;
    if (ph1_ > kPi) ph1_ -= 2 * kPi; else if (ph1_ < -kPi) ph1_ += 2 * kPi;
    if (ph2_ > kPi) ph2_ -= 2 * kPi; else if (ph2_ < -kPi) ph2_ += 2 * kPi;
    const cf32 p1 = x * m1, p2 = x * m2;
    s1_ += p1 - r1_[(size_t)idx_]; s2_ += p2 - r2_[(size_t)idx_];
    r1_[(size_t)idx_] = p1; r2_[(size_t)idx_] = p2;
    if (++idx_ >= sps_) idx_ = 0;
    n_++;
    if ((n_ & 4095) == 0) {                      // exact sums again: the running sum drifts slowly
        s1_ = s2_ = cf32(0, 0);
        for (int i = 0; i < sps_; i++) { s1_ += r1_[(size_t)i]; s2_ += r2_[(size_t)i]; }
    }
    e1_ = std::norm(s1_); e2_ = std::norm(s2_);
    const float d = (e1_ - e2_) / (e1_ + e2_ + 1e-20f);
    const int q = (int)(n_ % (uint64_t)sps_);
    avg_[(size_t)q] += 0.01f * (std::fabs(d) - avg_[(size_t)q]) * (float)1.0;
    // The symbol clock is a counter: a decision every `period_` samples. The period is sps, plus or minus one sample towards the phase where the
    // averaged |d| peaks (only when that peak is clear), so a wrong estimate can neither double nor drop a symbol.
    rateN_++;
    if (rateN_ >= (uint64_t)(10.0 * fs_)) { rate_ = (double)rateEmits_ / ((double)rateN_ / fs_); rateN_ = 0; rateEmits_ = 0; }
    if (++cntSym_ >= period_) {
        cntSym_ = 0;
        float bv = -1; int bi = 0;
        for (int i = 0; i < sps_; i++) {
            const float v = avg_[(size_t)((i + sps_ - 1) % sps_)] + 2 * avg_[(size_t)i] + avg_[(size_t)((i + 1) % sps_)];
            if (v > bv) { bv = v; bi = i; }
        }
        period_ = sps_;
        if (bv > 4 * 0.55f) {
            int diff = bi - q;
            if (diff > sps_ / 2) diff -= sps_; else if (diff < -sps_ / 2) diff += sps_;
            period_ = sps_ + (diff > 0 ? 1 : diff < 0 ? -1 : 0);
        }
        soft = d;
        emits_++; rateEmits_++;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------------------
namespace { constexpr int kFskTaps = 257; constexpr int kFskDec = 10; }

FskSlicer::FskSlicer() : slicer_(kBaseRate / kFskDec, 85.0, -85.0, 24) {
    taps_ = lowpass(kFskTaps, 260.0 / kBaseRate, 6.0);
    ring_.assign(kFskTaps, cf32(0, 0));
}

void FskSlicer::reset() {
    centre_ = centreTarget_ = 0; phase_ = 0; pos_ = 0; phaseDec_ = 0;
    for (auto& v : ring_) v = cf32(0, 0);
    slicer_.reset();
}

bool FskSlicer::step(cf32 x, cf32& y) {
    centre_ += 0.0005 * (centreTarget_ - centre_);
    phase_ -= 2 * kPi * centre_ / kBaseRate;
    if (phase_ < -kPi) phase_ += 2 * kPi;
    ring_[(size_t)pos_] = x * cf32((float)std::cos(phase_), (float)std::sin(phase_));
    if (++pos_ >= kFskTaps) pos_ = 0;
    if (++phaseDec_ < kFskDec) return false;
    phaseDec_ = 0;
    float ar = 0, ai = 0;
    int idx = pos_;                           // oldest sample
    for (int k = 0; k < kFskTaps; k++) {
        const cf32 v = ring_[(size_t)idx];
        ar += taps_[(size_t)k] * v.real(); ai += taps_[(size_t)k] * v.imag();
        if (++idx >= kFskTaps) idx = 0;
    }
    y = cf32(ar, ai);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
VhfSlicer::VhfSlicer() : slicer_(kBaseRate, 1300.0, 2100.0, 20) {}

void VhfSlicer::reset() { prev_ = cf32(0, 0); lp1_ = lp2_ = 0; slicer_.reset(); }

float VhfSlicer::disc(cf32 x) {
    const cf32 d = x * std::conj(prev_);
    prev_ = x;
    const float hz = std::atan2(d.imag(), d.real()) * (float)(kBaseRate / (2 * kPi));
    lp1_ += 0.5f * (hz - lp1_);
    lp2_ += 0.5f * (lp1_ - lp2_);
    return lp2_ * 0.0003f;                    // about +-1 for a deviation of +-3 kHz
}

// ---------------------------------------------------------------------------------------------------------------------------------
Spectrum::Spectrum() : avg_(kN, 0.f), buf_(kN), win_(kN), tmp_(kN) {
    fft_ = new Fft(kN);
    for (int i = 0; i < kN; i++) { win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kN)); win2_ += (double)win_[(size_t)i] * win_[(size_t)i]; }
}
Spectrum::~Spectrum() { delete static_cast<Fft*>(fft_); }

void Spectrum::reset() {
    for (auto& v : avg_) v = 0;
    fill_ = 0; frames_ = 0; primed_ = false;
}

bool Spectrum::push(const cf32* x, size_t n) {
    bool got = false;
    const double w2 = win2_;
    for (size_t i = 0; i < n; i++) {
        buf_[(size_t)fill_++] = x[i];
        if (fill_ == kN) {
            std::vector<cf32>& t = tmp_;
            for (int k = 0; k < kN; k++) t[(size_t)k] = buf_[(size_t)k] * win_[(size_t)k];
            static_cast<Fft*>(fft_)->forward(t.data());
            const float alpha = primed_ ? 0.25f : 1.f;
            for (int k = 0; k < kN; k++) avg_[(size_t)k] += alpha * ((float)(std::norm(t[(size_t)k]) / w2) - avg_[(size_t)k]);
            primed_ = true; frames_++;
            for (int k = 0; k < kN / 2; k++) buf_[(size_t)k] = buf_[(size_t)(k + kN / 2)];
            fill_ = kN / 2;
            got = true;
        }
    }
    return got;
}

double Spectrum::noisePerBin() const {
    // median over -3 .. +3 kHz (the wanted signal is narrow)
    const int span = (int)(3000.0 / binHz());
    std::vector<float> v;
    v.reserve((size_t)(2 * span + 1));
    for (int k = -span; k <= span; k++) v.push_back(avg_[(size_t)((k + kN) % kN)]);
    std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
    return (double)v[v.size() / 2] * 1.05 + 1e-30;
}

double Spectrum::freqPower(double hz) const {
    const int k = (int)std::lround(hz / binHz());
    return avg_[(size_t)(((k % kN) + kN) % kN)];
}

Spectrum::Fsk Spectrum::findFsk(double shiftHz, double searchHz) const {
    Fsk r;
    if (!primed_) return r;
    const double bw = binHz();
    const int h = (int)std::lround(shiftHz / 2 / bw), box = 17;
    const int sb = (int)(searchHz / bw);
    // prefix sums over -3 .. +3 kHz
    const int lim = (int)(3500.0 / bw);
    std::vector<double> pre((size_t)(2 * lim + 2), 0.0);
    for (int k = -lim; k <= lim; k++) pre[(size_t)(k + lim + 1)] = pre[(size_t)(k + lim)] + avg_[(size_t)((k + kN) % kN)];
    auto S = [&](int x) { return pre[(size_t)(x + box + lim + 1)] - pre[(size_t)(x - box + lim)]; };
    double best = -1; int bc = 0;
    for (int c = -sb; c <= sb; c++) {
        const double m = S(c - h) + S(c + h);
        if (m > best) { best = m; bc = c; }
    }
    const double n0 = noisePerBin();
    const double perBin = best / (2.0 * (2 * box + 1));
    r.levelDb = 10.0 * std::log10(perBin / n0 + 1e-12);
    r.centreHz = bc * bw;
    // signal power: bins from the low box to the high box
    double sum = 0;
    for (int k = bc - h - box; k <= bc + h + box; k++) sum += std::max(0.0, (double)avg_[(size_t)((k % kN + kN) % kN)] - n0);
    const double psig = sum / kN, pn3k = n0 * 3000.0 / kBaseRate;
    r.snrDb = 10.0 * std::log10(std::max(1e-9, psig / pn3k));
    r.found = r.levelDb > 5.0;
    // measured tone places: the largest bin within +-40 Hz of each expected place (smoothed over 5 bins)
    auto peak = [&](int centreBin) {
        double bv = -1; int bi = centreBin;
        const int w = (int)(40.0 / bw);
        for (int k = centreBin - w; k <= centreBin + w; k++) {
            double v = 0; for (int j = -2; j <= 2; j++) v += avg_[(size_t)(((k + j) % kN + kN) % kN)];
            if (v > bv) { bv = v; bi = k; }
        }
        return bi * bw;
    };
    r.highHz = peak(bc + h);
    r.lowHz = peak(bc - h);
    return r;
}

void Spectrum::audioDb(std::vector<float>& out, double loHz, double hiHz, int bins) const {
    out.assign((size_t)bins, -120.f);
    const double bw = binHz(), step = (hiHz - loHz) / bins;
    for (int i = 0; i < bins; i++) {
        const int a = (int)std::floor((loHz + i * step) / bw), b = std::max(a, (int)std::floor((loHz + (i + 1) * step) / bw) - 1);
        float m = 0;
        for (int k = a; k <= b; k++) m = std::max(m, avg_[(size_t)(((k % kN) + kN) % kN)]);
        out[(size_t)i] = 10.f * std::log10(m + 1e-12f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
namespace { constexpr int kFaxTaps = 301; }

FaxAudio::FaxAudio() {
    const auto lp = lowpass(kFaxTaps, 1950.0 / kBaseRate, 6.0);
    taps_.resize(kFaxTaps);
    const double w0 = 2 * kPi * 2000.0 / kBaseRate;     // band 50 .. 3950 Hz: the 300 Hz start tone and the 450 Hz stop tone must pass
    for (int i = 0; i < kFaxTaps; i++) {
        const double t = i - kFaxTaps / 2;
        // the ring is read oldest first, which reverses the filter in time: the modulation is conjugated to keep the band at +2000 Hz
        taps_[(size_t)i] = cf32((float)(lp[(size_t)i] * std::cos(w0 * t)), (float)(-lp[(size_t)i] * std::sin(w0 * t)));
    }
    ring_.assign(kFaxTaps, cf32(0, 0));
}

void FaxAudio::reset() { for (auto& v : ring_) v = cf32(0, 0); pos_ = 0; phase_ = 0; env_ = 0; }

void FaxAudio::process(const cf32* x, size_t n, std::vector<float>& out) {
    for (size_t i = 0; i < n; i++) {
        ring_[(size_t)pos_] = x[i];
        if (++pos_ >= kFaxTaps) pos_ = 0;
        if (++phase_ < 2) continue;
        phase_ = 0;
        float ar = 0, ai = 0;
        int idx = pos_;
        for (int k = 0; k < kFaxTaps; k++) {
            const cf32 v = ring_[(size_t)idx], t = taps_[(size_t)k];
            ar += t.real() * v.real() - t.imag() * v.imag();
            ai += t.real() * v.imag() + t.imag() * v.real();
            if (++idx >= kFaxTaps) idx = 0;
        }
        const float mag = std::sqrt(ar * ar + ai * ai);
        env_ += (mag > env_ ? 0.01f : 0.00005f) * (mag - env_);       // fast attack, slow release
        const float g = 0.5f / (env_ + 1e-9f);
        out.push_back(ar * g);
    }
}

} // namespace marine
} // namespace dect2
