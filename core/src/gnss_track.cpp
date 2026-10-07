// One tracking channel (see gnss_track.h).
#include "dect2/gnss_track.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
// Correlate N samples against the replica: out = {E, P, L} as (re, im). rep has N + 2D values; sample n uses rep[n + D] for the prompt, rep[n + 2D] for the
// early one (the code advanced by D samples) and rep[n] for the late one. The carrier is removed with phase theta0 + n * dtheta (cycles).
void correlate(const cf32* x, int N, double theta0, double dtheta, const float* rep, int D, float out[6]) {
    float er[8] = {}, ei[8] = {}, pr[8] = {}, pi[8] = {}, lr[8] = {}, li[8] = {};
    const float* xf = reinterpret_cast<const float*>(x);
    // the carrier in blocks of 64: w[k] = exp(-j 2 pi dtheta k), block phasor z_b
    constexpr int B = 64;
    float wr[B], wi[B];
    {
        double c = 1, s = 0;
        const double cd = std::cos(-2 * M_PI * dtheta), sd = std::sin(-2 * M_PI * dtheta);
        for (int k = 0; k < B; k++) { wr[k] = (float)c; wi[k] = (float)s; const double nc = c * cd - s * sd; s = c * sd + s * cd; c = nc; }
    }
    const double blockAng = -2 * M_PI * dtheta * B;
    const double cb = std::cos(blockAng), sb = std::sin(blockAng);
    double zc = std::cos(-2 * M_PI * theta0), zs = std::sin(-2 * M_PI * theta0);
    int n = 0;
    float yr[B], yi[B];
    while (n < N) {
        const int len = std::min(B, N - n);
        const float zr = (float)zc, zi = (float)zs;
        for (int k = 0; k < len; k++) {
            const float a = xf[2 * (n + k)], b = xf[2 * (n + k) + 1];
            const float tr = a * wr[k] - b * wi[k], ti = a * wi[k] + b * wr[k];
            yr[k] = tr * zr - ti * zi;
            yi[k] = tr * zi + ti * zr;
        }
        const float* rp = rep + n + D;
        const float* re_ = rep + n + 2 * D;
        const float* rl = rep + n;
        int k = 0;
        for (; k + 8 <= len; k += 8)
            for (int l = 0; l < 8; l++) {
                const float a = yr[k + l], b = yi[k + l];
                pr[l] += a * rp[k + l]; pi[l] += b * rp[k + l];
                er[l] += a * re_[k + l]; ei[l] += b * re_[k + l];
                lr[l] += a * rl[k + l]; li[l] += b * rl[k + l];
            }
        for (; k < len; k++) {
            pr[0] += yr[k] * rp[k]; pi[0] += yi[k] * rp[k];
            er[0] += yr[k] * re_[k]; ei[0] += yi[k] * re_[k];
            lr[0] += yr[k] * rl[k]; li[0] += yi[k] * rl[k];
        }
        const double nc = zc * cb - zs * sb;
        zs = zc * sb + zs * cb; zc = nc;
        n += len;
    }
    auto sum = [](const float* a) { float s = 0; for (int l = 0; l < 8; l++) s += a[l]; return s; };
    out[0] = sum(er); out[1] = sum(ei); out[2] = sum(pr); out[3] = sum(pi); out[4] = sum(lr); out[5] = sum(li);
}
} // namespace

void GnssTracker::start(const GnssSignalSpec& sp, const uint8_t* chips, int prn_, double dopplerHz, int64_t idx, double phase) {
    spec = sp;
    sys = sp.sys; prn = prn_;
    chips_.resize((size_t)sp.codeLen);
    for (int i = 0; i < sp.codeLen; i++) chips_[i] = chips[i] ? -1.f : 1.f;
    pos_ = idx; phi_ = phase; fcar_ = dopplerHz; fcar0_ = dopplerHz; theta_ = 0;
    locked_ = lost_ = pullInTimeout_ = false;
    pllInt_ = pllF0_ = 0;
    havePrev_ = false;
    fllErr_ = 1e3;
    ecount_ = locked_epochs_ = pullEpochs_ = 0;
    lockIdx_ = 0;
    m2_ = m4_ = 0; cnInit_ = 0; cn0Db_ = cnSmooth_ = 0;
    lowCn0Epochs_ = lowLockEpochs_ = 0;
    std::memset(hist_, 0, sizeof hist_);
    bitSync_ = frameSync_ = timeValid_ = false;
    bitHistTotal_ = 0; bitAcc_ = 0; bitCount_ = 0; bitsPushed_ = 0; bitsBase_ = 0;
    bits_.clear();
    framesOk_ = framesBad_ = 0; badInRow_ = 0; framesSinceSync_ = 0;
    events_.clear();
    epochT_.clear(); epochTFirst_ = 0;
    scatter_.clear(); cn0Hist_.clear(); lastHistEpoch_ = 0;
    lastEpochT_ = 0;
    // go to the next code epoch boundary: the first (partial) period is not used
    dllInt_ = 0; dllRate_ = 0;
    const double rc = (sp.chipRate + fcar_ * sp.chipRate / sp.rfHz) / sp.fsOut;
    const int skip = (int)std::ceil((sp.codeLen - phi_) / rc);
    phi_ = phi_ + skip * rc - sp.codeLen;
    pos_ += skip;
    theta_ = 0;
    codeHz_ = rc * sp.fsOut;
}

int GnssTracker::state() const {
    if (!locked_) return GnssChPullIn;
    if (!bitSync_) return GnssChLocked;
    if (!frameSync_) return GnssChBitSync;
    return GnssChFrameSync;
}

bool GnssTracker::step(const GnssBand& band) {
    if (lost_) return false;
    const double rc = (spec.chipRate + fcar_ * spec.chipRate / spec.rfHz + dllRate_) / spec.fsOut;     // chips per sample, aided by the carrier
    const int N = (int)std::ceil((spec.codeLen - phi_) / rc);
    if (band.end() < pos_ + N) return false;
    if (pos_ < band.base()) { lost_ = true; return false; }          // fell behind the buffer
    epoch(band, N, (double)pos_ - phi_ / rc);
    return true;
}

void GnssTracker::epoch(const GnssBand& band, int N, double startIdx) {
    const double fs = spec.fsOut;
    const int L = spec.codeLen;
    const double rc = (spec.chipRate + fcar_ * spec.chipRate / spec.rfHz + dllRate_) / fs;
    const double fUsed = fcar_;
    codeHz_ = rc * fs;
    const int D = spec.halfSpacing;
    // replica: rep[j] is the chip at sample (j - D) of this epoch
    rep_.resize((size_t)N + 2 * D);
    {
        double p = phi_ - D * rc;
        for (int j = 0; j < N + 2 * D; j++) {
            double q = p - std::floor(p / L) * L;
            int idx = (int)q; if (idx >= L) idx = L - 1;
            rep_[j] = chips_[idx];
            p += rc;
        }
    }
    float c[6];
    correlate(band.at(pos_), N, theta_, fcar_ / fs, rep_.data(), D, c);
    const double Ie = c[0], Qe = c[1], Ip = c[2], Qp = c[3], Il = c[4], Ql = c[5];
    const double T = (double)N / fs;
    const double amp = std::sqrt(Ip * Ip + Qp * Qp);
    const double ampE = std::sqrt(Ie * Ie + Qe * Qe), ampL = std::sqrt(Il * Il + Ql * Ql);
    // normalise the prompt to the noise level: the variance of a correlation of N unit-variance samples is N
    const double nrm = 1.0 / std::sqrt((double)N);
    const double I = Ip * nrm, Q = Qp * nrm;

    // ---- C/N0: the M2M4 estimator on the prompt magnitudes
    {
        const double p2 = I * I + Q * Q;
        if (cnInit_ < 50) { m2_ += p2; m4_ += p2 * p2; cnInit_++; if (cnInit_ == 50) { m2_ /= 50; m4_ /= 50; } }
        else { m2_ += (p2 - m2_) * 0.002; m4_ += (p2 * p2 - m4_) * 0.002; }
        if (cnInit_ >= 50) {
            const double s2 = 2 * m2_ * m2_ - m4_;
            const double S = s2 > 0 ? std::sqrt(s2) : 0;
            const double Nn = std::max(m2_ - S, 1e-9);
            const double snr = S / Nn;
            const double cn = 10 * std::log10(std::max(snr, 1e-3) / T);
            cnSmooth_ = (cnSmooth_ == 0) ? cn : cnSmooth_ + (cn - cnSmooth_) * 0.01;
            cn0Db_ = cnSmooth_;
        }
    }

    // ---- loops
    const double Tn = T;
    bool pullIn = !locked_;
    if (pullIn) {
        // frequency lock: atan of cross over dot of consecutive prompts (insensitive to a data bit flip)
        if (havePrev_) {
            const double cross = prevI_ * Q - I * prevQ_, dot = prevI_ * I + prevQ_ * Q;
            const double fe = std::atan(cross / (dot == 0 ? 1e-12 : dot)) / (2 * M_PI * Tn);
            fcar_ += 0.12 * fe;
            fllErr_ += (fe - fllErr_) * 0.02;
        }
        pullEpochs_++;
        // frequency settled: try the phase lock loop
        if (pullEpochs_ > 300 && std::fabs(fllErr_) < 10.0 && cnInit_ >= 50 && cn0Db_ > 24.0) {
            locked_ = true; pllInt_ = 0; pllF0_ = fcar_; lockIdx_ = 0; locked_epochs_ = 0;
        } else if ((pullEpochs_ > 1500 && cnInit_ >= 50 && cn0Db_ < 24.0) || pullEpochs_ > 6000 || std::fabs(fcar_ - fcar0_) > 3000.0) { lost_ = true; pullInTimeout_ = true; }
    } else {
        // Costas phase loop, second order, 15 Hz
        const double th = std::atan2(Q, I == 0 ? 1e-12 : I);
        double err = th;
        if (err > M_PI / 2) err -= M_PI; else if (err < -M_PI / 2) err += M_PI;
        const double wn = 15.0 / 0.53, zeta = 0.707;
        pllInt_ += wn * wn * Tn * err;
        fcar_ = pllF0_ + (pllInt_ + 2 * zeta * wn * err) / (2 * M_PI);
        locked_epochs_++;
        // lock indicator: (I^2 - Q^2) / (I^2 + Q^2), averaged
        const double li = (I * I - Q * Q) / (I * I + Q * Q + 1e-12);
        lockIdx_ += (li - lockIdx_) * 0.01;
        if (locked_epochs_ > 300 && lockIdx_ < 0.1) { lowLockEpochs_++; if (lowLockEpochs_ > 500) { locked_ = false; pullEpochs_ = 0; fllErr_ = 1e3; lowLockEpochs_ = 0; bitSync_ = frameSync_ = false; std::memset(hist_, 0, sizeof hist_); bitHistTotal_ = 0; } }
        else lowLockEpochs_ = 0;
        if (cnInit_ >= 50 && locked_epochs_ > 1000) {
            if (cn0Db_ < 17.0) { if (++lowCn0Epochs_ > 3000) lost_ = true; } else lowCn0Epochs_ = 0;
        }
    }
    prevI_ = I; prevQ_ = Q; havePrev_ = true;

    // ---- code loop: non-coherent early minus late, normalised; carrier aiding is in rc
    double dll = 0;
    if (ampE + ampL > 0) dll = (ampE - ampL) / (ampE + ampL);
    // second order: a proportional part and an integrator that absorbs a steady code rate error (a sample clock that does not follow the oscillator)
    double phiNext = phi_ + (double)N * rc - L;
    {
        const double bn = ecount_ < 400 ? 15.0 : (ecount_ < 2000 ? 5.0 : 1.2);
        const double wn = bn / 0.53, zeta = 0.707;
        const double err = 0.5 * dll;                 // chips
        dllInt_ += wn * wn * Tn * err;
        dllRate_ = dllInt_ + 2 * zeta * wn * err;     // chips per second, added to the aided rate for the next epoch
    }

    // ---- bookkeeping: epoch time, scatter, histories
    const double tStart = startIdx / fs;
    lastEpochT_ = tStart;
    epochT_.push_back(tStart);
    if (epochT_.size() > 256) { epochT_.pop_front(); epochTFirst_++; }
    if (locked_) {
        scatter_.push_back({(float)I, (float)Q});
        if (scatter_.size() > 400) scatter_.pop_front();
    }
    if (ecount_ - lastHistEpoch_ >= 1000) {
        lastHistEpoch_ = ecount_;
        cn0Hist_.push_back((float)cn0Db_);
        if (cn0Hist_.size() > 60) cn0Hist_.erase(cn0Hist_.begin());
    }
    // ---- data bits (only with a phase lock: the data sits in I)
    if (locked_ && locked_epochs_ > 100) bitLayer((float)I);
    ecount_++;
    // advance with the frequency that was used during this epoch
    theta_ = std::fmod(theta_ + fUsed / fs * (double)N, 1.0);
    pos_ += N;
    phi_ = phiNext;
}

void GnssTracker::bitLayer(float I) {
    const int sign = I >= 0 ? 1 : -1;
    if (!bitSync_) {
        // transitions of the sign of I between consecutive epochs, counted by the position in a 20 epoch cycle
        if (prevSign_ != 0 && sign != prevSign_) { hist_[ecount_ % 20]++; bitHistTotal_++; }
        prevSign_ = sign;
        if (bitHistTotal_ >= 24) {
            int best = 0, second = 0, at = 0;
            for (int i = 0; i < 20; i++) if (hist_[i] > best) { best = hist_[i]; at = i; }
            for (int i = 0; i < 20; i++) if (i != at && hist_[i] > second) second = hist_[i];
            if (best >= 12 && best >= 3 * second) {
                bitSync_ = true; syncPhase_ = at; bitAcc_ = 0; bitCount_ = 0; firstBitEpoch_ = -1; bitsPushed_ = 0;
                bits_.clear(); bitsBase_ = 0; frameSync_ = false; syncEpoch_ = ecount_;
            }
        }
        return;
    }
    if ((int)(ecount_ % 20) == syncPhase_) {
        if (firstBitEpoch_ < 0) firstBitEpoch_ = (int64_t)ecount_;
        else if (bitCount_ == 20) pushBit(bitAcc_ > 0 ? 1 : 0);
        bitAcc_ = 0; bitCount_ = 0;
    }
    if (firstBitEpoch_ >= 0) { bitAcc_ += I; bitCount_++; }
    // a bit synchronisation that does not lead to a frame is wrong: start again
    if (!frameSync_ && ecount_ - syncEpoch_ > 40000) { bitSync_ = false; std::memset(hist_, 0, sizeof hist_); bitHistTotal_ = 0; prevSign_ = 0; }
}

void GnssTracker::pushBit(int bit) {
    bits_.push_back((uint8_t)bit);
    bitsPushed_++;
    if (bits_.size() > 1200) { bits_.erase(bits_.begin(), bits_.begin() + 600); bitsBase_ += 600; }
    trySubframe();
}

void GnssTracker::trySubframe() {
    const int64_t total = bitsBase_ + (int64_t)bits_.size();
    int64_t start;
    if (frameSync_) {
        if (total - lastSubframeBit_ != 300) return;
        start = lastSubframeBit_;
    } else {
        if (total < 300) return;
        start = total - 300;
        if (start < bitsBase_) return;
        // the preamble first (either polarity): cheap
        const uint8_t* b = &bits_[(size_t)(start - bitsBase_)];
        unsigned v = 0;
        for (int i = 0; i < 8; i++) v = v << 1 | b[i];
        if (v != kLnavPreamble && v != (~kLnavPreamble & 0xFF)) return;
    }
    if (start < bitsBase_) { frameSync_ = false; return; }
    const uint8_t* b = &bits_[(size_t)(start - bitsBase_)];
    GnssSubframeEvent ev;
    const bool ok = lnavDecodeSubframeAuto(b, ev.sf) && lnavSubframeId(ev.sf) >= 1 && lnavSubframeId(ev.sf) <= 5 && lnavTowCount(ev.sf) < 100800;
    if (!ok) {
        if (frameSync_) {
            framesBad_++;
            lastSubframeBit_ += 300;
            if (++badInRow_ >= 4) frameSync_ = false;
        }
        return;
    }
    const int64_t startEpoch = firstBitEpoch_ + 20 * start;
    const double towStart = (double)((lnavTowCount(ev.sf) + 100799) % 100800) * 6.0;
    const double t0 = towStart - (double)startEpoch * 1e-3;
    if (timeValid_ && std::fabs(gpsWrap(t0 - towAtEpoch0_)) > 2e-4) {
        // the clock count disagrees with an earlier subframe: this synchronisation is wrong
        framesBad_++;
        frameSync_ = false; timeValid_ = false; badInRow_ = 0;
        return;
    }
    towAtEpoch0_ = t0;
    timeValid_ = true;
    framesOk_++;
    badInRow_ = 0;
    frameSync_ = true;
    lastSubframeBit_ = total;
    framesSinceSync_++;
    ev.towStart = towStart;
    ev.startEpoch = startEpoch;
    events_.push_back(ev);
}

bool GnssTracker::transmitTime(double tRx, double* tow) const {
    if (!timeValid_ || epochT_.size() < 3) return false;
    // find the epoch whose start is the last one not after tRx
    const size_t n = epochT_.size();
    if (tRx < epochT_.front() || tRx >= epochT_.back()) return false;
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) { const size_t mid = (lo + hi) / 2; if (epochT_[mid] <= tRx) lo = mid; else hi = mid; }
    const double t0 = epochT_[lo], t1 = epochT_[lo + 1];
    const uint64_t k = epochTFirst_ + lo;
    *tow = towAtEpoch0_ + (double)k * 1e-3 + (tRx - t0) / (t1 - t0) * 1e-3;
    return true;
}

} // namespace dect2
