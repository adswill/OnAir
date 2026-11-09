// One tracking channel (see gnss_track.h).
#include "dect2/gnss/gnss_track.h"
#include "dect2/gnss/gnss_bds.h"
#include "dect2/gnss/gnss_glo.h"
#include "dect2/gnss/gnss_msg.h"
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
    flSum_ = flDiff_ = 0; falseLocks_ = 0;
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
    sym_.clear(); symBase_ = symCount_ = lastWindowAt_ = 0; pairOffset_ = -1; lastMsgSym_ = -1; symAcc_ = 0; symHalf_ = 0;
    pairAmp_[0] = pairAmp_[1] = 0; havePrevI_ = false; prevIsym_ = 0;
    gsym_.clear(); gsymBase_ = 0; gloNextTm_ = -1; gloTmMiss_ = 0; gloPend_ = false;
    std::memset(nhRing_, 0, sizeof nhRing_); std::memset(nhAcc_, 0, sizeof nhAcc_); std::memset(nhAlt_, 0, sizeof nhAlt_); nhFill_ = 0; bdsInv_ = 0;
    isym_.clear(); isymBase_ = 0; nextPart_ = -1; inavPol_ = 1; syncMiss_ = 0; evenAt_ = -1; inavNoSync_ = 0; inavTry_ = 0; inavBase_ = 0;
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
    if (pos_ < band.base()) { lost_ = true; lostWhy_ = "fell behind the sample buffer (processing too slow)"; return false; }   // fell behind the buffer
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
        } else if ((pullEpochs_ > 1500 && cnInit_ >= 50 && cn0Db_ < 24.0) || pullEpochs_ > 6000 || std::fabs(fcar_ - fcar0_) > 3000.0) {
            lost_ = true; pullInTimeout_ = true;
            lostWhy_ = std::fabs(fcar_ - fcar0_) > 3000.0 ? "carrier ran off more than 3 kHz while pulling in" : cn0Db_ < 24.0 ? "too weak to lock (C/N0 under 24 dB)" : "carrier did not settle";
        }
    } else {
        // Costas phase loop, second order, 15 Hz
        const double th = std::atan2(Q, I == 0 ? 1e-12 : I);
        double err = th;
        if (err > M_PI / 2) err -= M_PI; else if (err < -M_PI / 2) err += M_PI;
        const double wn = 15.0 / 0.53, zeta = 0.707;
        pllInt_ += wn * wn * Tn * err;
        fcar_ = pllF0_ + (pllInt_ + 2 * zeta * wn * err) / (2 * M_PI);
        locked_epochs_++;
        // A false lock half the epoch rate away (500 Hz with 1 ms epochs): the carrier turns half a cycle per epoch, which the Costas loop takes for
        // data changes. Where a data symbol lasts two epochs or more (LNAV, SBAS), consecutive prompts then cancel instead of adding up: compare the
        // power of their sum and of their difference over the first 500 epochs of the lock, and move the carrier by half the epoch rate if it is false.
        if (spec.msg != GnssMsgInav && spec.msg != GnssMsgBdsD1 && havePrev_ && locked_epochs_ > 100 && locked_epochs_ <= 600) {
            const double si = I + prevI_, sq = Q + prevQ_, di = I - prevI_, dq = Q - prevQ_;
            flSum_ += si * si + sq * sq; flDiff_ += di * di + dq * dq;
            if (locked_epochs_ == 600) {
                if (flDiff_ > flSum_) {
                    const double half = 0.5 / Tn;
                    pllF0_ = fcar_ + (fcar0_ > fcar_ ? half : -half);
                    fcar_ = pllF0_; pllInt_ = 0; locked_epochs_ = 0; lockIdx_ = 0;
                    falseLocks_++;
                }
                flSum_ = flDiff_ = 0;
            }
        }
        // lock indicator: (I^2 - Q^2) / (I^2 + Q^2), averaged
        const double li = (I * I - Q * Q) / (I * I + Q * Q + 1e-12);
        lockIdx_ += (li - lockIdx_) * 0.01;
        if (locked_epochs_ > 300 && lockIdx_ < 0.1) { lowLockEpochs_++; if (lowLockEpochs_ > 500) { phaseDrop_ = true; locked_ = false; pullEpochs_ = 0; fllErr_ = 1e3; lowLockEpochs_ = 0; bitSync_ = frameSync_ = false; std::memset(hist_, 0, sizeof hist_); bitHistTotal_ = 0; } }
        else lowLockEpochs_ = 0;
        if (cnInit_ >= 50 && locked_epochs_ > 1000) {
            if (cn0Db_ < 17.0) { if (++lowCn0Epochs_ > 3000) { lost_ = true; lostWhy_ = "signal faded (C/N0 under 17 dB for 3 s)"; } } else lowCn0Epochs_ = 0;
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
    if (ecount_ - lastHistEpoch_ >= (uint64_t)std::lround(1.0 / spec.epochS)) {
        lastHistEpoch_ = ecount_;
        cn0Hist_.push_back((float)cn0Db_);
        if (cn0Hist_.size() > 60) cn0Hist_.erase(cn0Hist_.begin());
    }
    // ---- data bits (only with a phase lock: the data sits in I)
    if (locked_ && locked_epochs_ > 100) {
        if (spec.msg == GnssMsgSbas) sbasLayer((float)I);
        else if (spec.msg == GnssMsgInav) inavLayer((float)I);
        else if (spec.msg == GnssMsgBdsD1 || spec.msg == GnssMsgBdsD2) bdsLayer((float)I);
        else if (spec.msg == GnssMsgGlo) gloLayer((float)I);
        else bitLayer((float)I);
    }
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

// SBAS: 500 symbols a second, a symbol is two code periods. The symbol edges are found like the GPS bit edges (sign changes, here by the epoch
// number modulo 2); the symbols are decoded a second at a time over the last 1200 (the bit pairs tried both ways until messages are found).
void GnssTracker::sbasLayer(float I) {
    const int sign = I >= 0 ? 1 : -1;
    if (!bitSync_) {
        // the symbol edges: the sum of two epochs has the most power when both lie in one symbol, so the power of the sums of the pairs that
        // start at even and at odd epochs is compared (a count of sign changes would drown in noise flips at a low C/N0)
        (void)sign;
        if (havePrevI_) { pairAmp_[ecount_ % 2] += (prevIsym_ + I) * (prevIsym_ + I); bitHistTotal_++; }
        prevIsym_ = I; havePrevI_ = true;
        if (bitHistTotal_ >= 1000) {
            const double a = pairAmp_[1], b = pairAmp_[0];       // a pair that ends at an odd epoch began at an even one
            if (std::fabs(a - b) > 0.12 * std::max(a, b)) {
                bitSync_ = true; syncPhase_ = a > b ? 0 : 1; syncEpoch_ = ecount_;
                sym_.clear(); symBase_ = 0; symCount_ = 0; lastWindowAt_ = 0; pairOffset_ = -1; lastMsgSym_ = -1; symAcc_ = 0; symHalf_ = -1;
            }
            pairAmp_[0] = pairAmp_[1] = 0; bitHistTotal_ = 0;
        }
        return;
    }
    // a symbol starts at an epoch with the sync phase (the sign change lands at its start)
    if ((int)(ecount_ % 2) == syncPhase_) {
        if (symHalf_ == 2) {
            sym_.push_back((float)symAcc_);
            symCount_++;
            if (sym_.size() > 4000) { sym_.erase(sym_.begin(), sym_.begin() + 2000); symBase_ += 2000; }
        }
        symAcc_ = 0; symHalf_ = 0;
    }
    if (symHalf_ >= 0) { symAcc_ += I; symHalf_++; }
    if (!frameSync_ && ecount_ - syncEpoch_ > 20000) { bitSync_ = false; pairAmp_[0] = pairAmp_[1] = 0; bitHistTotal_ = 0; havePrevI_ = false; return; }
    const int W = 1200;
    if (symCount_ - lastWindowAt_ < 500 || (int64_t)sym_.size() < W + 2) return;
    lastWindowAt_ = symCount_;
    // normalise the soft values: the decoder only needs their ratios, but keep them near 1
    double m = 0;
    for (size_t k = sym_.size() - W - 1; k < sym_.size(); k++) m += std::fabs(sym_[k]);
    m = m / (W + 1) + 1e-9;
    std::vector<float> win((size_t)W);
    bool any = false;
    for (int o = 0; o < 2 && !any; o++) {
        int off = pairOffset_ >= 0 ? pairOffset_ : o;
        // the window starts at an absolute symbol with that parity
        int64_t a0 = symBase_ + (int64_t)sym_.size() - W - 1;
        if (((a0 % 2) + 2) % 2 != off) a0++;
        for (int k = 0; k < W; k++) win[(size_t)k] = (float)(sym_[(size_t)(a0 - symBase_ + k)] / m);
        auto msgs = sbasFindMessages(win.data(), W / 2);
        for (auto& msg : msgs) {
            const int64_t at = a0 + 2 * msg.bitPos;
            any = true;
            if (at <= lastMsgSym_) continue;
            if (lastMsgSym_ >= 0 && frameSync_) {
                const int64_t missed = (at - lastMsgSym_) / 500 - 1;
                if (missed > 0) framesBad_ += (uint32_t)missed;
            }
            lastMsgSym_ = at;
            framesOk_++;
            frameSync_ = true;
            framesSinceSync_++;
            GnssSubframeEvent ev;
            ev.kind = GnssMsgSbas;
            ev.sbasType = msg.type;
            for (int i = 0; i < 128 && i < kSbasMsgBits; i++) ev.word[i] = msg.bits[i];
            for (int i = 0; i < kSbasMsgBits; i++) ev.bits[i] = msg.bits[i];     // the whole message, for the SBAS content decoder
            ev.startEpoch = (int64_t)syncPhase_ + 2 * at;
            ev.towStart = -1;
            events_.push_back(ev);
        }
        if (any) pairOffset_ = off;
        if (pairOffset_ >= 0) break;
    }
    if (!any && frameSync_ && lastMsgSym_ >= 0 && symBase_ + (int64_t)sym_.size() - lastMsgSym_ > 2000) {
        // nothing for four seconds: the bit pairs or the symbols are wrong
        framesBad_++;
        if (++badInRow_ >= 3) { frameSync_ = false; pairOffset_ = -1; badInRow_ = 0; }
    } else if (any) badInRow_ = 0;
}

// Galileo I/NAV on E1-B: one symbol per code period. A page part is 250 symbols that begin with the sync pattern 0101100000; found once twice
// 250 symbols apart, the parts are decoded one by one (deinterleaving, Viterbi) and an even part and the odd part after it make a page with a CRC.
void GnssTracker::inavLayer(float I) {
    // symbols are numbered by their epoch: a gap (the lock lost for a while) starts the buffer again
    if (isym_.empty() || isymBase_ + (int64_t)isym_.size() != (int64_t)ecount_) { isym_.clear(); isymBase_ = (int64_t)ecount_; nextPart_ = -1; evenAt_ = -1; }
    isym_.push_back(I);
    if (isym_.size() > 1000) { isym_.pop_front(); isymBase_++; }
    const int64_t last = isymBase_ + (int64_t)isym_.size() - 1;        // absolute number (= epoch count) of the newest symbol
    if (nextPart_ < 0 && !frameSync_) {
        // No sync pattern after 4 s of lock: a carrier locked half the epoch rate (125 Hz) or a multiple of it away turns the symbols over every
        // epoch, which the Costas loop cannot see with a symbol in every epoch. Try the neighbouring lock points in turn: +1, -1, +2, -2 times 125 Hz.
        if (++inavNoSync_ > 1000) {
            static const int kTry[4] = {1, -1, 2, -2};
            if (inavTry_ == 0) inavBase_ = fcar_;
            const double half = 0.5 / spec.epochS;
            if (inavTry_ < 4) {
                pllF0_ = inavBase_ + kTry[inavTry_] * half;
                fcar_ = pllF0_; pllInt_ = 0; lockIdx_ = 0;
                inavTry_++;
            } else { pllF0_ = inavBase_; fcar_ = pllF0_; pllInt_ = 0; inavTry_ = 0; }
            inavNoSync_ = 0;
            isym_.clear(); isymBase_ = (int64_t)ecount_ + 1;
            return;
        }
    }
    if (nextPart_ < 0) {
        // two sync patterns 250 symbols apart, in the same polarity
        const int64_t s0 = last - 259;
        if (s0 < isymBase_) return;
        auto match = [&](int64_t at) {
            int pos = 0, neg = 0;
            for (int k = 0; k < 10; k++) {
                const int b = isym_[(size_t)(at - isymBase_ + k)] < 0 ? 1 : 0;
                pos += b == kInavSync[k]; neg += b != kInavSync[k];
            }
            return pos == 10 ? 1 : (neg == 10 ? -1 : 0);
        };
        const int a = match(s0), b = match(s0 + 250);
        if (a == 0 || a != b) return;
        inavPol_ = a; nextPart_ = s0; syncMiss_ = 0; evenAt_ = -1; inavNoSync_ = 0; inavTry_ = 0;
        bitSync_ = true;
    }
    while (nextPart_ >= 0 && last >= nextPart_ + 249) {
        if (nextPart_ < isymBase_) { nextPart_ = -1; bitSync_ = false; break; }
        inavPart(nextPart_);
        if (nextPart_ >= 0) nextPart_ += 250;
    }
}

void GnssTracker::inavPart(int64_t p) {
    const size_t o = (size_t)(p - isymBase_);
    int ok = 0;
    for (int k = 0; k < 10; k++) ok += ((isym_[o + k] * inavPol_ < 0) ? 1 : 0) == kInavSync[k];
    if (ok < 8) {
        if (++syncMiss_ >= 3) { nextPart_ = -1; bitSync_ = false; frameSync_ = false; return; }
    } else syncMiss_ = 0;
    float soft[240];
    double m = 0;
    for (int k = 0; k < 240; k++) { soft[k] = isym_[o + 10 + k] * (float)inavPol_; m += std::fabs(soft[k]); }
    m = m / 240 + 1e-9;
    for (auto& v : soft) v = (float)(v / m);
    uint8_t bits[120];
    inavDecodePart(soft, bits);
    if (bits[0] == 0) { std::memcpy(evenBits_, bits, 120); evenAt_ = p; return; }
    if (evenAt_ != p - 250) return;                  // an odd part without its even part
    GnssSubframeEvent ev;
    ev.kind = GnssMsgInav;
    if (!inavCheckPage(evenBits_, bits, ev.word)) {
        framesBad_++;
        if (++badInRow_ >= 6) { frameSync_ = false; }
        return;
    }
    std::memcpy(ev.sar, bits + 58, 22);             // odd part: 2 + 16 + reserved 40 (OSNMA), then SAR
    badInRow_ = 0;
    framesOk_++;
    frameSync_ = true;
    framesSinceSync_++;
    ev.startEpoch = evenAt_;
    ev.towStart = -1;
    GalNav scratch;
    int tow = -1;
    inavParseWord(ev.word, scratch, &tow);
    if (tow >= 0 && tow < 604800) {
        ev.towStart = tow;
        const double t0 = (double)tow - (double)evenAt_ * spec.epochS;
        if (timeValid_ && std::fabs(gpsWrap(t0 - towAtEpoch0_)) > 2e-4) { timeValid_ = false; }
        else { towAtEpoch0_ = t0; timeValid_ = true; }
    }
    events_.push_back(ev);
}

// BeiDou: D1 bits last 20 code periods with the NH code on them (its phase found by correlating with the code at each of the 20 offsets), D2 bits two
// code periods (found like the SBAS symbols). The bits go into a window searched for the preamble; a subframe of 300 bits is decoded with the BCH code.
void GnssTracker::bdsLayer(float I) {
    const bool d1 = spec.msg == GnssMsgBdsD1;
    const int per = d1 ? 20 : 2;
    if (!bitSync_) {
        bool sync = false;
        if (d1) {
            nhRing_[ecount_ % 20] = I;
            if (++nhFill_ >= 20) {
                // a bit (and so the NH code) that started 19 epochs ago: its start lies at phase (e + 1) mod 20
                // also with every other code period turned over: a carrier locked half the epoch rate (500 Hz) away does that to the symbols,
                // which the Costas loop cannot see (the NH code hides it from the check the other messages use)
                double c = 0, ca = 0;
                for (int k = 0; k < 20; k++) {
                    const double v = nhRing_[(ecount_ + 1 + k) % 20] * (kBdsNh[k] ? -1.0 : 1.0);
                    c += v; ca += (k & 1) ? -v : v;
                }
                nhAcc_[(ecount_ + 1) % 20] += std::fabs(c);
                nhAlt_[(ecount_ + 1) % 20] += std::fabs(ca);
                bitHistTotal_++;
            }
            if (bitHistTotal_ >= 1000) {
                int at = 0, atA = 0;
                for (int q = 1; q < 20; q++) { if (nhAcc_[q] > nhAcc_[at]) at = q; if (nhAlt_[q] > nhAlt_[atA]) atA = q; }
                double second = 0;
                for (int q = 0; q < 20; q++) if (q != at) second = std::max(second, nhAcc_[q]);
                if (nhAlt_[atA] > 1.3 * nhAcc_[at]) {
                    // false lock: move the carrier by half the epoch rate, towards the frequency the channel started with
                    const double half = 0.5 / spec.epochS;
                    pllF0_ = fcar_ + (fcar0_ > fcar_ ? half : -half);
                    fcar_ = pllF0_; pllInt_ = 0; locked_epochs_ = 0; lockIdx_ = 0;
                    falseLocks_++;
                    nhFill_ = 0;
                } else if (nhAcc_[at] > 1.3 * second) { sync = true; syncPhase_ = at; }
                std::memset(nhAcc_, 0, sizeof nhAcc_); std::memset(nhAlt_, 0, sizeof nhAlt_); bitHistTotal_ = 0;
            }
        } else {
            if (havePrevI_) { pairAmp_[ecount_ % 2] += (prevIsym_ + I) * (prevIsym_ + I); bitHistTotal_++; }
            prevIsym_ = I; havePrevI_ = true;
            if (bitHistTotal_ >= 1000) {
                const double a = pairAmp_[1], b = pairAmp_[0];
                if (std::fabs(a - b) > 0.12 * std::max(a, b)) { sync = true; syncPhase_ = a > b ? 0 : 1; }
                pairAmp_[0] = pairAmp_[1] = 0; bitHistTotal_ = 0;
            }
        }
        if (sync) {
            bitSync_ = true; bitAcc_ = 0; bitCount_ = 0; firstBitEpoch_ = -1; bitsPushed_ = 0;
            bits_.clear(); bitsBase_ = 0; frameSync_ = false; syncEpoch_ = ecount_;
        }
        return;
    }
    if ((int)(ecount_ % per) == syncPhase_) {
        if (firstBitEpoch_ < 0) firstBitEpoch_ = (int64_t)ecount_;
        else if (bitCount_ == per) {
            bits_.push_back(bitAcc_ > 0 ? 0 : 1);
            bitsPushed_++;
            if (bits_.size() > 1200) { bits_.erase(bits_.begin(), bits_.begin() + 600); bitsBase_ += 600; }
            bdsTrySubframe();
        }
        bitAcc_ = 0; bitCount_ = 0;
    }
    if (firstBitEpoch_ >= 0) {
        const int k = (int)((ecount_ + 20 - (uint64_t)syncPhase_) % 20);
        bitAcc_ += d1 && kBdsNh[k] ? -I : I;
        bitCount_++;
    }
    // a bit synchronisation that does not lead to a subframe is wrong: start again
    if (!frameSync_ && ecount_ - syncEpoch_ > 40000) { bitSync_ = false; nhFill_ = 0; bitHistTotal_ = 0; havePrevI_ = false; pairAmp_[0] = pairAmp_[1] = 0; }
}

void GnssTracker::bdsTrySubframe() {
    const bool d1 = spec.msg == GnssMsgBdsD1;
    const int per = d1 ? 20 : 2;
    const int64_t total = bitsBase_ + (int64_t)bits_.size();
    int64_t start;
    if (frameSync_) {
        if (total - lastSubframeBit_ != 300) return;
        start = lastSubframeBit_;
    } else {
        if (total < 300) return;
        start = total - 300;
    }
    if (start < bitsBase_) { frameSync_ = false; return; }
    const uint8_t* b = &bits_[(size_t)(start - bitsBase_)];
    unsigned v = 0;
    for (int i = 0; i < 11; i++) v = v << 1 | b[i];
    int inv = -1;
    if (v == kBdsPreamble) inv = 0;
    else if (v == (~kBdsPreamble & 0x7FF)) inv = 1;
    GnssSubframeEvent ev;
    ev.kind = spec.msg;
    bool ok = inv >= 0;
    if (ok) {
        uint8_t rx[300];
        for (int i = 0; i < 300; i++) rx[i] = (uint8_t)(b[i] ^ inv);
        const int fixed = bdsDecodeSubframe(rx, ev.bits);
        const unsigned id = bdsFraId(ev.bits), sow = bdsSow(ev.bits);
        ok = fixed <= 6 && id >= 1 && id <= 5 && sow < 604800 && (d1 ? sow % 6 == 0 && (sow / 6) % 5 == id - 1 : sow % 3 == 0);
    }
    if (!ok) {
        if (frameSync_) {
            framesBad_++;
            lastSubframeBit_ += 300;
            if (++badInRow_ >= 4) frameSync_ = false;
        }
        return;
    }
    const unsigned id = bdsFraId(ev.bits);
    const double towStart = (double)bdsSow(ev.bits) + (d1 ? 0.0 : 0.6 * (id - 1));
    const int64_t startEpoch = firstBitEpoch_ + per * start;
    const double t0 = towStart - (double)startEpoch * 1e-3;
    if (timeValid_ && std::fabs(gpsWrap(t0 - towAtEpoch0_)) > 2e-4) {
        framesBad_++;
        frameSync_ = false; timeValid_ = false; badInRow_ = 0;
        return;
    }
    towAtEpoch0_ = t0;
    timeValid_ = true;
    framesOk_++;
    badInRow_ = 0;
    frameSync_ = true;
    lastSubframeBit_ = start + 300;
    framesSinceSync_++;
    ev.towStart = towStart;
    ev.startEpoch = startEpoch;
    events_.push_back(ev);
}

// GLONASS: 10 ms symbols (the 100 Hz meander turns every data bit over half way, so the symbol edges are found like the GPS bit edges, by the
// sign changes counted modulo 10 epochs). A string is 170 data symbols and the 30 chip time mark: found once by the time mark, then every 200
// symbols. Each data bit is two symbols of opposite sign; the bits are in relative code, so neither the meander's nor the carrier's sign matters.
void GnssTracker::gloLayer(float I) {
    const int sign = I >= 0 ? 1 : -1;
    if (!bitSync_) {
        if (prevSign_ != 0 && sign != prevSign_) { hist_[ecount_ % 10]++; bitHistTotal_++; }
        prevSign_ = sign;
        if (bitHistTotal_ >= 40) {
            int best = 0, second = 0, at = 0;
            for (int i = 0; i < 10; i++) if (hist_[i] > best) { best = hist_[i]; at = i; }
            for (int i = 0; i < 10; i++) if (i != at && hist_[i] > second) second = hist_[i];
            if (best >= 24 && best >= 3 * second) {
                bitSync_ = true; syncPhase_ = at; bitAcc_ = 0; bitCount_ = 0; firstBitEpoch_ = -1;
                gsym_.clear(); gsymBase_ = 0; gloNextTm_ = -1; gloTmMiss_ = 0; frameSync_ = false; syncEpoch_ = ecount_;
            } else if (bitHistTotal_ > 400) { std::memset(hist_, 0, sizeof hist_); bitHistTotal_ = 0; }
        }
        return;
    }
    if ((int)(ecount_ % 10) == syncPhase_) {
        if (firstBitEpoch_ < 0) firstBitEpoch_ = (int64_t)ecount_;
        else if (bitCount_ == 10) {
            gsym_.push_back((float)bitAcc_);
            if (gsym_.size() > 1000) { gsym_.pop_front(); gsymBase_++; }
            gloTry();
        }
        bitAcc_ = 0; bitCount_ = 0;
    }
    if (firstBitEpoch_ >= 0) { bitAcc_ += I; bitCount_++; }
    if (!frameSync_ && ecount_ - syncEpoch_ > 60000) { bitSync_ = false; std::memset(hist_, 0, sizeof hist_); bitHistTotal_ = 0; prevSign_ = 0; }
}

void GnssTracker::gloTry() {
    const int64_t last = gsymBase_ + (int64_t)gsym_.size() - 1;
    auto tmMatch = [&](int64_t end) {
        if (end - 29 < gsymBase_) return 0;
        int pos = 0;
        for (int k = 0; k < 30; k++) pos += ((gsym_[(size_t)(end - 29 + k - gsymBase_)] < 0) ? 1 : 0) == (kGloTimeMark[k] - '0');
        return pos;                    // agreements in one polarity (30 - pos in the other)
    };
    if (gloNextTm_ < 0) {
        const int p = tmMatch(last);
        if (p != 30 && p != 0) return;
        gloNextTm_ = last;
        gloTmMiss_ = 0;
    }
    if (last != gloNextTm_) return;
    const int p = tmMatch(last);
    if (p < 26 && p > 4) {
        if (++gloTmMiss_ >= 3) { gloNextTm_ = -1; frameSync_ = false; return; }
    } else gloTmMiss_ = 0;
    if (last - 199 >= gsymBase_) gloString(last - 199);
    gloNextTm_ += 200;
}

void GnssTracker::gloString(int64_t start) {
    uint8_t r[85];
    for (int j = 0; j < 85; j++) {
        const float a = gsym_[(size_t)(start + 2 * j - gsymBase_)], b = gsym_[(size_t)(start + 2 * j + 1 - gsymBase_)];
        r[j] = (a - b) < 0 ? 1 : 0;
    }
    GnssSubframeEvent ev;
    ev.kind = GnssMsgGlo;
    uint8_t* s = ev.bits;
    s[85] = 0;
    for (int j = 1; j < 85; j++) s[85 - j] = (uint8_t)(r[j] ^ r[j - 1]);
    const int chk = gloCheckString(s);
    const int m = (int)gloField(s, 84, 4);
    const int64_t startEpoch = firstBitEpoch_ + 10 * start;
    if (chk < 0 || m < 1 || m > 15) {
        if (frameSync_) { framesBad_++; if (++badInRow_ >= 6) frameSync_ = false; }
        return;
    }
    // the time: string 1 says when its frame began (tk, the satellite's time of day); the others must fall where that puts them
    // (a time from string 1 is used once the next string falls where it says: a string that passed the check by chance cannot set it)
    auto expectedM = [&](double t0) {
        const double tod = t0 + (double)startEpoch * 1e-3;
        return (int)std::lround(std::fmod(std::fmod(tod, 30.0) + 30.0, 30.0) / 2.0) % 15 + 1;
    };
    if (m == 1) {
        const double tk = gloField(s, 76, 5) * 3600.0 + gloField(s, 71, 6) * 60.0 + gloField(s, 65, 1) * 30.0;
        const double t0 = tk - (double)startEpoch * 1e-3;
        if (timeValid_ && std::fabs(gloWrapDay(t0 - towAtEpoch0_)) > 2e-4) { framesBad_++; timeValid_ = false; gloPendT0_ = t0; gloPend_ = true; return; }
        if (!timeValid_) { gloPendT0_ = t0; gloPend_ = true; }
    } else if (timeValid_) {
        if (expectedM(towAtEpoch0_) != m) { framesBad_++; timeValid_ = false; return; }
    } else if (gloPend_) {
        if (expectedM(gloPendT0_) == m) { towAtEpoch0_ = gloPendT0_; timeValid_ = true; }
        gloPend_ = false;
    }
    framesOk_++;
    badInRow_ = 0;
    frameSync_ = true;
    framesSinceSync_++;
    ev.startEpoch = startEpoch;
    const bool known = timeValid_ || (m == 1 && gloPend_);
    const double base = timeValid_ ? towAtEpoch0_ : gloPendT0_;
    ev.towStart = known ? std::fmod(base + (double)startEpoch * 1e-3 + 86400.0, 86400.0) : -1;
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
    *tow = towAtEpoch0_ + ((double)k + (tRx - t0) / (t1 - t0)) * spec.epochS;
    return true;
}

} // namespace dect2
