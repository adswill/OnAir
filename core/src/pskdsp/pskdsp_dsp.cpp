// PSK demodulator building blocks: filters, the streaming channeliser and the demodulator (matched filter, Gardner timing, Costas carrier).
#include "dect2/pskdsp/pskdsp.h"
#include <algorithm>
#include <cmath>

namespace dect2::pskd {

namespace {
constexpr double kPi = 3.14159265358979323846;
double wrapPi(double a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }
void loopGains(double bn, double& kp, double& ki) {   // second order, damping 0.707, detector gain 1, per update
    const double z = 0.7071, th = bn / (z + 0.25 / z), d = 1 + 2 * z * th + th * th;
    kp = 4 * z * th / d;
    ki = 4 * th * th / d;
}
double rrc(double t, double a) {   // t in symbols
    if (a <= 0) return std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / kPi;
    if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-6)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    return (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) / (kPi * t * (1 - 16 * a * a * t * t));
}
} // namespace

const char* orderName(Order o) {
    switch (o) {
    case Order::Bpsk: return "BPSK";
    case Order::Qpsk: return "QPSK";
    case Order::Oqpsk: return "OQPSK";
    case Order::Psk8: return "8PSK";
    default: return "auto";
    }
}
int orderPoints(Order o) { return o == Order::Bpsk ? 2 : o == Order::Qpsk || o == Order::Oqpsk ? 4 : o == Order::Psk8 ? 8 : 0; }
int orderBits(Order o) { return o == Order::Bpsk ? 1 : o == Order::Psk8 ? 3 : o == Order::Auto ? 0 : 2; }

std::vector<float> rrcTaps(double sps, double alpha, int spanSym) {
    const int half = (int)std::ceil(spanSym * sps / 2);
    std::vector<float> h((size_t)(2 * half + 1));
    double e = 0;
    for (int i = -half; i <= half; i++) {
        const double v = rrc(i / sps, alpha);
        h[(size_t)(i + half)] = (float)v;
        e += v * v;
    }
    const double s = 1.0 / std::sqrt(e);
    for (auto& v : h) v = (float)(v * s);
    return h;
}

std::vector<float> lowpassTaps(double fc, int taps) {
    if ((taps & 1) == 0) taps++;
    std::vector<float> h((size_t)taps);
    const int c = taps / 2;
    double s = 0;
    for (int i = 0; i < taps; i++) {
        const double t = i - c;
        const double v = t == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (taps - 1)) + 0.08 * std::cos(4 * kPi * i / (taps - 1));
        h[(size_t)i] = (float)(v * w);
        s += h[(size_t)i];
    }
    for (auto& v : h) v = (float)(v / s);
    return h;
}

cf interpCubic(const cf* v, double mu) {   // v points at sample floor(t) - 1; mu = t - floor(t)
    const float m = (float)mu;
    const cf a = v[0], b = v[1], c = v[2], d = v[3];
    // cubic Lagrange through the four samples
    const float m1 = m + 1, m0 = m, mm1 = m - 1, mm2 = m - 2;
    return a * (-m0 * mm1 * mm2 / 6.f) + b * (m1 * mm1 * mm2 / 2.f) + c * (-m1 * m0 * mm2 / 2.f) + d * (m1 * m0 * mm1 / 6.f);
}

// ---------------------------------------------------------------- channeliser
double Channeliser::planRate(double fsIn, double passHz, double minOut, int* dec, int* up) {
    double P = std::max(passHz, minOut / 3.0);
    P = std::min(P, fsIn * 0.98);
    int cic = 1, upN = 1, fd = 1;
    if (fsIn >= 6 * P) {
        const int tot = std::max(1, (int)std::floor(fsIn / (3 * P)));
        cic = std::max(1, (int)std::floor(fsIn / (16 * P)));
        if (cic > tot) cic = tot;
        if (cic < 4) cic = 1;   // not worth a CIC
        const double fs1 = fsIn / cic;
        fd = std::max(1, (int)std::floor(fs1 / (3 * P)));
    } else if (fsIn < 3 * P) {
        upN = std::max(2, (int)std::ceil(3 * P / fsIn));
    }
    if (dec) *dec = cic * 1000 + fd;
    if (up) *up = upN;
    return fsIn / cic * upN / fd;
}

void Channeliser::configure(double fsIn, double offsetHz, double passHz, double minOutRate) {
    fsIn_ = fsIn; off_ = offsetHz; pass_ = std::min(passHz, fsIn * 0.98);
    int dec = 1001, upN = 1;
    fsOut_ = planRate(fsIn, passHz, minOutRate, &dec, &upN);
    cicD_ = dec / 1000; firD_ = dec % 1000; up_ = upN;
    const double fsF = fsIn / cicD_ * up_;   // rate the FIR runs at
    double cut = 0.55 * pass_;
    if (up_ > 1) cut = std::min(cut, 0.49 * fsIn);
    cut = std::min(cut, 0.45 * fsOut_);
    // stop band starts where aliases (or images) of the band would land
    double stop = fsOut_ - cut;
    if (up_ > 1) stop = std::min(stop, fsIn - cut);
    const double trans = std::max(stop - cut, 0.02 * fsF);
    int taps = (int)std::ceil(5.5 * fsF / trans) + 1;
    taps = std::clamp(taps, 15, 1025);
    fir_ = lowpassTaps(cut / fsF, taps);
    if (up_ > 1) for (auto& v : fir_) v *= (float)up_;
    setOffset(offsetHz);
    reset();
}

void Channeliser::setOffset(double hz) { off_ = hz; dphi_ = fsIn_ > 0 ? -2 * kPi * hz / fsIn_ : 0; }

void Channeliser::reset() {
    phase_ = 0;
    i1_ = i2_ = i3_ = c1_ = c2_ = c3_ = {};
    cicN_ = 0; firN_ = 0;
    hist_.assign(fir_.size() * 2, cf(0, 0));
    hpos_ = 0;
}

void Channeliser::process(const cf* x, size_t n, std::vector<cf>& out) {
    if (fir_.empty()) return;
    const size_t L = fir_.size();
    std::complex<double> ph(std::cos(phase_), std::sin(phase_));
    const std::complex<double> step(std::cos(dphi_), std::sin(dphi_));
    const double cicG = 1.0 / ((double)cicD_ * cicD_ * cicD_);
    auto firIn = [&](cf v) {   // one sample into the FIR at fsF (after the interpolator's zero stuffing: only real samples here)
        for (int p = 0; p < up_; p++) {
            const cf s = p == 0 ? v : cf(0, 0);
            hist_[hpos_] = s; hist_[hpos_ + L] = s;
            hpos_ = (hpos_ + 1) % L;
            if (++firN_ >= firD_) {
                firN_ = 0;
                // hist_[hpos_ .. hpos_+L-1] is oldest .. newest
                const cf* hp = hist_.data() + hpos_;
                float re = 0, im = 0;
                if (up_ == 1) {
                    for (size_t k = 0; k < L; k++) { re += hp[k].real() * fir_[L - 1 - k]; im += hp[k].imag() * fir_[L - 1 - k]; }
                } else {   // only every up_-th history sample is non-zero
                    // newest real sample sits at index L-1-p'; walk the non-zero ones
                    for (size_t k = L - 1 - (size_t)p; k < L; k -= (size_t)up_) {
                        re += hp[k].real() * fir_[L - 1 - k]; im += hp[k].imag() * fir_[L - 1 - k];
                        if (k < (size_t)up_) break;
                    }
                }
                out.emplace_back(re, im);
            }
        }
    };
    for (size_t i = 0; i < n; i++) {
        const std::complex<double> v = std::complex<double>(x[i].real(), x[i].imag()) * ph;
        ph *= step;
        if ((i & 1023) == 1023) ph /= std::abs(ph);
        if (cicD_ <= 1) { firIn(cf((float)v.real(), (float)v.imag())); continue; }
        i1_ += v; i2_ += i1_; i3_ += i2_;
        if (++cicN_ < cicD_) continue;
        cicN_ = 0;
        const std::complex<double> d1 = i3_ - c1_; c1_ = i3_;
        const std::complex<double> d2 = d1 - c2_; c2_ = d1;
        const std::complex<double> d3 = d2 - c3_; c3_ = d2;
        firIn(cf((float)(d3.real() * cicG), (float)(d3.imag() * cicG)));
    }
    phase_ = std::arg(ph);
    // keep the CIC integrators bounded (they wrap in exact arithmetic; doubles drift): re-centre occasionally
    if (std::abs(i3_) > 1e12) { i1_ = i2_ = i3_ = c1_ = c2_ = c3_ = {}; }
}

// ---------------------------------------------------------------- decisions
cf decide(cf z, Order o, int* idx) {
    int k = 0;
    cf p(1, 0);
    switch (o) {
    case Order::Bpsk:
        k = z.real() >= 0 ? 0 : 1;
        p = cf(k ? -1.f : 1.f, 0);
        break;
    case Order::Qpsk: case Order::Oqpsk: {
        const bool ip = z.real() >= 0, qp = z.imag() >= 0;
        k = ip ? (qp ? 0 : 3) : (qp ? 1 : 2);
        const float r = 0.70710678f;
        p = cf(ip ? r : -r, qp ? r : -r);
        break;
    }
    case Order::Psk8: {
        double a = std::atan2(z.imag(), z.real());
        k = ((int)std::lround(a / (kPi / 4)) % 8 + 8) % 8;
        p = cf((float)std::cos(k * kPi / 4), (float)std::sin(k * kPi / 4));
        break;
    }
    default: break;
    }
    if (idx) *idx = k;
    return p;
}

// ---------------------------------------------------------------- demodulator
void Demod::configure(const DemodConfig& c) {
    c_ = c;
    c_.sps = std::max(1.5, c_.sps);
    if (c_.alpha > 0) mf_ = rrcTaps(c_.sps, std::min(1.0, c_.alpha), 10);
    else {
        const int L = std::max(1, (int)std::lround(c_.sps));
        mf_.assign((size_t)L, (float)(1.0 / std::sqrt((double)L)));
    }
    loopGains(c_.timingBn, tKp_, tKi_);
    loopGains(c_.carrierBn, cKp_, cKi_);
    reset();
}

void Demod::reset() {
    mfHist_.assign(mf_.size() * 2, cf(0, 0));
    mfPos_ = 0;
    buf_.clear();
    bufBase_ = 0;
    nextT_ = c_.sps * 2 + 4;
    spsAdj_ = 0;
    phase_ = 0;
    freq_ = c_.initFreq;
    agc_ = 0;
    prevSym_ = prevM_ = cf(0, 0);
    havePrev_ = false;
    evmAcc_ = 0; phVar_ = 1;
    nIn_ = nSym_ = 0;
}

DemodStats Demod::stats() const {
    DemodStats s;
    s.freq = freq_;
    s.spsNow = c_.sps + spsAdj_;
    s.evm = std::sqrt(std::max(0.0, evmAcc_));
    s.lockMetric = std::clamp(1.0 - phVar_ / 0.33, 0.0, 1.0);
    s.symbols = nSym_;
    return s;
}

void Demod::process(const cf* x, size_t n, std::vector<cf>& sym, std::vector<float>* phErr, std::vector<float>* tErr, std::vector<float>* eye) {
    const size_t L = mf_.size();
    const double sps = c_.sps;
    const bool oq = c_.order == Order::Oqpsk;
    const double half = sps / 2;
    const int M = std::max(2, orderPoints(c_.order));
    auto at = [&](double t) -> cf {   // the derotated matched-filter output at absolute fractional index t
        const double rel = t - bufBase_;
        long i = (long)std::floor(rel);
        if (i < 1) i = 1;
        if (i + 2 >= (long)buf_.size()) i = (long)buf_.size() - 3;
        return interpCubic(buf_.data() + (i - 1), rel - (double)i);
    };
    auto aligned = [&](double t) -> cf {   // OQPSK: Q read half a symbol later
        if (!oq) return at(t);
        return cf(at(t).real(), at(t + half).imag());
    };
    for (size_t i = 0; i < n; i++) {
        mfHist_[mfPos_] = x[i]; mfHist_[mfPos_ + L] = x[i];
        mfPos_ = (mfPos_ + 1) % L;
        const cf* hp = mfHist_.data() + mfPos_;
        float re = 0, im = 0;
        for (size_t k = 0; k < L; k++) { re += hp[k].real() * mf_[k]; im += hp[k].imag() * mf_[k]; }
        const cf rot((float)std::cos(phase_), (float)-std::sin(phase_));
        buf_.push_back(cf(re, im) * rot);
        phase_ = wrapPi(phase_ + freq_);
        nIn_++;
        // symbol strobes whose samples (and the half-symbol lookahead of OQPSK) have arrived
        while (nextT_ + (oq ? half : 0) + 3 < bufBase_ + (double)buf_.size()) {
            if (agc_ <= 0) {   // first strobe: start the level from the samples so far
                double p = 0;
                for (const cf& v : buf_) p += std::norm(v);
                agc_ = std::max(1e-20, p / (double)buf_.size());
            }
            const double g = 1.0 / std::sqrt(agc_);
            const cf raw = aligned(nextT_);
            const cf z = raw * (float)g;
            const cf mid = aligned(nextT_ - half) * (float)g;
            agc_ = 0.995 * agc_ + 0.005 * std::norm(raw);
            // timing (Gardner): late strobes give a positive error
            double te = 0;
            if (havePrev_) {
                te = (std::conj(mid) * (z - prevSym_)).real();
                te = std::clamp(te, -1.0, 1.0);
            }
            // carrier: decision directed, plus the FLL from the M-th power
            int idx = 0;
            const cf d = decide(z, c_.order, &idx);
            double pe = std::arg(z * std::conj(d));
            if (std::abs(z) < 0.2f) pe = 0;
            cf zm = z;
            for (int k = 1; k < M; k++) zm *= z;
            double fe = 0;
            if (havePrev_ && c_.fllGain > 0) fe = std::arg(zm * std::conj(prevM_)) / M;
            const double fll = c_.fllGain * (phVar_ < 0.08 ? 0.25 : 1.0);
            freq_ += (cKi_ * pe + fll * fe) / sps;
            phase_ = wrapPi(phase_ + cKp_ * pe);
            phVar_ = 0.99 * phVar_ + 0.01 * pe * pe;
            const double ev = std::norm(z - d);
            evmAcc_ = nSym_ < 20 ? ev : 0.99 * evmAcc_ + 0.01 * ev;
            // eye diagram: the I rail from one symbol before the previous strobe to one after it
            if (eye && havePrev_) {
                const double t0 = nextT_ - 2 * sps;
                for (int k = 0; k < 16; k++) eye->push_back((float)(aligned(t0 + k * sps / 8).real() * g));
            }
            sym.push_back(z);
            if (phErr) phErr->push_back((float)pe);
            if (tErr) tErr->push_back((float)te);
            prevSym_ = z; prevM_ = zm; havePrev_ = true;
            nSym_++;
            // advance
            spsAdj_ -= tKi_ * te * sps;
            spsAdj_ = std::clamp(spsAdj_, -0.02 * sps, 0.02 * sps);
            nextT_ += sps + spsAdj_ - tKp_ * te * sps;
        }
        // drop what no strobe needs any more (keep three symbols behind the next one for the eye)
        if (buf_.size() > 4096 + (size_t)(4 * sps)) {
            const double keepFrom = nextT_ - 3 * sps - 4;
            const long drop = (long)std::floor(keepFrom - bufBase_);
            if (drop > 2048) {
                buf_.erase(buf_.begin(), buf_.begin() + drop);
                bufBase_ += (double)drop;
            }
        }
    }
}

} // namespace dect2::pskd
