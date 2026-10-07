// Analog TV receiver, radio side. See atv_front.h.
#include "atv_front.h"
#include "atv_dsp.h"
#include "dect2/fftutil.h"
#include "dect2/dsp_compat.h"
#include "dect2/exact_resampler.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

using namespace atvdsp;

// ------------------------------------------------------------------------------------------------ symmetric decimating FIR on a real stream

struct AtvFront::Dec {
    std::vector<float> h;
    std::vector<float> hist, x;
    int d = 1, cnt = 0;
    void design(std::vector<float> taps, int dec) { h = std::move(taps); d = std::max(1, dec); reset(); }
    void reset() { hist.assign(h.size() - 1, 0.f); cnt = 0; }
    // writes the outputs; one output per d inputs; group delay (taps - 1) / 2 input samples
    size_t process(const float* in, size_t n, float* out) {
        const size_t nt = h.size(), base = nt - 1;
        x.resize(base + n);
        memcpy(x.data(), hist.data(), base * sizeof(float));
        memcpy(x.data() + base, in, n * sizeof(float));
        size_t o = 0;
        const size_t j0 = (size_t)(d - 1 - cnt);
        if (j0 < n) {
            o = (n - 1 - j0) / (size_t)d + 1;
            desamp(x.data() + j0, d, h.data(), out, (int)o, (int)nt);
        }
        cnt = (int)(((size_t)cnt + n) % (size_t)d);
        memcpy(hist.data(), x.data() + n, base * sizeof(float));
        return o;
    }
};

// ------------------------------------------------------------------------------------------------ carrier search

struct AtvCarrierSearch::Fx { Fft fft; explicit Fx(int n) : fft(n) {} };

void AtvCarrierSearch::configure(double fs) {
    fs_ = fs;
    fft_ = std::make_shared<Fx>((int)nfft_);
    win_.resize(nfft_);
    for (size_t i = 0; i < nfft_; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)nfft_));
    reset();
}

void AtvCarrierSearch::reset() {
    acc_.assign(nfft_, 0.f);
    buf_.assign(nfft_, cf32(0, 0));
    fill_ = 0; skip_ = 0; count_ = 0;
}

void AtvCarrierSearch::feed(const cf32* x, size_t n) {
    if (!fft_) return;
    size_t i = 0;
    const size_t hop = monitor_ ? std::max(2 * nfft_, (size_t)(fs_ / 40)) : 2 * nfft_;   // searching: half of the time is used, a spectrum costs 1/16th of a real-time FFT chain
    while (i < n) {
        if (skip_) { const size_t k = std::min(skip_, n - i); skip_ -= k; i += k; continue; }
        const size_t k = std::min(nfft_ - fill_, n - i);
        memcpy(&buf_[fill_], x + i, k * sizeof(cf32));
        fill_ += k; i += k;
        if (fill_ == nfft_) {
            std::vector<cf32> t(nfft_);
            for (size_t j = 0; j < nfft_; j++) t[j] = buf_[j] * win_[j];
            fft_->fft.forward(t.data());
            if (monitor_) for (size_t j = 0; j < nfft_; j++) acc_[j] = count_ == 0 ? std::norm(t[j]) : acc_[j] + 0.3f * (std::norm(t[j]) - acc_[j]);
            else for (size_t j = 0; j < nfft_; j++) acc_[j] += std::norm(t[j]);
            count_++;
            fill_ = 0; skip_ = hop - nfft_;
        }
    }
}

void AtvCarrierSearch::setMonitor(bool on) { monitor_ = on; reset(); }

void AtvCarrierSearch::display(double centreHz, double loMhz, double hiMhz, int points, std::vector<float>& out) const {
    out.assign((size_t)std::max(points, 0), -120.f);
    if (!monitor_ || count_ == 0 || points <= 0) return;
    const double binHz = fs_ / (double)nfft_;
    float mx = 1e-20f;
    for (int i = 0; i < points; i++) {
        const double f0 = centreHz + (loMhz + (hiMhz - loMhz) * i / points) * 1e6, f1 = centreHz + (loMhz + (hiMhz - loMhz) * (i + 1) / points) * 1e6;
        if (f1 < -fs_ / 2 || f0 > fs_ / 2) continue;
        const long k0 = (long)std::floor(f0 / binHz), k1 = std::max(k0, (long)std::floor(f1 / binHz) - 1);
        float p = 0;
        for (long k = k0; k <= k1; k++) {
            const long kk = ((k % (long)nfft_) + (long)nfft_) % (long)nfft_;
            p = std::max(p, acc_[(size_t)kk]);
        }
        out[(size_t)i] = p;
        mx = std::max(mx, p);
    }
    for (auto& v : out) v = v > 0 ? 10 * std::log10(std::max(v / mx, 1e-12f)) : -120.f;
}

bool AtvCarrierSearch::peakNear(double centreHz, double halfWidthHz, double& fHz, double& promDb, double& levelDb) const {
    if (!monitor_ || count_ < 4) return false;
    const double binHz = fs_ / (double)nfft_;
    const long n = (long)nfft_, k0 = (long)std::floor((centreHz - halfWidthHz) / binHz), k1 = (long)std::ceil((centreHz + halfWidthHz) / binHz);
    auto at = [&](long k) { return acc_[(size_t)(((k % n) + n) % n)]; };
    long kb = k0;
    for (long k = k0; k <= k1; k++) if (at(k) > at(kb)) kb = k;
    const double a = std::log(std::max(at(kb - 1), 1e-30f)), b = std::log(std::max(at(kb), 1e-30f)), d = std::log(std::max(at(kb + 1), 1e-30f));
    const double den = a - 2 * b + d, delta = den < 0 ? 0.5 * (a - d) / den : 0.0;
    fHz = ((double)kb + delta) * binHz;
    double rest = 0;
    long c = 0;
    for (long k = k0; k <= k1; k++) if (std::labs(k - kb) > 3) { rest += at(k); c++; }
    promDb = c ? 10 * std::log10((double)at(kb) / std::max(rest / (double)c, 1e-30)) : 0.0;
    levelDb = 10 * std::log10(std::max((double)at(kb), 1e-30));
    return true;
}

void AtvCarrierSearch::spectrumDb(std::vector<float>& db, double& binHz) const {
    db.assign(nfft_, -120.f);
    binHz = fs_ / (double)nfft_;
    if (count_ == 0) return;
    float mx = 1e-20f;
    for (float a : acc_) mx = std::max(mx, a);
    for (size_t k = 0; k < nfft_; k++) {
        const size_t src = (k + nfft_ / 2) % nfft_;                 // fftshift
        db[k] = 10 * std::log10(std::max(acc_[src] / mx, 1e-12f));
    }
}

std::vector<AtvCarrier> AtvCarrierSearch::candidates() const {
    std::vector<AtvCarrier> out;
    if (count_ < 4) return out;
    const int n = (int)nfft_;
    const double binHz = fs_ / n;
    std::vector<float> db((size_t)n);
    for (int k = 0; k < n; k++) db[(size_t)k] = 10 * std::log10(std::max(acc_[(size_t)k] / (float)count_, 1e-20f));
    auto at = [&](int k) { return db[(size_t)((k % n + n) % n)]; };
    auto pw = [&](int k) { return std::pow(10.0, at(k) / 10); };
    auto bin = [&](double hz) { return (int)std::lround(hz / binHz); };
    auto freqOf = [&](int k) { return (k < n / 2 ? k : k - n) * binHz; };
    // local maxima that rise above the lines around them
    struct Cand { int k; double prom; };
    std::vector<Cand> cs;
    const int near = bin(24e3), far = bin(90e3);
    for (int k = 0; k < n; k++) {
        const double f = freqOf(k);
        if (std::fabs(f) < 12e3 || std::fabs(f) > fs_ / 2 - 150e3) continue;
        const float v = at(k);
        bool top = true;
        for (int d = -8; d <= 8 && top; d++) if (d != 0 && at(k + d) > v) top = false;
        if (!top) continue;
        double s = 0; int c = 0;
        for (int d = near; d <= far; d += 2) { s += pw(k + d) + pw(k - d); c += 2; }
        const double prom = v - 10 * std::log10(s / c);
        if (prom > 9) cs.push_back({k, prom});
    }
    std::sort(cs.begin(), cs.end(), [&](const Cand& a, const Cand& b) { return at(a.k) > at(b.k); });
    if (cs.size() > 6) cs.resize(6);
    for (const auto& c : cs) {
        AtvCarrier r;
        const double a = at(c.k - 1), b = at(c.k), d = at(c.k + 1);
        const double den = a - 2 * b + d;
        const double delta = den < 0 ? 0.5 * (a - d) / den : 0.0;
        r.visionHz = freqOf(c.k) + delta * binHz;
        r.visionDb = c.prom;
        // the sound carrier: an FM carrier at one of the standard spacings, a bump above its surroundings
        double bestProm = 0;
        for (int s = 0; s < kAtvSpacingCount; s++) {
            const double fsnd = r.visionHz + atvSpacingMhz(s) * 1e6;
            if (std::fabs(fsnd) > fs_ / 2 - 150e3) continue;
            const int kc = bin(fsnd < 0 ? fsnd + fs_ : fsnd);
            double peak = -200;
            for (int d = -bin(30e3); d <= bin(30e3); d++) {                  // the carrier moves a little with the modulation
                double sm = 0;
                for (int e = -3; e <= 3; e++) sm += pw(kc + d + e);
                peak = std::max(peak, 10 * std::log10(sm / 7));
            }
            double fl = 0; int cnt = 0;
            for (int d = bin(150e3); d <= bin(320e3); d += 3) { fl += pw(kc + d) + pw(kc - d); cnt += 2; }
            const double prom = peak - 10 * std::log10(fl / cnt);
            if (prom > bestProm) { bestProm = prom; r.spacingMhz = atvSpacingMhz(s); r.soundHz = fsnd; r.soundDb = prom; r.soundRelDb = peak - at(c.k); }
        }
        // The FM carrier is a strong line (40 dB above its surroundings; the line of the colour subcarrier is 10 to 15 dB, and well below the vision carrier)
        if (bestProm < 16 || r.soundRelDb < -30) { r.spacingMhz = 0; r.soundHz = 0; r.soundDb = 0; r.soundRelDb = -99; }
        // flat double sideband or Nyquist slope
        double lo = 0, hi = 0;
        for (int d = bin(0.3e6); d <= bin(0.6e6); d++) { lo += pw(c.k - d); hi += pw(c.k + d); }
        r.dsbDb = 10 * std::log10(std::max(lo, 1e-20) / std::max(hi, 1e-20));
        r.score = std::min(r.visionDb, 40.0) + (r.spacingMhz > 0 ? 10 : 0);        // a bare carrier stands out more than a modulated one: do not let that win
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const AtvCarrier& a, const AtvCarrier& b) { return a.score > b.score; });
    return out;
}

// ------------------------------------------------------------------------------------------------ sound channel

struct AtvSound::Impl {
    double fs = 0, fs1 = 0, fs2 = 0, devHz = 50e3, tauUs = 50;
    int d1 = 1, d2 = 5, d3 = 5;
    std::vector<float> h1, h2, h3;
    std::vector<float> a1i, a1q, a2i, a2q;
    // plain streaming decimators on complex data (I and Q kept apart)
    struct Dc { std::vector<float> h, hi, hq, xi, xq; int d = 1, cnt = 0;
        void design(std::vector<float> t, int dd) { h = std::move(t); d = dd; hi.assign(h.size() - 1, 0.f); hq = hi; cnt = 0; }
        void reset() { std::fill(hi.begin(), hi.end(), 0.f); std::fill(hq.begin(), hq.end(), 0.f); cnt = 0; }
        size_t run(const float* ii, const float* qq, size_t n, float* oi, float* oq) {
            const size_t nt = h.size(), base = nt - 1;
            xi.resize(base + n); xq.resize(base + n);
            memcpy(xi.data(), hi.data(), base * sizeof(float)); memcpy(xi.data() + base, ii, n * sizeof(float));
            memcpy(xq.data(), hq.data(), base * sizeof(float)); memcpy(xq.data() + base, qq, n * sizeof(float));
            size_t o = 0;
            const size_t j0 = (size_t)(d - 1 - cnt);
            if (j0 < n) {
                o = (n - 1 - j0) / (size_t)d + 1;
                desamp(xi.data() + j0, d, h.data(), oi, (int)o, (int)nt);
                desamp(xq.data() + j0, d, h.data(), oq, (int)o, (int)nt);
            }
            cnt = (int)(((size_t)cnt + n) % (size_t)d);
            memcpy(hi.data(), xi.data() + n, base * sizeof(float)); memcpy(hq.data(), xq.data() + n, base * sizeof(float));
            return o;
        }
    } s1, s2;
    Dc s3;                                   // real audio: the Q input is zero and ignored
    ExactResampler rs;
    bool rsOk = false;
    double ph = 0;
    cf32 prev = cf32(1, 0);
    double deemY = 0, deemA = 0;
    // statistics
    double envSum = 0, envSq = 0, devMax = 0, audSq = 0, meanSum = 0; size_t statN = 0, audN = 0, meanN = 0;
    float gain = 0;                          // fade in and out with the carrier
    std::vector<float> t1i, t1q, t2i, t2q, t3i, t3q, aud, disc, zero, mi, mq;
    double deemX1 = 0;
    std::vector<cf32> rin, rout;
};

void AtvSound::configure(double fs) {
    fs_ = fs;
    p_ = std::make_shared<Impl>();
    Impl& s = *p_;
    s.fs = fs;
    s.d1 = std::max(1, (int)std::lround(fs / 1.25e6));
    s.fs1 = fs / s.d1;
    s.d2 = std::max(1, (int)std::lround(s.fs1 / 250e3));
    s.fs2 = s.fs1 / s.d2;
    s.s1.design(lowpass(250e3, std::min(1.0e6, s.fs1 - 250e3), fs, 55, 301), s.d1);
    s.s2.design(lowpass(100e3, std::min(150e3, s.fs2 - 100e3), s.fs1, 55, 301), s.d2);
    s.d3 = std::max(1, (int)std::lround(s.fs2 / 50e3));
    s.s3.design(lowpass(15e3, 22e3, s.fs2, 50, 301), s.d3);
    s.rsOk = s.rs.configure(s.fs2 / s.d3, 48000.0);                 // exact ratio, whatever the radio's rate
    reset();
}

void AtvSound::setSystem(double devKhz, double preEmphUs) {
    if (!p_) return;
    p_->devHz = devKhz * 1e3; p_->tauUs = preEmphUs;
    const double k = 2 * p_->fs2 * preEmphUs * 1e-6;           // bilinear transform of 1 / (1 + s tau)
    p_->deemA = (1 - k) / (1 + k);
}

void AtvSound::reset() {
    if (!p_) return;
    Impl& s = *p_;
    s.s1.reset(); s.s2.reset(); s.s3.reset();
    if (s.rsOk) s.rs.reset();
    s.ph = 0; s.prev = cf32(1, 0); s.deemY = 0; s.deemX1 = 0;
    s.envSum = s.envSq = s.devMax = s.audSq = s.meanSum = 0; s.statN = s.audN = s.meanN = 0;
    s.gain = 0;
    present_ = false; devKhz_ = 0; levelDb_ = -120; trim_ = 0;
    setSystem(s.devHz / 1e3, s.tauUs);
}

void AtvSound::process(const cf32* x, size_t n, std::vector<float>& out) {
    if (!p_ || !p_->rsOk) return;
    Impl& s = *p_;
    const double fc = carrier_ + trim_;
    const double w = -2 * kPi * fc / s.fs;
    s.t1i.resize(n / (size_t)s.d1 + 4); s.t1q.resize(s.t1i.size());
    // mix to zero, then the two decimating filters
    std::vector<float>& mi = s.mi;
    std::vector<float>& mq = s.mq;
    mi.resize(n); mq.resize(n);
    {
        // four phasors a sample apart, each stepped by four samples
        const std::complex<double> p0 = std::polar(1.0, s.ph), r1 = std::polar(1.0, w);
        std::complex<float> p[4];
        for (int q = 0; q < 4; q++) p[q] = std::complex<float>(p0 * std::pow(r1, q));
        const std::complex<float> r4(std::pow(r1, 4));
        size_t j = 0;
        for (; j + 4 <= n; j += 4) {
            for (int q = 0; q < 4; q++) {
                const float xr = x[j + q].real(), xi = x[j + q].imag();
                mi[j + q] = xr * p[q].real() - xi * p[q].imag();
                mq[j + q] = xr * p[q].imag() + xi * p[q].real();
                p[q] *= r4;
            }
        }
        for (; j < n; j++) {
            const cf32 v = x[j] * p[0];
            mi[j] = v.real(); mq[j] = v.imag();
            p[0] *= r1;
        }
        s.ph = std::fmod(s.ph + w * (double)n, 2 * kPi);
    }
    const size_t n1 = s.s1.run(mi.data(), mq.data(), n, s.t1i.data(), s.t1q.data());
    s.t2i.resize(n1 / (size_t)s.d2 + 4); s.t2q.resize(s.t2i.size());
    const size_t n2 = s.s2.run(s.t1i.data(), s.t1q.data(), n1, s.t2i.data(), s.t2q.data());
    if (!n2) return;
    // discriminator: frequency in Hz, scaled to full deviation = 1
    s.aud.resize(n2);
    std::vector<float>& disc = s.disc;
    disc.resize(n2);
    for (size_t j = 0; j < n2; j++) {
        const cf32 z(s.t2i[j], s.t2q[j]);
        const cf32 pr = z * std::conj(s.prev);
        s.prev = z;
        const double f = std::atan2((double)pr.imag(), (double)pr.real()) * s.fs2 / (2 * kPi);
        disc[j] = (float)f;
        const double a = std::abs(z);
        s.envSum += a; s.envSq += a * a; s.statN++;
        s.meanSum += f; s.meanN++;
        s.devMax = std::max(s.devMax, std::fabs(f));
    }
    // carrier present? a constant envelope; and where the carrier really is
    if (s.statN >= (size_t)(0.02 * s.fs2)) {
        const double m = s.envSum / s.statN, var = std::max(0.0, s.envSq / s.statN - m * m);
        const double cv = m > 1e-9 ? std::sqrt(var) / m : 1.0;
        present_ = cv < (present_ ? 0.45 : 0.35);
        const double mean = s.meanSum / s.meanN;
        if (present_) trim_ = std::max(-20e3, std::min(20e3, trim_ + 0.3 * mean));
        devKhz_ = (float)(s.devMax / 1e3);
        s.envSum = s.envSq = s.devMax = s.meanSum = 0; s.statN = s.meanN = 0;
    }
    // de-emphasis (one pole, unity gain at DC), then the 15 kHz low-pass and the rate change to 48 kHz
    // bilinear transform of 1 / (1 + s tau) with k = 2 fs tau: y = (x + x1) / (1 + k) - a y1, a = (1 - k) / (1 + k), so 1 / (1 + k) = (1 + a) / 2
    const double kk = (1 + s.deemA) / 2;
    for (size_t j = 0; j < n2; j++) {
        const double xin = disc[j] / s.devHz;
        s.deemY = kk * (xin + s.deemX1) - s.deemA * s.deemY;
        s.deemX1 = xin;
        s.aud[j] = (float)s.deemY;
    }
    std::vector<float>& zero = s.zero;
    zero.assign(n2, 0.f);
    s.t3i.resize(n2 / (size_t)s.d3 + 4); s.t3q.resize(s.t3i.size());
    const size_t n3 = s.s3.run(s.aud.data(), zero.data(), n2, s.t3i.data(), s.t3q.data());
    s.rin.resize(n3);
    for (size_t j = 0; j < n3; j++) s.rin[j] = cf32(s.t3i[j], 0.f);
    s.rout.clear();
    s.rs.process(s.rin.data(), n3, s.rout);
    const float target = present_ ? 1.f : 0.f;
    for (const cf32& v : s.rout) {
        s.gain += (target - s.gain) * 0.002f;
        const float a = v.real() * s.gain;
        out.push_back(a);
        s.audSq += (double)a * a; s.audN++;
    }
    if (s.audN >= 4800) { levelDb_ = (float)(10 * std::log10(std::max(s.audSq / s.audN, 1e-12))); s.audSq = 0; s.audN = 0; }
}

// ------------------------------------------------------------------------------------------------ vision channel

void AtvFront::configure(double fs) {
    fs_ = fs;
    ready_ = false;
    if (fs < 7.9e6) return;
    decim_ = std::max(1, (int)std::floor(fs / 10e6 + 1e-9));
    fv_ = fs / decim_;
    di_ = std::make_shared<Dec>();
    dq_ = std::make_shared<Dec>();
    design();
    ready_ = true;
    reset();
}

void AtvFront::setPlan(double soundSpacingMhz) {
    if (!ready_ || std::fabs(soundSpacingMhz - spacing_) < 0.01) return;
    spacing_ = soundSpacingMhz;
    design();
    reset();
}

void AtvFront::design() {
    // The video band reaches from 1.25 MHz below the carrier to a little below the sound carrier (the sound has to stay out of the picture),
    // and not above what the sample rate holds. A low-pass around the middle of that band, a mixer, and the carrier ends up at 0 Hz.
    const double nyq = 0.5 * fv_ / 1e6;
    upMhz_ = std::min(spacing_ - 0.55, nyq - 0.1);
    const double lowMhz = 1.25;
    centreHz_ = 0.5 * (upMhz_ - lowMhz) * 1e6;
    const double hwPass = 0.5 * (upMhz_ + lowMhz) * 1e6, hwStop = hwPass + 0.4e6;
    const std::vector<float> lp = lowpass(hwPass, hwStop, fs_, 42, 481);
    di_->design(lp, decim_);
    dq_->design(lp, decim_);
    nGain_ = 0;
    for (float t : lp) nGain_ += (double)t * t;                        // white noise in: this much of its power comes out
    vsbMhz_ = spacing_ > 5.8 && spacing_ < 6.1 ? 1.25 : 0.75;
    // the correction for the double-sideband part: gain 0.5 at low frequencies, 1 above the vestigial region, 1 / (1 + skirt)
    const double vs = vsbMhz_ * 1e6, ro = 0.5e6, fvv = fv_;
    const float amt = 1.f;
    auto resp = [&](double f) -> std::complex<double> {
        const double a = std::fabs(f);
        const double skirt = a <= vs ? 1.0 : a >= vs + ro ? 0.0 : std::pow(std::cos(kPi / 2 * (a - vs) / ro), 2);
        return 1.0 / (1.0 + amt * skirt);
    };
    const auto t = fromResponse(resp, fvv, 25, 5.0);
    corrTaps_.assign(t.size(), 0.f);
    for (size_t k = 0; k < t.size(); k++) corrTaps_[k] = (float)t[k].real();
}

void AtvFront::reset() {
    if (!ready_) return;
    di_->reset(); dq_->reset();
    trim_ = 0; trimRate_ = 0;
    ph1_ = 0; nv_ = 0;
    histI_.assign(corrTaps_.size() - 1, 0.f);
    histQ_ = histI_;
}

void AtvFront::setCarrier(double hz) { carrier_ = hz; trim_ = 0; }

void AtvFront::carrierError(double rad, double trust) {
    const double kp = 0.15 * trust, ki = 0.0056 * trust;
    ph1_ += kp * rad;                                          // the mixer multiplies by e^{-j ph1}: more phase there, less in the output
    trim_ = std::max(-30e3, std::min(30e3, trim_ + ki * rad * 15625.0 / (2 * kPi)));
}

size_t AtvFront::process(const cf32* x, size_t n) {
    if (!ready_ || n == 0) return 0;
    n = std::min(n, kMaxBlock);
    const double fmix = carrier_ + trim_ + centreHz_;
    const double w = -2 * kPi * fmix / fs_;
    m1i_.resize(n); m1q_.resize(n);
    {
        // four phasors a quarter of a turn apart in the block, each stepped by four samples: the multiplications do not wait for each other
        const std::complex<double> p0 = std::polar(1.0, -ph1_), r1 = std::polar(1.0, w);
        std::complex<float> p[4];
        for (int q = 0; q < 4; q++) p[q] = std::complex<float>(p0 * std::pow(r1, q));
        const std::complex<float> r4(std::pow(r1, 4));
        size_t j = 0;
        for (; j + 4 <= n; j += 4) {
            for (int q = 0; q < 4; q++) {
                const float xr = x[j + q].real(), xi = x[j + q].imag();
                m1i_[j + q] = xr * p[q].real() - xi * p[q].imag();
                m1q_[j + q] = xr * p[q].imag() + xi * p[q].real();
                p[q] *= r4;
            }
        }
        for (; j < n; j++) {
            const std::complex<float> v = x[j] * p[0];
            m1i_[j] = v.real(); m1q_[j] = v.imag();
            p[0] *= r1;
        }
        ph1_ = std::fmod(ph1_ - w * (double)n, 2 * kPi);
    }
    const size_t cap = n / (size_t)decim_ + 4;
    d1i_.resize(cap); d1q_.resize(cap);
    const size_t m = di_->process(m1i_.data(), n, d1i_.data());
    const size_t m2 = dq_->process(m1q_.data(), n, d1q_.data());
    (void)m2;
    // carrier to 0 Hz: multiply by e^{+j 2 pi centre nv / fv}
    y_i_.resize(m); y_q_.resize(m);
    {
        const double c = centreHz_ / fv_;
        const double p0 = 2 * kPi * std::fmod(c * nv_, 1.0);
        const std::complex<double> pd = std::polar(1.0, p0), rd = std::polar(1.0, 2 * kPi * c);
        std::complex<float> p[4];
        for (int q = 0; q < 4; q++) p[q] = std::complex<float>(pd * std::pow(rd, q));
        const std::complex<float> r4(std::pow(rd, 4));
        size_t j = 0;
        for (; j + 4 <= m; j += 4) {
            for (int q = 0; q < 4; q++) {
                y_i_[j + q] = d1i_[j + q] * p[q].real() - d1q_[j + q] * p[q].imag();
                y_q_[j + q] = d1i_[j + q] * p[q].imag() + d1q_[j + q] * p[q].real();
                p[q] *= r4;
            }
        }
        for (; j < m; j++) {
            y_i_[j] = d1i_[j] * p[0].real() - d1q_[j] * p[0].imag();
            y_q_[j] = d1i_[j] * p[0].imag() + d1q_[j] * p[0].real();
            p[0] *= std::complex<float>(rd);
        }
        nv_ += (double)m;
    }
    // detectors, delayed by half the length of the low-frequency correction so that video and baseband stay aligned
    const size_t nt = corrTaps_.size(), H = nt - 1, dly = nt / 2;
    corrIn_.resize(H + m);
    memcpy(corrIn_.data(), histI_.data(), H * sizeof(float));
    memcpy(corrIn_.data() + H, y_i_.data(), m * sizeof(float));
    std::vector<float>& qi = corrOut_;
    qi.resize(H + m);
    memcpy(qi.data(), histQ_.data(), H * sizeof(float));
    memcpy(qi.data() + H, y_q_.data(), m * sizeof(float));
    i_.resize(m); q_.resize(m); v_.resize(m);
    for (size_t j = 0; j < m; j++) { i_[j] = corrIn_[j + dly]; q_[j] = qi[j + dly]; }
    if (syncDet_) {
        desamp(corrIn_.data(), 1, corrTaps_.data(), v_.data(), (int)m, (int)nt);
        if (dsbAmt_ < 1.f) for (size_t j = 0; j < m; j++) v_[j] = dsbAmt_ * v_[j] + (1.f - dsbAmt_) * 0.5f * i_[j];
    } else {
        for (size_t j = 0; j < m; j++) v_[j] = 0.5f * std::sqrt(i_[j] * i_[j] + q_[j] * q_[j]);
    }
    memcpy(histI_.data(), corrIn_.data() + m, H * sizeof(float));
    memcpy(histQ_.data(), qi.data() + m, H * sizeof(float));
    return m;
}

} // namespace dect2
