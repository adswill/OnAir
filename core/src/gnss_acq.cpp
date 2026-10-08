// Acquisition by FFT (see gnss_acq.h).
#include "dect2/gnss_acq.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
// the false alarm threshold of the peak over the mean of the grid: the sum of K/2 chi-square(2) terms is a Gamma(K/2) variable; find x with
// P(Gamma(m) > m x) = p (an upper tail; the Poisson sum)
double gammaThreshold(int m, double p) {
    double lo = 1.0, hi = 50.0;
    for (int it = 0; it < 80; it++) {
        const double x = 0.5 * (lo + hi) * m;      // the value of the sum in units of the mean of one term
        // P(Gamma(m,1) > x) = exp(-x) * sum_{k<m} x^k / k!
        double term = 1.0, s = 1.0;
        for (int k = 1; k < m; k++) { term *= x / k; s += term; }
        const double tail = std::exp(-x) * s;
        if (tail > p) lo = 0.5 * (lo + hi); else hi = 0.5 * (lo + hi);
    }
    return 0.5 * (lo + hi);
}

// Add the power of one 2 ms pair to the sum, moved by `shift` samples (circular): the code drift against the samples since the start of the segment
void addShifted(float* P, const float* aRe, const float* aIm, const float* bRe, const float* bIm, int N, int shift) {
    shift = ((shift % N) + N) % N;
    // P[n] += |a[n - shift] + b[n - shift]|^2
    for (int n = 0; n < N; n++) {
        int m = n - shift; if (m < 0) m += N;
        const float x = aRe[m] + bRe[m], y = aIm[m] + bIm[m];
        P[n] += x * x + y * y;
    }
}
}

void GnssAcq::init(const GnssAcqConfig& cfg) {
    cfg_ = cfg;
    N_ = 1 << cfg.fftLog2;
    wantLo_ = cfg.qMin; wantHi_ = cfg.qMax;
    wantBlocks_ = std::max(2, std::min(kMaxBlocks, cfg.blocks & ~1));
    qLo_ = wantLo_; qHi_ = wantHi_; blocks_ = wantBlocks_;
    nBins_ = (qHi_ - qLo_ + 1) * 4;
    thrBins_ = thrPairs_ = -1;
    startPrn();
    codeRe_.assign(cfg.prns.size(), {});
    codeIm_.assign(cfg.prns.size(), {});
    codeValid_.assign(cfg.prns.size(), 0);
    reset();
}

void GnssAcq::setPlan(double centerHz, double halfWidthHz, int blocks) {
    const double h = std::max(1000.0, halfWidthHz);
    // whole kilohertz: the shift of the spectrum is in steps of one bin (1 kHz), the four offsets fill in between
    wantLo_ = (int)std::floor((centerHz - h) / 1000.0);
    wantHi_ = (int)std::ceil((centerHz + h) / 1000.0) - 1;
    const int lim = N_ / 2 - 2;
    wantLo_ = std::max(-lim, wantLo_); wantHi_ = std::min(lim, std::max(wantLo_, wantHi_));
    wantBlocks_ = std::max(2, std::min(kMaxBlocks, blocks & ~1));
}

// The window for the next satellite, and the threshold for its size and the integration of the segment
void GnssAcq::startPrn() {
    qLo_ = wantLo_; qHi_ = wantHi_;
    nBins_ = (qHi_ - qLo_ + 1) * 4;
    const int pairs = blocks_ / 2;
    if (nBins_ != thrBins_ || pairs != thrPairs_) {
        // cells searched: bins x code phases at chip resolution, and the noise mean estimated from the grid itself
        const double cells = (double)nBins_ * (double)N_ / 4.0;
        threshold_ = (float)gammaThreshold(pairs, cfg_.pfa / cells);
        thrBins_ = nBins_; thrPairs_ = pairs;
    }
    curBin_ = 0; sumP_ = 0; cnt_ = 0;
    best_ = 0; bestBin_ = 0; bestN_ = 0; leftVal_ = rightVal_ = 0;
    hasLeft_ = hasRight_ = needRight_ = false;
}

void GnssAcq::reset() {
    state_ = 0;
    nextCapture_ = 0;
    queue_.clear();
    qPos_ = 0;
    curBin_ = 0;
    curPrn_ = 0;
    rounds_ = 0;
    lastCorr_.clear();
    lastPrn_ = 0; lastPeakIdx_ = -1; lastRatio_ = 0; lastDoppler_ = 0;
    havePrev_ = false;
    idleGap_ = 0; searched_ = 0;
}

void GnssAcq::buildQueue(const std::function<bool(int)>& skip) {
    queue_.clear();
    for (int p : priority_) if (std::find(cfg_.prns.begin(), cfg_.prns.end(), p) != cfg_.prns.end() && !skip(p)) queue_.push_back(p);
    for (int p : cfg_.prns) if (!skip(p) && std::find(queue_.begin(), queue_.end(), p) == queue_.end()) queue_.push_back(p);
    qPos_ = 0;
}

void GnssAcq::captureSegment(const GnssBand& band) {
    blocks_ = wantBlocks_;
    const int blocks = blocks_;
    segStart_ = band.end() - (int64_t)blocks * N_;
    nextCapture_ = band.end();
    data_.resize((size_t)4 * blocks);
    const float w0 = -2.f * (float)M_PI / (float)cfg_.fsOut;
    std::vector<float> re(N_), im(N_);
    for (int d = 0; d < 4; d++) {
        const double delta = 250.0 * d;
        for (int k = 0; k < blocks; k++) {
            const cf32* x = band.at(segStart_ + (int64_t)k * N_);
            // multiplication by exp(-j 2 pi delta t), phase continuous over the whole segment (needed to add blocks coherently)
            const double ph0 = -2 * M_PI * delta * (double)((int64_t)k * N_) / cfg_.fsOut;
            const double dph = w0 * delta;
            double c = std::cos(ph0), s = std::sin(ph0);
            const double cd = std::cos(dph), sd = std::sin(dph);
            float cr = (float)c, ci = (float)s;
            const float cdr = (float)cd, cdi = (float)sd;
            for (int n = 0; n < N_; n++) {
                const float xr = x[n].real(), xi = x[n].imag();
                re[n] = xr * cr - xi * ci;
                im[n] = xr * ci + xi * cr;
                const float nr = cr * cdr - ci * cdi, ni = cr * cdi + ci * cdr;
                cr = nr; ci = ni;
                if ((n & 1023) == 1023) { const float m = 1.f / std::sqrt(cr * cr + ci * ci); cr *= m; ci *= m; }
            }
            fftSplit(re.data(), im.data(), cfg_.fftLog2, false);
            Spec& sp = data_[(size_t)d * blocks + k];
            sp.re = re; sp.im = im;
        }
    }
    state_ = 1;
    buildQueue([](int) { return false; });
}

bool GnssAcq::codeFft(size_t pi) {
    if (codeValid_[pi]) return codeValid_[pi] == 1;
    std::vector<cf32> rep(N_);
    if (!cfg_.replica(cfg_.prns[pi], rep.data(), N_)) { codeValid_[pi] = 2; return false; }
    std::vector<float>& cr = codeRe_[pi]; std::vector<float>& ci = codeIm_[pi];
    cr.resize(N_); ci.resize(N_);
    for (int n = 0; n < N_; n++) { cr[n] = rep[n].real(); ci[n] = rep[n].imag(); }
    fftSplit(cr.data(), ci.data(), cfg_.fftLog2, false);
    codeValid_[pi] = 1;
    return true;
}

int GnssAcq::work(const GnssBand& band, int units, const std::function<bool(int)>& skip, std::vector<GnssAcqHit>& hits) {
    int used = 0;
    if (tmpRe_.size() != (size_t)N_) { tmpRe_.assign(N_, 0.f); tmpIm_.assign(N_, 0.f); prevRe_.assign(N_, 0.f); prevIm_.assign(N_, 0.f); }
    if (curP_.size() != (size_t)N_) { curP_.assign(N_, 0.f); prevP_.assign(N_, 0.f); bestP_.assign(N_, 0.f); }
    while (used < units) {
        if (state_ == 0) {
            // a segment of fresh samples: not overlapping the last one
            const int blocks = wantBlocks_;
            if (band.end() - nextCapture_ < (int64_t)blocks * N_ + idleGap_ && nextCapture_ != 0) return used;
            if (band.end() - band.base() < (int64_t)blocks * N_) return used;
            if (units - used < 4 * blocks) return used;
            captureSegment(band);
            used += 4 * blocks;
            buildQueue(skip);
            startPrn();
            continue;
        }
        if (qPos_ >= queue_.size()) {
            // a whole round done on this segment: take a new one (after a pause when there was nothing to search)
            state_ = 0;
            rounds_++;
            idleGap_ = searched_ == 0 ? (int64_t)(2.0 * cfg_.fsOut) : 0;
            searched_ = 0;
            return used;          // the caller may change the plan before the next segment
        }
        const int blocks = blocks_;
        const int prn = queue_[qPos_];
        if (skip(prn)) { qPos_++; startPrn(); continue; }
        const size_t pi = (size_t)(std::find(cfg_.prns.begin(), cfg_.prns.end(), prn) - cfg_.prns.begin());
        curPrn_ = prn; curIdx_ = (int)pi;
        if (curBin_ == 0) searched_++;
        if (!codeValid_[pi]) { codeFft(pi); used += 1; }
        if (codeValid_[pi] == 2) { qPos_++; startPrn(); continue; }
        if (units - used < blocks && used > 0) return used;
        // one step: offset d and Doppler bin q of this satellite
        const int d = curBin_ % 4, qi = curBin_ / 4, q = qLo_ + qi;
        const double f = q * 1000.0 + 250.0 * d;
        // the code start moves back this many samples a millisecond (the code runs faster with the frequency)
        const double drift = (double)N_ * f / cfg_.rfHz;
        float* P = curP_.data();
        std::memset(P, 0, (size_t)N_ * sizeof(float));
        const float* cr = codeRe_[pi].data();
        const float* ci = codeIm_[pi].data();
        const int sh = ((q % N_) + N_) % N_;
        for (int k = 0; k < blocks; k++) {
            const Spec& sp = data_[(size_t)d * blocks + k];
            // Y[f] = X[f + q] * conj(C[f])
            for (int g0 = 0; g0 < N_; g0++) {
                int g = g0 + sh; if (g >= N_) g -= N_;
                tmpRe_[g0] = sp.re[g] * cr[g0] + sp.im[g] * ci[g0];
                tmpIm_[g0] = sp.im[g] * cr[g0] - sp.re[g] * ci[g0];
            }
            fftSplit(tmpRe_.data(), tmpIm_.data(), cfg_.fftLog2, true);
            used++;
            if ((k & 1) == 0) { std::memcpy(prevRe_.data(), tmpRe_.data(), N_ * sizeof(float)); std::memcpy(prevIm_.data(), tmpIm_.data(), N_ * sizeof(float)); }
            else addShifted(P, prevRe_.data(), prevIm_.data(), tmpRe_.data(), tmpIm_.data(), N_, (int)std::lround((k - 0.5) * drift));
        }
        float bv = 0; int bn = 0;
        double sum = 0;
        for (int n = 0; n < N_; n++) { sum += P[n]; if (P[n] > bv) { bv = P[n]; bn = n; } }
        sumP_ += sum;
        cnt_ += N_;
        if (needRight_) { rightVal_ = P[bestN_]; hasRight_ = true; needRight_ = false; }
        if (bv > best_) {
            best_ = bv; bestBin_ = curBin_; bestN_ = bn;
            hasLeft_ = curBin_ > 0; leftVal_ = hasLeft_ ? prevP_[bn] : 0.f;
            hasRight_ = false; needRight_ = true;
            std::memcpy(bestP_.data(), P, (size_t)N_ * sizeof(float));
        }
        std::swap(curP_, prevP_);
        curBin_++;
        if (curBin_ >= nBins_) { finishPrn(hits); qPos_++; startPrn(); }
    }
    return used;
}

bool GnssAcq::confirm(const GnssBand& band, int prn, double fd, int hitBlocks, GnssAcqHit* out) {
    // as long as the search that found it (a weak signal needs it), at least 8 ms
    const int K2 = std::max(8, std::min(kMaxBlocks, hitBlocks & ~1));
    if (band.end() - band.base() < (int64_t)K2 * N_) return false;
    const size_t pi = (size_t)(std::find(cfg_.prns.begin(), cfg_.prns.end(), prn) - cfg_.prns.begin());
    if (pi >= cfg_.prns.size()) return false;
    if (!codeFft(pi)) return false;
    const int64_t seg = band.end() - (int64_t)K2 * N_;
    const double dfs[3] = {-100.0, 0.0, 100.0};
    const double drift = (double)N_ * fd / cfg_.rfHz;
    std::vector<std::vector<float>> P(3, std::vector<float>((size_t)N_, 0.f));
    std::vector<float> re(N_), im(N_), pRe(N_), pIm(N_);
    for (int d = 0; d < 3; d++) {
        const double f = fd + dfs[d];
        for (int k = 0; k < K2; k++) {
            const cf32* x = band.at(seg + (int64_t)k * N_);
            const double ph0 = -2 * M_PI * f * (double)((int64_t)k * N_) / cfg_.fsOut;
            const double dph = -2 * M_PI * f / cfg_.fsOut;
            float cr = (float)std::cos(ph0), ci = (float)std::sin(ph0);
            const float cdr = (float)std::cos(dph), cdi = (float)std::sin(dph);
            for (int n = 0; n < N_; n++) {
                re[n] = x[n].real() * cr - x[n].imag() * ci;
                im[n] = x[n].real() * ci + x[n].imag() * cr;
                const float nr = cr * cdr - ci * cdi, ni = cr * cdi + ci * cdr;
                cr = nr; ci = ni;
                if ((n & 1023) == 1023) { const float m = 1.f / std::sqrt(cr * cr + ci * ci); cr *= m; ci *= m; }
            }
            fftSplit(re.data(), im.data(), cfg_.fftLog2, false);
            const float* cR = codeRe_[pi].data();
            const float* cI = codeIm_[pi].data();
            for (int n = 0; n < N_; n++) {
                const float a = re[n] * cR[n] + im[n] * cI[n], b = im[n] * cR[n] - re[n] * cI[n];
                re[n] = a; im[n] = b;
            }
            fftSplit(re.data(), im.data(), cfg_.fftLog2, true);
            if ((k & 1) == 0) { pRe = re; pIm = im; }
            else addShifted(P[d].data(), pRe.data(), pIm.data(), re.data(), im.data(), N_, (int)std::lround((k - 0.5) * drift));
        }
    }
    float best = 0; int bd = 0, bn = 0;
    double sum = 0;
    for (int d = 0; d < 3; d++) for (int n = 0; n < N_; n++) { sum += P[d][n]; if (P[d][n] > best) { best = P[d][n]; bd = d; bn = n; } }
    const double mean = sum / (3.0 * N_);
    const float ratio = (float)(best / std::max(mean, 1e-30));
    const double thr = gammaThreshold(K2 / 2, std::max(1e-3, cfg_.pfa) / (3.0 * N_ / 4.0));
    if (ratio < thr) return false;
    GnssAcqHit h;
    h.prn = prn; h.ratio = ratio; h.segStart = seg; h.blocks = K2;
    const std::vector<float>& Pb = P[bd];
    const float a = Pb[(bn + N_ - 1) % N_], b = Pb[bn], c = Pb[(bn + 1) % N_];
    const float den = a - 2 * b + c;
    h.codePhase = bn + (std::fabs(den) > 1e-20f ? 0.5f * (a - c) / den : 0.f);
    double fdd = fd + dfs[bd];
    if (bd == 1) {
        const float fa = P[0][bn], fc = P[2][bn];
        const float dd = fa - 2 * best + fc;
        if (std::fabs(dd) > 1e-20f) fdd += 100.0 * std::max(-0.5f, std::min(0.5f, 0.5f * (fa - fc) / dd));
    }
    h.dopplerHz = fdd;
    h.cn0Est = 0;
    *out = h;
    return true;
}

void GnssAcq::finishPrn(std::vector<GnssAcqHit>& hits) {
    const float best = best_;
    const int bBin = bestBin_, bN = bestN_;
    const double mean = sumP_ / std::max(cnt_, 1.0);
    const float ratio = (float)(best / std::max(mean, 1e-30));
    lastPrn_ = curPrn_; lastRatio_ = ratio;
    // the plot: the best Doppler bin, decimated to 256 points by the maximum
    lastCorr_.assign(256, 0.f);
    const float* P = bestP_.data();
    const int per = N_ / 256;
    for (int i = 0; i < 256; i++) { float m = 0; for (int k = 0; k < per; k++) m = std::max(m, P[i * per + k]); lastCorr_[i] = m / std::max(best, 1e-30f); }
    lastPeakIdx_ = bN / per;
    const double f0 = (qLo_ * 1000.0) + 250.0 * bBin;
    lastDoppler_ = f0;
    if (ratio < threshold_) return;
    GnssAcqHit h;
    h.prn = curPrn_;
    h.ratio = ratio;
    h.blocks = blocks_;
    // parabolic interpolation of the code phase (circular) and of the Doppler (neighbouring bins at the same code phase)
    {
        const float a = P[(bN + N_ - 1) % N_], b = P[bN], c = P[(bN + 1) % N_];
        const float den = a - 2 * b + c;
        h.codePhase = bN + (std::fabs(den) > 1e-20f ? 0.5f * (a - c) / den : 0.f);
        double fd = f0;
        if (hasLeft_ && hasRight_) {
            const float fa = leftVal_, fb = best, fc = rightVal_;
            const float dd = fa - 2 * fb + fc;
            if (std::fabs(dd) > 1e-20f) fd += 250.0 * std::max(-0.5f, std::min(0.5f, 0.5f * (fa - fc) / dd));
        }
        h.dopplerHz = fd;
        lastDoppler_ = fd;
    }
    h.segStart = segStart_;
    // C/N0 from the peak: the correlation sum over `blocks` ms; peak/mean - 1 is about (C/N0 * T_total) divided by a loss of ~2 dB
    const double snrTot = std::max(0.0, (double)ratio - 1.0) / (double)(blocks_ / 2);
    const double tc = 0.002;
    h.cn0Est = (float)(10 * std::log10(std::max(snrTot / (2.0 * tc), 1.0)));
    hits.push_back(h);
}

} // namespace dect2
