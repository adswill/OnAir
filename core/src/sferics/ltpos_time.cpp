// Lightning position (network), timing: the ranked sources, the sample clock discipline, the sferic leading-edge timer, the MSK common-view
// timer and the server's common-view alignment (ltpos.h).
#include "dect2/sferics/ltpos.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dect2::ltpos {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kPriorRate = 50e-6;   // the rate prior: +-50 ppm
constexpr int kWarmBlocks = 3;         // MSK blocks before the first reading goes out
}

// ---------------------------------------------------------------- sources
const char* timeSrcName(TimeSrc s) {
    switch (s) {
    case TimeSrc::GpsHw: return "GPS (radio time)";
    case TimeSrc::Gnss: return "GNSS receiver";
    case TimeSrc::CommonView: return "common view";
    case TimeSrc::Ptp: return "PTP";
    case TimeSrc::Ntp: return "NTP";
    case TimeSrc::System: return "system clock";
    default: return "none";
    }
}
int timeSrcRank(TimeSrc s) {
    switch (s) {
    case TimeSrc::GpsHw: return 1;
    case TimeSrc::Gnss: return 2;
    case TimeSrc::CommonView: return 3;
    case TimeSrc::Ptp: return 4;
    case TimeSrc::Ntp: return 5;
    case TimeSrc::System: return 6;
    default: return 99;
    }
}
double timeSrcTypicalNs(TimeSrc s) {
    switch (s) {
    case TimeSrc::GpsHw: return 100;
    case TimeSrc::Gnss: return 1000;
    case TimeSrc::CommonView: return 3000;
    case TimeSrc::Ptp: return 100e3;
    case TimeSrc::Ntp: return 5e6;
    case TimeSrc::System: return 200e6;
    default: return 1e9;
    }
}
bool timeSrcFineCapable(TimeSrc s) { return s == TimeSrc::GpsHw || s == TimeSrc::Gnss || s == TimeSrc::CommonView; }

TimeMark systemClockMark(int kind, double sampleS, int64_t nowNs, double userSigmaUs, double bufferS) {
    TimeMark m;
    m.src = kind == 2 ? TimeSrc::Ptp : kind == 1 ? TimeSrc::Ntp : TimeSrc::System;
    m.sampleS = sampleS;
    // a block is delivered just after its last sample was taken; the driver's and the ring's own delays are the random part
    const double buf = std::max(0.0, bufferS);
    m.utcNs = nowNs;
    m.sigmaNs = std::max(1000.0, buf * 0.5e9);
    m.biasNs = userSigmaUs > 0 ? userSigmaUs * 1e3 : timeSrcTypicalNs(m.src);
    return m;
}

bool gnssMark(const GnssTime& g, double streamToLocalS, double alignSigmaNs, TimeMark& out) {
    if (!g.valid || !(g.unixAtSignal > 0) || !std::isfinite(g.unixAtSignal)) return false;
    const double sec = std::floor(g.unixAtSignal);
    out.src = TimeSrc::Gnss;
    out.sampleS = g.signalS + streamToLocalS;
    out.utcNs = (int64_t)sec * 1000000000LL + (int64_t)std::llround((g.unixAtSignal - sec) * 1e9);
    const double tdop = g.tdop > 0 ? g.tdop : 1.5;
    const double rms = g.residualRmsM > 0 ? g.residualRmsM : 5.0;
    out.sigmaNs = std::max(20.0, tdop * rms / kC * 1e9);
    out.biasNs = std::max(0.0, alignSigmaNs);
    return true;
}

// ---------------------------------------------------------------- sample clock
void SampleClock::reset() {
    marks_.clear();
    src_ = TimeSrc::None;
    a_ = 0; b_ = 1; varA_ = varB_ = covAB_ = 0; biasNs_ = 0; rmsNs_ = 0; badRun_ = 0;
}

void SampleClock::fit() {
    baseNs_ = marks_.back().utcNs;
    double sw = 0, swx = 0;
    for (const TimeMark& m : marks_) { const double w = 1.0 / std::max(1.0, m.sigmaNs * m.sigmaNs); sw += w; swx += w * m.sampleS; }
    sRef_ = swx / sw;
    // y in seconds relative to the newest mark: a double keeps well under a nanosecond over the window
    double r0 = 0, n11 = 0, r1 = 0;
    for (const TimeMark& m : marks_) {
        const double w = 1e18 / std::max(1.0, m.sigmaNs * m.sigmaNs);   // per second^2
        const double x = m.sampleS - sRef_, y = (double)(m.utcNs - baseNs_) * 1e-9;
        r0 += w * y; n11 += w * x * x; r1 += w * x * y;
    }
    const double n00 = sw * 1e18;
    const double wp = 1.0 / (kPriorRate * kPriorRate);
    n11 += wp; r1 += wp * 1.0;
    a_ = r0 / n00; b_ = r1 / n11;
    varA_ = 1.0 / n00; varB_ = 1.0 / n11; covAB_ = 0;
    double chi = 0, ss = 0;
    biasNs_ = 0;
    for (const TimeMark& m : marks_) {
        const double x = m.sampleS - sRef_, y = (double)(m.utcNs - baseNs_) * 1e-9;
        const double r = (y - a_ - b_ * x) * 1e9;
        chi += r * r / std::max(1.0, m.sigmaNs * m.sigmaNs);
        ss += r * r;
        biasNs_ = std::max(biasNs_, m.biasNs);
    }
    const size_t n = marks_.size();
    rmsNs_ = n > 0 ? std::sqrt(ss / (double)n) : 0;
    if (n >= 3) { const double s2 = chi / (double)(n - 2); if (s2 > 1) { varA_ *= s2; varB_ *= s2; } }
}

bool SampleClock::add(const TimeMark& m) {
    if (!std::isfinite(m.sampleS) || m.sigmaNs < 0) return false;
    if (src_ != m.src || (!marks_.empty() && m.sampleS < marks_.back().sampleS - 1e-6)) { reset(); }   // another source, or the timeline restarted
    if (marks_.size() >= 3) {
        int64_t p; double s;
        stamp(m.sampleS, p, s);
        const double resid = (double)(m.utcNs - p);
        const double thr = 6.0 * std::sqrt(s * s - biasNs_ * biasNs_ + m.sigmaNs * m.sigmaNs) + 3.0 * rmsNs_ + 50.0;
        if (std::fabs(resid) > thr) {
            refused_++;
            if (++badRun_ < 3) return false;
            marks_.clear(); restarts_++;   // the timeline jumped: start again from this mark
        }
    }
    badRun_ = 0;
    src_ = m.src;
    marks_.push_back(m);
    while (marks_.size() > maxMarks_ || (marks_.size() > 1 && marks_.front().sampleS < m.sampleS - windowS_)) marks_.pop_front();
    fit();
    return true;
}

bool SampleClock::stamp(double sampleS, int64_t& utcNs, double& sigmaNs) const {
    if (marks_.empty()) return false;
    const double x = sampleS - sRef_;
    const double y = a_ + b_ * x;
    utcNs = baseNs_ + (int64_t)std::llround(y * 1e9);
    const double v = varA_ + x * x * varB_ + 2 * x * covAB_;
    sigmaNs = std::sqrt(std::max(0.0, v) * 1e18 + biasNs_ * biasNs_);
    return true;
}

double SampleClock::ppm() const { return marks_.empty() ? 0 : (1.0 / b_ - 1.0) * 1e6; }
double SampleClock::ppmSigma() const { return std::sqrt(varB_) * 1e6; }

// ---------------------------------------------------------------- the sferic timer
void PulseTimer::configure(double rate) {
    rate_ = rate;
    W_ = std::max<size_t>(2, (size_t)std::llround(10e-6 * rate));
    B_ = std::max<size_t>(16, (size_t)std::llround(1e-3 * rate));
    hold_ = std::max<size_t>(2, (size_t)std::llround(100e-6 * rate));
    pre_ = std::max<size_t>(4, W_ + (size_t)std::llround(20e-6 * rate));
    maxLen_ = std::max<size_t>(64, (size_t)std::llround(5e-3 * rate));
    reset();
}

void PulseTimer::reset() {
    win_.assign(W_, 0.f);
    wpos_ = 0; wsum_ = 0; blockSum_ = 0; blockN_ = 0; sinceMedian_ = 0;
    blocks_.clear(); floorLin_ = 0; floorOk_ = false;
    n_ = 0; tooLong_ = 0;
    hist_.clear(); active_ = false; ev_.clear(); evStart_ = 0; below_ = 0;
}

double PulseTimer::floorDb() const { return floorOk_ && floorLin_ > 0 ? 10 * std::log10(floorLin_) : -200; }

void PulseTimer::feed(const std::complex<float>* x, size_t n, std::vector<Pulse>& out) {
    if (rate_ <= 0) return;
    const double on = std::pow(10.0, thrDb_ / 10.0), off = on * 0.5;
    for (size_t i = 0; i < n; i++, n_++) {
        const std::complex<float> v = x[i];
        const float p = std::norm(v);
        wsum_ += p - win_[wpos_]; win_[wpos_] = p; wpos_ = (wpos_ + 1) % W_;
        const double ps = std::max(0.0, wsum_ / (double)W_);
        blockSum_ += p;
        if (++blockN_ >= B_) {
            blocks_.push_back((float)(blockSum_ / (double)B_));
            blockSum_ = 0; blockN_ = 0;
            if (blocks_.size() > 2000) blocks_.pop_front();
            if (++sinceMedian_ >= 100 || (!floorOk_ && blocks_.size() >= 300)) {
                sinceMedian_ = 0;
                std::vector<float> b(blocks_.begin(), blocks_.end());
                std::nth_element(b.begin(), b.begin() + (long)b.size() / 2, b.end());
                floorLin_ = b[b.size() / 2];
                floorOk_ = blocks_.size() >= 300 && floorLin_ > 0;
                wsum_ = 0; for (float f : win_) wsum_ += f;   // keep the running sum exact
            }
        }
        if (active_) {
            ev_.push_back(v);
            if (ps < off * floorLin_) { if (++below_ >= hold_) { finish(out); active_ = false; } }
            else below_ = 0;
            if (active_ && ev_.size() > maxLen_ + pre_ + hold_) { tooLong_++; active_ = false; ev_.clear(); }
        } else if (floorOk_ && ps > on * floorLin_) {
            active_ = true; below_ = 0;
            ev_.assign(hist_.begin(), hist_.end());
            ev_.push_back(v);
            evStart_ = n_ - hist_.size();
        }
        if (!active_) { hist_.push_back(v); if (hist_.size() > pre_) hist_.pop_front(); }
        else if (!hist_.empty()) hist_.clear();
    }
}

void PulseTimer::finish(std::vector<Pulse>& out) {
    const size_t m = ev_.size();
    if (m < 4) { ev_.clear(); return; }
    // amplitude envelope with a centred 3-tap power average (no shift), minus nothing: the noise is small next to a sferic
    std::vector<double> a(m);
    for (size_t k = 0; k < m; k++) {
        double s = std::norm(ev_[k]), c = 1;
        if (k > 0) { s += std::norm(ev_[k - 1]); c++; }
        if (k + 1 < m) { s += std::norm(ev_[k + 1]); c++; }
        a[k] = std::sqrt(s / c);
    }
    size_t kp = 0;
    for (size_t k = 1; k < m; k++) if (a[k] > a[kp]) kp = k;
    const double ap = a[kp];
    auto edge = [&](double level, double& slope) {   // last crossing of level before the peak, interpolated
        size_t k = kp;
        while (k > 0 && a[k - 1] >= level) k--;
        if (k == 0) { slope = 0; return 0.0; }
        const double d = a[k] - a[k - 1];
        slope = d;
        return (double)(k - 1) + (d > 0 ? (level - a[k - 1]) / d : 0.5);
    };
    double slope = 0, s10 = 0, s90 = 0;
    const double e50 = edge(0.5 * ap, slope);
    const double e10 = edge(0.1 * ap, s10), e90 = edge(0.9 * ap, s90);
    size_t lo = kp, hi = kp;
    while (lo > 0 && a[lo - 1] >= 0.5 * ap) lo--;
    while (hi + 1 < m && a[hi + 1] >= 0.5 * ap) hi++;
    std::complex<double> f = 0;
    for (size_t k = lo; k < hi && k + 1 < m; k++) f += std::complex<double>(ev_[k + 1]) * std::conj(std::complex<double>(ev_[k]));
    Pulse p;
    p.sampleS = ((double)evStart_ + e50) / rate_;
    const double sa = std::sqrt(std::max(1e-30, floorLin_) / 2.0) / std::sqrt(1.5);   // the envelope's noise
    const double sl = std::max(std::fabs(slope), 1e-12);
    p.sigmaS = std::clamp(sa / sl, 0.02, 5.0) / rate_;
    p.peakDb = (float)(10 * std::log10(std::max(1e-30, ap * ap) / std::max(1e-30, floorLin_)));
    p.amp = (float)ap;
    p.widthUs = (float)((double)(hi - lo + 1) / rate_ * 1e6);
    p.riseUs = (float)(std::max(0.0, e90 - e10) / rate_ * 1e6);
    p.freqHz = (float)(std::arg(f) * rate_ / (2 * kPi));
    out.push_back(p);
    ev_.clear();
}

// ---------------------------------------------------------------- MSK common-view timer
void MskRefTimer::configure(double rate, double offsetHz, double baud, int refId, double blockS) {
    rate_ = rate; off_ = offsetHz; baud_ = baud > 0 ? baud : 200; id_ = refId; blockS_ = std::max(0.2, blockS);
    D_ = std::max<size_t>(1, (size_t)std::floor(rate / (8.0 * baud_)));
    fs2_ = rate / (double)D_;
    subN_ = std::max<size_t>(8, (size_t)std::llround(0.05 * fs2_));
    subsPerBlock_ = std::max<size_t>(2, (size_t)std::llround(blockS_ * fs2_ / (double)subN_));
    reset();
}

void MskRefTimer::reset() {
    ph_ = 0; fErr_ = 0;
    dph_ = rate_ > 0 ? -2 * kPi * off_ / rate_ : 0;
    acc_ = 0; accN_ = 0; nIn_ = 0; nDec_ = 0;
    ap_ = am_ = 0; pz_ = 0; subFill_ = 0;
    subD_.clear(); subP_.clear(); subPow_.clear(); subT_.clear();
    yb_.clear(); tb_.clear();
    hist_.clear(); eps_ = 0; nObs_ = 0; nBlocks_ = 0;
    locked_ = false; lastSigma_ = 0; lastSnr_ = 0;
}

void MskRefTimer::feed(const std::complex<float>* x, size_t n, std::vector<RefObs>& out) {
    if (rate_ <= 0) return;
    for (size_t i = 0; i < n; i++, nIn_++) {
        acc_ += std::complex<double>(x[i]) * std::complex<double>(std::cos(ph_), std::sin(ph_));
        ph_ += dph_;
        if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
        if (++accN_ < D_) continue;
        const std::complex<double> y = acc_ / (double)D_;
        acc_ = 0; accN_ = 0;
        // the centre of the boxcar, in sample time
        const double tk = ((double)nDec_ * (double)D_ + 0.5 * (double)(D_ - 1)) / rate_;
        nDec_++;
        yb_.push_back(y); tb_.push_back(tk);
        const std::complex<double> z = y * y;
        double fr = 0.5 * baud_ * tk;
        fr -= std::floor(fr);
        const std::complex<double> e(std::cos(2 * kPi * fr), std::sin(2 * kPi * fr));
        ap_ += z * std::conj(e);
        am_ += z * e;
        pz_ += std::norm(z);
        if (subT_.size() == subD_.size()) subT_.push_back(tk);   // the start of this sub-block
        if (++subFill_ >= subN_) { subBlock(); if (subD_.size() >= subsPerBlock_) block(out); }
    }
}

void MskRefTimer::subBlock() {
    subD_.push_back(am_ * std::conj(ap_));
    subP_.push_back(ap_ * am_);
    subPow_.push_back(pz_);
    ap_ = am_ = 0; pz_ = 0; subFill_ = 0;
}

void MskRefTimer::block(std::vector<RefObs>& out) {
    const size_t M = subD_.size();
    std::complex<double> sd = 0;
    double sa = 0;
    for (const auto& d : subD_) { sd += d; sa += std::abs(d); }
    const double T = 1.0 / baud_;
    double tau = std::arg(sd) / (2 * kPi) * T;   // the square-law estimate, modulo T
    if (tau < 0) tau += T;
    const double R = sa > 0 ? std::abs(sd) / sa : 0;
    // carrier offset: the product of the two lines turns at 4 x the offset; steer the mixer by half the estimate
    std::complex<double> pp = 0;
    for (size_t j = 1; j < M; j++) pp += subP_[j] * std::conj(subP_[j - 1]);
    const double dt = (double)subN_ / fs2_;
    if (std::abs(pp) > 0) {
        const double df = std::arg(pp) / (2 * kPi * 4.0 * dt);
        fErr_ += (nBlocks_ < 2 ? 1.0 : 0.5) * df;   // full steps at first, then gently
        nBlocks_++;
        dph_ = -2 * kPi * (off_ + fErr_) / rate_;
    }
    const double tc = 0.5 * (subT_.front() + subT_.back() + dt);
    double tauC = tau + T * std::round((tc - tau) / T);   // the boundary nearest the middle of the block
    // decision-directed refinement over the stored samples
    const size_t N = yb_.size();
    const double Ts = T * (1 + eps_);
    const double t0 = tb_.empty() ? 0 : tb_.front(), dts = (double)D_ / rate_;
    const int64_t kLo = (int64_t)std::floor((t0 - tauC) / Ts) - 1, kHi = (int64_t)std::ceil(((tb_.empty() ? 0 : tb_.back()) - tauC) / Ts) + 1;
    std::vector<int> bits;
    if (N > 16 && kHi > kLo) {
        bits.resize((size_t)(kHi - kLo + 1), 0);   // 0: not read (the partial symbols at the ends of the block): left out of the fit
        for (int64_t k = kLo; k <= kHi; k++) {
            const double b0 = tauC + (double)k * Ts;
            const long i1 = std::lround((b0 + 0.15 * Ts - t0) / dts), i2 = std::lround((b0 + 0.85 * Ts - t0) / dts);
            if (i1 < 0 || i2 >= (long)N || i2 <= i1) continue;
            bits[(size_t)(k - kLo)] = std::arg(yb_[(size_t)i2] * std::conj(yb_[(size_t)i1])) >= 0 ? 1 : -1;
        }
    }
    std::vector<double> theta(bits.size() + 1, 0);
    for (size_t k = 0; k < bits.size(); k++) theta[k + 1] = theta[k] + bits[k] * kPi / 2;
    // the model of one decimated sample: the boxcar average (width W) of the MSK phasor, split at a symbol boundary inside the window
    const double W = (double)D_ / rate_;
    auto model = [&](double t, double tc0, int& sign) -> std::complex<double> {
        std::complex<double> acc = 0;
        double a = t - 0.5 * W;
        const double end = t + 0.5 * W;
        sign = 0;
        while (a < end - 1e-15) {
            int64_t k = (int64_t)std::floor((a - tc0) / Ts);
            if (tc0 + (double)(k + 1) * Ts <= a + 1e-15) k++;   // rounding put a on the boundary of the previous symbol
            const int64_t kk = k - kLo;
            const double bnd = tc0 + (double)k * Ts;
            const double bEnd = std::max(std::min(end, bnd + Ts), a + 1e-12);
            if (kk < 0 || kk >= (int64_t)bits.size()) return 0;
            const int bt = bits[(size_t)kk];
            if (bt == 0) { sign = 0; return 0; }
            if (sign == 0) sign = bt;
            const double w = bt * kPi / (2 * Ts);
            const double pa = theta[(size_t)kk] + w * (a - bnd);
            const double L = bEnd - a;
            acc += std::polar(1.0, pa) * std::complex<double>(std::sin(w * L), 1 - std::cos(w * L)) / w;   // (e^{jwL} - 1) / (j w)
            a = bEnd;
        }
        return acc / W;
    };
    // the samples of the fit are fixed once: one sample more or less as the timing moves would shift the objective by more than the timing does
    std::vector<char> use(N, 0);
    for (size_t i = 0; i < N && !bits.empty(); i++) {
        int s1 = 0, s2 = 0;
        model(tb_[i], tauC - T / 4, s1);
        model(tb_[i], tauC + T / 4, s2);
        use[i] = s1 != 0 && s2 != 0;
    }
    auto J = [&](double tc0, double* amp, double* resid, double* nPlus) {
        double sum = 0, p = 0, np = 0, fitted = 0, mm = 0;
        std::complex<double> c = 0;
        size_t inSub = 0;
        for (size_t i = 0; i < N; i++) {
            if (!use[i]) continue;
            int sign = 0;
            const std::complex<double> m = model(tb_[i], tc0, sign);
            if (sign == 0) continue;
            c += yb_[i] * std::conj(m);
            mm += std::norm(m);
            p += std::norm(yb_[i]);
            if (sign > 0) np++;
            if (++inSub >= subN_) { sum += std::abs(c); if (mm > 0) fitted += std::norm(c) / mm; c = 0; mm = 0; inSub = 0; }
        }
        sum += std::abs(c);
        if (mm > 0) fitted += std::norm(c) / mm;
        if (amp) *amp = sum;
        if (resid) *resid = std::max(0.0, p - fitted);
        if (nPlus) *nPlus = np;
        return fitted;   // the energy the model explains: the corners' amplitude loss must not pull the maximum
    };
    double refined = tauC, sigma = 0, fineHz = 0;
    bool good = false;
    if (!bits.empty()) {
        auto search = [&](double h) {
            for (int it = 0; it < 40 && h > T / 20000; it++) {
                const double jm = J(refined - h, nullptr, nullptr, nullptr), j0 = J(refined, nullptr, nullptr, nullptr), jp = J(refined + h, nullptr, nullptr, nullptr);
                const double den = jm - 2 * j0 + jp;
                double x = 0;
                if (den < 0) x = std::clamp(0.5 * h * (jm - jp) / den, -h, h);
                else x = jp > jm ? h : (jm > jp ? -h : 0);
                refined += x;
                if (std::fabs(x) < 0.75 * h) h /= 4;   // inside the bracket: look closer; else keep walking at this step
            }
        };
        search(T / 16);
        // a residual carrier offset of even 0.01 Hz couples with the bit pattern and moves the timing by microseconds, so it is measured from
        // the samples with the data taken off (the slope of the sub-block phases over the whole block), removed, and the timing found again
        for (int pass = 0; pass < 2; pass++) {
            std::vector<double> tj, pj;
            std::complex<double> c = 0;
            double ts = 0;
            size_t inSub = 0;
            double prev = 0;
            for (size_t i = 0; i < N; i++) {
                if (!use[i]) continue;
                int sign = 0;
                const std::complex<double> m = model(tb_[i], refined, sign);
                if (sign == 0) continue;
                c += yb_[i] * std::conj(m);
                ts += tb_[i];
                if (++inSub >= subN_) {
                    double a = std::arg(c);
                    if (!pj.empty()) { while (a - prev > kPi) a -= 2 * kPi; while (a - prev < -kPi) a += 2 * kPi; }
                    prev = a;
                    tj.push_back(ts / (double)inSub); pj.push_back(a);
                    c = 0; ts = 0; inSub = 0;
                }
            }
            if (pj.size() < 3) break;
            double mt = 0, mp = 0;
            for (size_t j = 0; j < pj.size(); j++) { mt += tj[j]; mp += pj[j]; }
            mt /= (double)pj.size(); mp /= (double)pj.size();
            double sxx = 0, sxy = 0;
            for (size_t j = 0; j < pj.size(); j++) { sxx += (tj[j] - mt) * (tj[j] - mt); sxy += (tj[j] - mt) * (pj[j] - mp); }
            const double w = sxx > 0 ? sxy / sxx : 0;   // rad/s
            for (size_t i = 0; i < N; i++) yb_[i] *= std::polar(1.0, -w * (tb_[i] - mt));
            fineHz += w / (2 * kPi);
            search(T / 64);
        }
        double amp = 0, res = 0, np = 0;
        J(refined, &amp, &res, &np);
        size_t Nu = 0;
        for (char u : use) Nu += u != 0;
        const double Nf = (double)std::max<size_t>(1, Nu);
        const double A = amp / Nf;                             // the signal amplitude per sample
        const double nv = std::max(1e-30, res / Nf);           // what the model leaves
        const double snr = A * A / nv;
        const double nm = Nf - np;
        const double balance = 4.0 * np * nm / (Nf * Nf);
        const double info = 2.0 * snr * Nf * (kPi / (2 * Ts)) * (kPi / (2 * Ts)) * std::max(1e-3, balance);
        sigma = 1.0 / std::sqrt(info);
        good = std::fabs(refined - tauC) < T / 8 && snr > 1.0;
        lastSnr_ = (float)(10 * std::log10(snr * fs2_ / baud_));   // in a band as wide as the symbol rate
    }
    if (good && std::fabs(fineHz) < 2.0) { fErr_ += fineHz; dph_ = -2 * kPi * (off_ + fErr_) / rate_; }   // the precise value also steers the mixer
    if (!good) { refined = tauC; sigma = T / 50; lastSnr_ = (float)(10 * std::log10(std::max(1e-9, R * R) / std::max(1e-9, 1 - R * R))); }
    RefObs o;
    o.refId = id_;
    o.sampleS = refined;
    o.sigmaS = std::max(1e-9, sigma);
    o.snrDb = lastSnr_;
    o.freqErrHz = fErr_;
    lastSigma_ = o.sigmaS;
    locked_ = good && R > 0.3 && o.sigmaS < T / 200;
    if (locked_) {
        // the sample clock's rate over the last 30 blocks (boundaries are whole periods apart; a long base keeps the noise out)
        hist_.push_back(o.sampleS);
        if (hist_.size() > 30) hist_.pop_front();
        if (hist_.size() >= 2) {
            const double dlt = hist_.back() - hist_.front();
            const double m = std::round(dlt / T);
            if (m > 0) eps_ = std::clamp(dlt / (m * T) - 1, -200e-6, 200e-6);
        }
        nObs_++;
        if (nObs_ > kWarmBlocks) out.push_back(o);   // the first blocks only settle the carrier and the rate
    } else { hist_.clear(); nObs_ = 0; }
    subD_.clear(); subP_.clear(); subPow_.clear(); subT_.clear();
    yb_.clear(); tb_.clear();
}

// ---------------------------------------------------------------- common-view alignment (server)
double wrapPeriod(double x, double T) {
    if (!(T > 0)) return x;
    double r = std::fmod(x, T);
    if (r > T / 2) r -= T; else if (r <= -T / 2) r += T;
    return r;
}

void CommonViewAligner::report(uint32_t station, int64_t anchorNs, double sigmaNs, double lat, double lon, bool absolute, const PropModel& pm) {
    St& s = st_[station];
    s.anchorNs = anchorNs;
    s.sigmaNs = sigmaNs;
    s.absolute = absolute;
    s.delayNs = propagationS(pm, geodesicM(ref_.lat, ref_.lon, lat, lon)) * 1e9;
}

bool CommonViewAligner::correction(uint32_t station, double& corrNs, double& sigmaNs, bool& tiedToUtc) const {
    const auto it = st_.find(station);
    const double T = periodNs();
    if (it == st_.end() || !(T > 0)) return false;
    const int64_t R = st_.begin()->second.anchorNs;   // a common reference so differences fit a double exactly
    auto phase = [&](const St& s) { return std::fmod((double)(s.anchorNs - R), T); };
    // the epoch grid E (relative to R): from the absolute stations (their reading is UTC), else 0
    std::complex<double> e = 0;
    double sE = 0;
    int nAbs = 0;
    for (const auto& kv : st_) {
        if (!kv.second.absolute) continue;
        const double th = 2 * kPi * (phase(kv.second) - kv.second.delayNs) / T;
        e += std::complex<double>(std::cos(th), std::sin(th));
        sE += kv.second.sigmaNs * kv.second.sigmaNs;
        nAbs++;
    }
    double E = 0;
    tiedToUtc = nAbs > 0;
    if (nAbs > 0) { E = std::arg(e) / (2 * kPi) * T; sE = std::sqrt(sE) / nAbs; }
    const St& s = it->second;
    corrNs = s.absolute ? 0.0 : wrapPeriod(E + s.delayNs - phase(s), T);
    const double prop = 2e-4 * s.delayNs;   // the velocity of the reference path, known to about 0.02 %
    sigmaNs = s.absolute ? s.sigmaNs : std::sqrt(s.sigmaNs * s.sigmaNs + sE * sE + prop * prop);
    return true;
}

} // namespace dect2::ltpos
