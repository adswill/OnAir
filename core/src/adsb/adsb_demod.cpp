// ADS-B pulse demodulator (see adsb_demod.h).
#include "dect2/adsb/adsb_demod.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

static const size_t kPiece = 4096;               // samples handled at a time: bounds the buffers
static const int kFrameBins = (8 + 112) * 4;     // preamble and a long message, in bins
static const int kLookahead = kFrameBins + 12;   // a candidate is examined when this many bins follow it
static const int kBlockBins = 256;
static const float kQuartileBias = 0.93f;        // the lower quartile of the block means is this much below the mean (measured on noise, see test_adsb_rx)

// A sample that is not a number, or absurdly large, becomes zero: one bad value from a radio must not poison the running sums for good.
static inline float clean(float v) { return (v > -64.f && v < 64.f) ? v : 0.f; }

// Integral of the power from the start of the buffer to position x (in samples; sample j is at position j), the power being the straight line
// between neighbouring samples. A pulse that falls between two samples then keeps its place: the hold model that treated every sample as a flat
// stretch could not tell a pulse on a sample from one halfway to the next, which at 2 Msps is the whole width of a pulse.
static inline double integral(const double* cum, const float* pw, double x) {
    const size_t j = (size_t)x;
    const double f = x - (double)j;
    return cum[j] + f * pw[j] + 0.5 * f * f * ((double)pw[j + 1] - (double)pw[j]);
}

void AdsbDemod::configure(double rate) {
    const double keepSec = rate_ > 0 ? (double)samples_ / rate_ : 0.0;   // the clock runs on through a new start (#85): it was set back to 0
    inRate_ = rate;
    up_ = rate < 4.5e6;
    rate_ = up_ ? 2.0 * rate : rate;
    spb_ = rate_ / 4e6;
    spus_ = rate_ / 1e6;
    delaySec_ = up_ ? 6.0 / rate : 0.0;
    if (up_) {
        // half-sample interpolation: windowed sinc (Blackman) over 12 input samples, sum normalised to 1 so that a DC offset passes unchanged
        double sum = 0, c[12];
        for (int k = 0; k < 12; k++) {
            const double u = k - 5.5;
            const double sinc = std::sin(M_PI * u) / (M_PI * u);
            const double w = 0.42 + 0.5 * std::cos(M_PI * u / 6.0) + 0.08 * std::cos(2.0 * M_PI * u / 6.0);
            c[k] = sinc * w;
            sum += c[k];
        }
        for (int k = 0; k < 12; k++) taps_[k] = (float)(c[k] / sum);
        tmp_.assign(2048 + 11, cf32(0, 0));
        z_.assign(4096, cf32(0, 0));
    }
    for (auto& h : hist_) h = cf32(0, 0);
    samples_ = rate_ > 0 ? (uint64_t)std::llround(keepSec * rate_) : 0;
    reset();
    pw_.reserve(kPiece + (size_t)(spus_ * 140) + 64);
    zs_.reserve(kPiece + (size_t)(spus_ * 140) + 64);
    cum_.reserve(kPiece + (size_t)(spus_ * 140) + 65);
    bin_.reserve(4 * 140 + kPiece * 4 / 8 + 64);
    score_.reserve(4 * 140 + kPiece * 4 / 8 + 64);
}

void AdsbDemod::reset() {
    smpBase_ = samples_;
    pw_.clear();
    zs_.clear();
    cum_.assign(1, 0.0);   // an empty buffer still has cum_[0]
    dcInit_ = false; dcRe_ = dcIm_ = 0;
    binBase_ = spb_ > 0 ? (int64_t)std::ceil((double)smpBase_ / spb_) : 0;   // first bin that starts inside the data
    bin_.clear(); score_.clear();
    scoreEnd_ = scanBin_ = skipUntil_ = binBase_;
    cancelStart_ = cancelEnd_ = goodEnd_ = binBase_;
    failed_.clear();
    n0_ = 0; noiseReady_ = false;
    blockSum_ = 0; blockCount_ = 0; blockN_ = 0; blockPos_ = 0;
}

void AdsbDemod::feed(const cf32* x, size_t n) {
    if (!ready()) return;
    while (n) {
        if (up_) {
            const size_t m = std::min<size_t>(n, 2048);
            // the real and the imaginary parts as two real streams through the 12 taps (the correlation kernel of dsp_compat)
            tmpR_.resize(11 + m); tmpI_.resize(11 + m); yR_.resize(m); yI_.resize(m);
            for (int k = 0; k < 11; k++) { tmpR_[(size_t)k] = hist_[k].real(); tmpI_[(size_t)k] = hist_[k].imag(); }
            for (size_t j = 0; j < m; j++) { tmpR_[11 + j] = clean(x[j].real()); tmpI_[11 + j] = clean(x[j].imag()); }
            desamp(tmpR_.data(), 1, taps_, yR_.data(), (int)m, 12);
            desamp(tmpI_.data(), 1, taps_, yI_.data(), (int)m, 12);
            for (size_t j = 0; j < m; j++) {
                z_[2 * j] = cf32(tmpR_[j + 5], tmpI_[j + 5]);
                z_[2 * j + 1] = cf32(yR_[j], yI_[j]);
            }
            for (int k = 0; k < 11; k++) hist_[k] = cf32(tmpR_[m + (size_t)k], tmpI_[m + (size_t)k]);
            pushSamples(z_.data(), 2 * m);
            x += m; n -= m;
        } else {
            const size_t m = std::min(n, kPiece);
            pushSamples(x, m);
            x += m; n -= m;
        }
        makeBins();
        scanCandidates();
        compact();
    }
}

// Power of every sample, with the radio's DC offset taken out. The offset is followed with a time constant of about a millisecond, from blocks
// of 64 samples that hold no signal (a pulse train raises the block mean well above the noise).
void AdsbDemod::pushSamples(const cf32* x, size_t m) {
    const float* xf = reinterpret_cast<const float*>(x);
    const size_t base = pw_.size();
    pw_.resize(base + m);
    zs_.resize(base + m);
    cum_.resize(base + m);
    float* pw = pw_.data() + base;
    cf32* zs = zs_.data() + base;
    const float beta = (float)std::min(0.5, 64.0 / (rate_ * 1e-3));
    for (size_t i = 0; i < m; i += 64) {
        const size_t c = std::min<size_t>(64, m - i);
        const float* b = xf + 2 * i;
        if (!dcInit_) {
            float sr = 0, si = 0;
            for (size_t j = 0; j < c; j++) { sr += clean(b[2 * j]); si += clean(b[2 * j + 1]); }
            dcRe_ = sr / c; dcIm_ = si / c; dcInit_ = true;
        }
        const float dr = dcRe_, di = dcIm_;
        // four lanes of partial sums (a single running sum would make every addition wait for the last, and keep the loop from vectorising)
        float sr4[4] = {0, 0, 0, 0}, si4[4] = {0, 0, 0, 0}, bp4[4] = {0, 0, 0, 0};
        size_t j = 0;
        for (; j + 4 <= c; j += 4) {
            for (size_t l = 0; l < 4; l++) {
                const float xr = clean(b[2 * (j + l)]), xi = clean(b[2 * (j + l) + 1]);
                const float re = xr - dr, im = xi - di;
                const float p = re * re + im * im;
                pw[i + j + l] = p;
                zs[i + j + l] = cf32(re, im);
                bp4[l] += p; sr4[l] += xr; si4[l] += xi;
            }
        }
        for (; j < c; j++) {
            const float xr = clean(b[2 * j]), xi = clean(b[2 * j + 1]);
            const float re = xr - dr, im = xi - di;
            const float p = re * re + im * im;
            pw[i + j] = p;
            zs[i + j] = cf32(re, im);
            bp4[0] += p; sr4[0] += xr; si4[0] += xi;
        }
        float bp = (bp4[0] + bp4[1]) + (bp4[2] + bp4[3]), sr = (sr4[0] + sr4[1]) + (sr4[2] + sr4[3]), si = (si4[0] + si4[1]) + (si4[2] + si4[3]);
        bp /= c;
        if (!noiseReady_ || bp < 4.0f * n0_) {
            const float k = beta * (float)c / 64.0f;
            dcRe_ += k * (sr / c - dcRe_); dcIm_ += k * (si / c - dcIm_);
        }
    }
    // cum_[j] = integral of the power from sample 0 to sample j, with the power interpolated linearly between the samples
    const float* all = pw_.data();
    for (size_t j = base ? base - 1 : 0; j + 1 < base + m; j++) cum_[j + 1] = cum_[j] + 0.5 * ((double)all[j] + (double)all[j + 1]);
    samples_ += m;
}

void AdsbDemod::updateNoise(float m) { updateNoiseRun(&m, 1); }

// The bins are taken a block of kBlockBins at a time: each block gives its mean and the spread of its half-microsecond windows, and the
// lower quartile of the last 16 blocks is the noise level.
void AdsbDemod::updateNoiseRun(const float* m, size_t n) {
    size_t i = 0;
    while (i < n) {
        const size_t take = std::min(n - i, (size_t)(kBlockBins - blockCount_));
        double sum = blockSum_, sumW = blockSumW_, sumW2 = blockSumW2_;
        float prev = prevBin_;
        for (size_t k = 0; k < take; k++) {
            const float v = m[i + k];
            const float w = 0.5f * (v + prev);             // a window of 0.5 us
            prev = v;
            sum += v; sumW += w; sumW2 += (double)w * w;
        }
        blockSum_ = sum; blockSumW_ = sumW; blockSumW2_ = sumW2; prevBin_ = prev;
        blockCount_ += (int)take;
        i += take;
        if (blockCount_ >= kBlockBins) endBlock();
    }
}

void AdsbDemod::endBlock() {
    const double meanW = blockSumW_ / kBlockBins;
    blocks_[blockPos_] = (float)(blockSum_ / kBlockBins);
    blockCv_[blockPos_] = (float)(std::sqrt(std::max(0.0, blockSumW2_ / kBlockBins - meanW * meanW)) / std::max(meanW, 1e-30));
    blockPos_ = (blockPos_ + 1) & 15;
    if (blockN_ < 16) blockN_++;
    blockSum_ = blockSumW_ = blockSumW2_ = 0; blockCount_ = 0;
    if (blockN_ < 4) return;
    int idx[16];
    for (int i = 0; i < blockN_; i++) idx[i] = i;
    const int q = blockN_ / 4;                         // lower quartile of the blocks collected so far
    std::nth_element(idx, idx + q, idx + blockN_, [&](int a, int b) { return blocks_[a] < blocks_[b]; });
    const float est = std::max(blocks_[idx[q]] / kQuartileBias, 1e-9f);
    const float cv = blockCv_[idx[q]];
    n0_ = noiseReady_ ? n0_ + 0.5f * (est - n0_) : est;
    cv_ = noiseReady_ ? cv_ + 0.5f * (cv - cv_) : cv;
    noiseReady_ = true;
}

void AdsbDemod::makeBins() {
    const size_t len = pw_.size();
    const double* cum = cum_.data();
    const float* pw = pw_.data();
    auto F = [&](double x) { return integral(cum, pw, x); };
    int64_t next = binBase_ + (int64_t)bin_.size();
    const double base = (double)smpBase_;
    double x0 = (double)next * spb_ - base;
    double f0 = F(x0);
    if (spb_ >= 1.0 && spb_ <= 16.0 && spb_ == (double)(int)spb_) {
        // a whole number of samples a bin (1 at 4 Msps in, 2 for the interpolated 2 to 4.4 Msps): the bins start on samples, and a bin is the mean of
        // the straight lines between the samples, half weights on the two ends
        const int s = (int)spb_;
        const double x0i = (double)next * spb_ - base;                       // integer: sample index of the first new bin
        const int64_t js = (int64_t)x0i;
        if (js >= 0 && js + s + 2 <= (int64_t)len) {
            const size_t cnt = (size_t)(((int64_t)len - 2 - s - js) / s) + 1;     // bins with js + k s + s < len - 1
            const size_t o = bin_.size();
            bin_.resize(o + cnt);
            float* out = bin_.data() + o;
            const float* p = pw + js;
            const float inv = 1.f / (float)s;
            for (size_t q = 0; q < cnt; q++) {
                const float* a = p + q * (size_t)s;
                float sum = 0.5f * (a[0] + a[s]);
                for (int k = 1; k < s; k++) sum += a[k];
                out[q] = sum * inv;
            }
            updateNoiseRun(out, cnt);
            next += (int64_t)cnt;
        }
        x0 = (double)next * spb_ - base;
        f0 = F(x0);
    }
    for (;;) {
        const double x1 = (double)(next + 1) * spb_ - base;
        if (x1 >= (double)len - 1.0) break;
        const double f1 = F(x1);
        const float m = (float)((f1 - f0) / spb_);
        bin_.push_back(m);
        updateNoise(m);
        f0 = f1;
        next++;
    }
    // preamble score for every bin that has the 8 us of its preamble (32 bins) behind it
    const int64_t binEnd = binBase_ + (int64_t)bin_.size();
    const float* bn = bin_.data();
    const bool legacy = kSum_ <= 0;
    const float thr = legacy ? pulseThreshold() : n0_ * (1.0f + kMinP_ * cv_);
    const float dThr = legacy ? 0.f : 2.0f * kSum_ * 0.58f * cv_ * n0_;   // pulse mean over quiet mean, in the units of `pulse` below (twice the window mean)
    int64_t b = scoreEnd_;
    const int64_t last = binEnd - 32;
    const size_t need = (size_t)std::max<int64_t>(0, last - binBase_);
    if (score_.size() < need) score_.resize(need, 0.f);
    // the cheapest rejection first, for all bins at once (plain arithmetic, so that it vectorises): both of the first two pulses (windows of
    // two bins, 0.5 us, at 0 and 1 us) have to be above the threshold
    const size_t nflag = last > b ? (size_t)(last - b) : 0;
    flag_.resize(nflag);
    if (noiseReady_ && nflag) {
        const float* fp = bn + (b - binBase_);
        const float thr2 = 2.f * thr;
        uint8_t* fl = flag_.data();
        for (size_t q = 0; q < nflag; q++) fl[q] = (uint8_t)((fp[q] + fp[q + 1] > thr2) & (fp[q + 4] + fp[q + 5] > thr2));
    }
    const int64_t b0 = b;
    for (; b < last; b++) {
        const float* p = bn + (b - binBase_);
        score_[(size_t)(b - binBase_)] = noiseReady_ && flag_[(size_t)(b - b0)] ? scoreAt(p, thr, dThr) : 0.f;
    }
    scoreEnd_ = std::max(scoreEnd_, last);
}

float AdsbDemod::scoreAt(const float* p, float thr, float dThr) const {
    // windows of two bins (0.5 us): the four pulses first, the cheapest rejection
    const float h0 = p[0] + p[1], h1 = p[4] + p[5];
    if (!(h0 * 0.5f > thr && h1 * 0.5f > thr)) return 0.f;
    const float h2 = p[14] + p[15], h3 = p[18] + p[19];
    if (!(h2 * 0.5f > thr && h3 * 0.5f > thr)) return 0.f;
    const float pulse = (h0 + h1 + h2 + h3) * 0.25f;                       // sum of two bins = twice the window mean
    const float gap = p[2] + p[3] + p[6] + p[7] + p[8] + p[9] + p[10] + p[11] + p[12] + p[13] + p[16] + p[17];   // 6 windows inside the preamble
    float tail = 0;
    for (int k = 20; k < 32; k++) tail += p[k];                            // 3 us of quiet before the data
    const float q = (gap + tail) * (1.0f / 24.0f) * 2.0f;                  // mean of the 12 quiet windows, in the same units as pulse
    const float qi = gap * (1.0f / 12.0f) * 2.0f;
    return pulse > kGap_ * q && pulse > kGap_ * qi && pulse - q > dThr ? (pulse - q) * 0.5f : 0.f;
}

void AdsbDemod::scanCandidates() {
    const int64_t binEnd = binBase_ + (int64_t)bin_.size();
    while (scanBin_ + 2 < scoreEnd_ && scanBin_ + kLookahead <= binEnd) {
        const int64_t b = scanBin_++;
        if (b < skipUntil_) continue;
        const int64_t i = b - binBase_;
        const float s = score_[(size_t)i];
        if (s <= 0) continue;
        auto at = [&](int64_t k) { return k >= 0 && k < (int64_t)score_.size() ? score_[(size_t)k] : 0.f; };
        // a peak of the score. The looser detector also finds the preamble of a strong frame shifted by 1 us (two of the four pulses still hit),
        // which then slices into garbage: its peak has to be the highest within 1.5 us
        const int span = kSum_ > 0 ? 6 : 2;
        bool peak = true;
        for (int k = 1; k <= span && peak; k++) peak = s >= at(i - k) && s > at(i + k);
        if (peak) {
            candidates_++;
            tryFrame(b);
        }
    }
}

// The sink, except for what is left of a frame that was subtracted: the same address and format at the same time is that frame again
bool AdsbDemod::emit(AdsbRaw& r, bool last) {
    if (!sink_) return false;
    if (overlap_) for (const Cut& c : cuts_) if (std::fabs(r.timeSec - c.t) < 3e-6 && std::memcmp(r.bytes, c.head, 4) == 0) return false;
    return sink_(r, last);
}

bool AdsbDemod::tryFrame(int64_t bin) {
    const size_t len = pw_.size();
    const double* cum = cum_.data();
    const float* pw = pw_.data();
    auto F = [&](double x) { return integral(cum, pw, x); };
    const double hw = 0.5 * spus_;                                   // half a microsecond in samples
    const double xb = (double)bin * spb_ - (double)smpBase_;
    static const double pulseAt[4] = {0.0, 1.0, 3.5, 4.5};
    auto pulses = [&](double x0) {
        double e = 0;
        for (double p : pulseAt) { const double xs = x0 + p * spus_; e += F(xs + hw) - F(xs); }
        return e;
    };
    // timing: the shift (within +-0.5 us of the coarse position) that puts the most energy into the four pulse windows
    double best = -1, x0 = xb;
    for (int k = -10; k <= 10; k++) {
        const double xs = xb + k * 0.05 * spus_;
        if (xs < 1.0) continue;
        const double e = pulses(xs);
        if (e > best) { best = e; x0 = xs; }
    }
    if (best < 0 || x0 + 122.0 * spus_ > (double)len - 3.0) return false;
    // the preamble again, at the refined timing
    double pulse = 0, minH = 1e30;
    for (int k = 0; k < 4; k++) {
        const double xs = x0 + pulseAt[k] * spus_;
        const double h = (F(xs + hw) - F(xs)) / hw;
        pulse += h; minH = std::min(minH, h);
    }
    pulse *= 0.25;
    static const double gapAt[12] = {0.5, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 5.5, 6.0, 6.5, 7.0, 7.5};
    double gap = 0, gapIn = 0;
    for (int k = 0; k < 12; k++) {
        const double xs = x0 + gapAt[k] * spus_;
        const double g = (F(xs + hw) - F(xs)) / hw;
        gap += g;
        if (k < 6) gapIn += g;
    }
    gap /= 12; gapIn /= 6;
    const bool legacy = kSum_ <= 0;
    if (legacy) {
        if (minH <= pulseThreshold() || pulse <= kGap_ * gap || pulse <= kGap_ * gapIn) return false;
    } else {
        // the four pulses together against the quiet windows: with a noise window of standard deviation cv n0, the difference of the mean of 4 and
        // the mean of 12 windows has about 0.58 cv n0. Each pulse on its own only has to be above the noise (a weak pulse is often below 3 n0).
        // lowest effort (a busy computer): only the preambles the stricter test of before 3.0 accepts
        if (effort_ == 0 && !(minH > pulseThreshold() && pulse > kGap_ * gap && pulse > kGap_ * gapIn)) return false;
        const double sd = 0.58 * cv_ * n0_;
        if (minH <= n0_ * (1.0 + kMinP_ * cv_) || pulse - gap <= kSum_ * sd || pulse <= kGap_ * gap || pulse <= kGap_ * gapIn) return false;
        // the data: a real frame has a pulse in one half of every bit, noise has about the same power in both. Mean contrast of the first 56 bits
        // against kData_ standard deviations of a noise window (noise alone: about 1.13, with about 0.11 of spread).
        double c = 0;
        for (int k = 0; k < 56; k++) {
            const double s0 = x0 + (8.0 + k) * spus_;
            c += std::fabs((F(s0 + hw) - F(s0)) - (F(s0 + 2 * hw) - F(s0 + hw)));
        }
        c /= 56.0 * hw;
        if (c <= kData_ * cv_ * n0_) { dataRejects_++; return false; }
    }
    preambles_++;
    // Overlapping replies (step 6): the candidate lies in a frame that was subtracted, or is tried again after one was, or another preamble shows
    // inside its data, or a frame that decoded ends after its start, or it is strong (10 dB and more) and still fails everything else: a
    // stronger reply hides the preamble of a weaker one in its data, where the preamble test cannot see it.
    const bool inCancel = overlap_ && bin >= cancelStart_ && bin < cancelEnd_;
    bool second = false;
    if (overlap_ && effort_ >= 1) {
        for (int64_t k = bin + 24, e = std::min<int64_t>(bin + kFrameBins - 24, scoreEnd_); k < e && !second; k++)
            second = k >= binBase_ && score_[(size_t)(k - binBase_)] > 0.f;
    }
    // A failure is only counted (the "failed the CRC" figure, and the sign of activity on the channel) for a preamble that would also have passed
    // the stricter test of before 3.0: the looser one finds many weak candidates that the figure would otherwise be full of. Not for a candidate
    // inside a subtracted frame (what is left of a strong frame may look like a preamble) nor on a retry (it was counted the first time).
    const bool countable = (legacy || (minH > pulseThreshold() && pulse > kGap_ * gap && pulse > kGap_ * gapIn)) && !inCancel && !retrying_;

    AdsbRaw raw;
    const double nz = std::max((double)n0_, 1e-12);
    const double sig = std::max(pulse - nz, 1e-12);
    const float levelDbfs = (float)(10.0 * std::log10(sig));
    const float snrDb = (float)(10.0 * std::log10(sig / nz));
    // below this a frame that fails the CRC is not tried again with other settings. The older rule used the SNR per sample, which at high sample
    // rates (more noise per sample) left out frames that are well above the threshold in 2 MHz.
    const bool worth = legacy ? snrDb >= 6.0f : 10.0 * std::log10(sig / (nz * std::max(1e-3, (double)cv_ * cv_))) >= kWorthDb_;
    const bool suspect = overlap_ && effort_ >= 1 && (inCancel || retrying_ || second || bin < goodEnd_ || (worth && snrDb >= 10.0f));

    // The first 56 bits of the data, with a window of `w` samples centred on each half microsecond slot, starting at x. Collects the energy that is
    // in the winning half of every bit: the quantity that timing should maximise.
    auto slicedEnergy = [&](double x, double w, int bits) {
        double e = 0;
        for (int k = 0; k < bits; k++) {
            const double c = x + (8.0 + k) * spus_ + 0.5 * hw;
            const double e1 = F(c + 0.5 * w) - F(c - 0.5 * w), e0 = F(c + hw + 0.5 * w) - F(c + hw - 0.5 * w);
            e += std::max(e1, e0);
        }
        return e;
    };
    // bits and confidences with windows of width w (in samples) centred on the half-bit slots
    // tie: a bit whose two half energies differ by less than this fraction of the pulse contrast takes the value of the bit before it (see below)
    auto slice = [&](AdsbRaw& r, double x, double w, double tie) {
        std::memset(r.bytes, 0, sizeof r.bytes);
        const double tieE = tie * sig * w;
        int prev = 1;                                                  // the guard time before the data is quiet: as if the bit before were a 1
        for (int k = 0; k < 112; k++) {
            const double c = x + (8.0 + k) * spus_ + 0.5 * hw;
            const double e1 = F(c + 0.5 * w) - F(c - 0.5 * w), e0 = F(c + hw + 0.5 * w) - F(c + hw - 0.5 * w);
            const int b = std::fabs(e1 - e0) < tieE ? prev : (e1 > e0 ? 1 : 0);
            if (b) r.bytes[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
            prev = b;
            r.conf[k] = (float)(std::fabs(e1 - e0) / w);
        }
        r.bits = 112;
        r.levelDbfs = levelDbfs; r.snrDb = snrDb;
        r.timeSec = ((double)smpBase_ + x) / rate_ - delaySec_;
    };
    double xUsed = x0;                                                 // the timing of the slice handed on last (for the cancellation)
    auto attempt = [&](double x, double w, bool last, double tie = 0.0) {
        slice(raw, x, w, tie);
        xUsed = x;
        return emit(raw, last && countable);
    };

    // effort (setEffort): 2 every pass for every candidate; 1 the coherent pass and the retries only for candidates that pass the stricter test of
    // before 3.0; 0 no coherent pass, retries only for those (about the cost of before 3.0)
    const bool retries = worth && (effort_ >= 2 || countable);
    const bool useCoh = coherent_ && (effort_ >= 2 || (effort_ == 1 && countable));
    const bool more = suspect;                                         // the separation slicers come last: no failure is counted before them
    bool good = coherentFirst_ ? (slice(raw, x0, hw, 0.0), false) : attempt(x0, hw, !retries && !useCoh && !more);
    // the coherent slicer, guided by the bits just sliced: helps most near the threshold, where the power slicer loses about 1.5 dB
    if (!good && useCoh) {
        AdsbRaw guide = raw, isi;
        if (sliceCoherent(raw, x0, hw, guide, &isi)) {
            xUsed = x0;
            good = emit(raw, false);
            // the same halves, with the smearing of each pulse into its neighbours taken into account (sequence estimation)
            if (!good && std::memcmp(isi.bytes, raw.bytes, 14) != 0) { raw = isi; good = emit(raw, !retries && !more && countable); }
            else if (!good && !retries && !more) good = emit(raw, countable);
        } else if (!retries && !more) good = emit(raw, countable);   // the failure is counted once
    }
    double xd = x0;
    if (!good && retries) {
        // timing again from the data: the shift that puts the most energy into the winning half of each of the first 56 bits and the preamble
        double bestE = -1;
        for (int k = -4; k <= 4; k++) {
            const double xs = x0 + k * 0.04 * spus_;
            const double e = slicedEnergy(xs, hw, 56) + pulses(xs);
            if (e > bestE) { bestE = e; xd = xs; }
        }
        if (std::fabs(xd - x0) > 1e-9) good = attempt(xd, hw, false);
        // narrower windows, which keep out of the neighbouring pulses when the radio's filter has smeared them
        static const double widths[3] = {0.6, 0.4, 0.25};
        for (int k = 0; k < 3 && !good; k++) good = attempt(xd, widths[k] * hw, false);
        // Samples that fall on the edges of the pulses (at 2 Msps, at one sampling phase in about four) show a run of equal bits as a flat line: the
        // two halves of a bit are then equal, and the bit is the same as the one before it. Only tried when everything else has failed.
        static const double ties[3] = {0.3, 0.4, 0.5};
        for (int k = 0; k < 3 && !good; k++) good = attempt(xd, hw, k == 2 && !more, ties[k]);
    }
    bool separated = false;
    if (!good && more && coherent_) {
        // separation: another reply is on the air during this one (sliceCoherent, sep)
        AdsbRaw pwr, coh, isi2, sep;
        slice(pwr, xd, hw, 0.0);
        if (sliceCoherent(coh, xd, hw, pwr, &isi2, &sep)) {
            const AdsbRaw* hyp[2] = {&sep, nullptr};
            int nh = 1;
            // multi-hypothesis: the coherent bits with the bits on which the power, coherent, sequence and separation decisions disagree made the
            // least certain (in their order), so that the tracker's repair (CRC syndromes over the least certain bits) works on those first
            AdsbRaw mix = coh;
            const AdsbRaw* others[3] = {&pwr, &isi2, &sep};
            int dis = 0;
            for (int k = 0; k < 112; k++) {
                const int m = (mix.bytes[k >> 3] >> (7 - (k & 7))) & 1;
                bool differ = false;
                for (const AdsbRaw* o : others) if (((o->bytes[k >> 3] >> (7 - (k & 7))) & 1) != m) differ = true;
                if (differ) { mix.conf[k] *= 1e-3f; dis++; }
            }
            if (dis > 0 && dis <= 40) hyp[nh++] = &mix;
            for (int i = 0; i < nh && !good; i++) {
                if (i == 0 && std::memcmp(sep.bytes, coh.bytes, 14) == 0 && std::memcmp(sep.bytes, isi2.bytes, 14) == 0) continue;   // tried already
                raw = *hyp[i];
                xUsed = xd;
                good = emit(raw, i == nh - 1 && countable);
            }
            separated = good;
        }
        if (!good && countable && !separated) emit(raw, true);   // the failure is counted once
    }
    if (good) {
        const int64_t end = bin + (int64_t)(8 + raw.bits) * 4;
        goodEnd_ = std::max(goodEnd_, end);
        if (inCancel || retrying_ || separated) recovered_++;
        // subtract it, and look on inside it: a second reply buried in its data is found on what is left
        const bool cut = overlap_ && effort_ >= 1 && cancelFrame(xUsed, raw);
        if (cut) {
            cancelled_++;
            Cut& c = cuts_[cutPos_++ & 3];
            std::memcpy(c.head, raw.bytes, 4); c.t = raw.timeSec;
            cancelStart_ = bin; cancelEnd_ = std::max(cancelEnd_, end);
            skipUntil_ = std::max(skipUntil_, bin + 8);
            if (!retrying_ && !failed_.empty()) {
                // candidates that started before this frame and failed while it was on the air: again, on what is left
                std::vector<int64_t> list;
                list.swap(failed_);
                retrying_ = true;
                for (int64_t fb : list) {
                    if (fb < bin && fb + kFrameBins > bin) tryFrame(fb);
                    else if (fb >= bin - kFrameBins) failed_.push_back(fb);
                }
                retrying_ = false;
            }
        } else skipUntil_ = std::max(skipUntil_, end - 4);
    } else if (overlap_ && !retrying_ && effort_ >= 1) {
        failed_.erase(std::remove_if(failed_.begin(), failed_.end(), [&](int64_t f) { return f < bin - kFrameBins; }), failed_.end());
        if (failed_.size() < 16) failed_.push_back(bin);
    }
    return good;
}


// Coherent slicing. The pulses of one reply come from one oscillator: within the 120 us of a frame they have the same phase apart from the carrier
// offset (transponders may be up to 1 MHz off). The offset comes from the lag-1 products of the samples inside the pulses that the power slicer found
// (`guide`), the samples are turned back by it, and every half bit is the complex integral of its window instead of the integral of the power. A
// phase reference from the pulses around each bit (the preamble and the guide's pulses, 32 us of them) then gives a real number per bit: the difference
// of the two halves along the reference. Noise outside the phase of the signal no longer counts, which is the coherent detection gain (about 1.5 dB
// in bit error rate near the threshold). Returns false when the frame cannot be sliced (not enough samples).
bool AdsbDemod::sliceCoherent(AdsbRaw& r, double x, double w, const AdsbRaw& guide, AdsbRaw* isi, AdsbRaw* sep) {
    const double hw = 0.5 * spus_;
    const int64_t s0 = (int64_t)std::floor(x) - 2;
    const int64_t s1 = (int64_t)std::ceil(x + 120.0 * spus_) + 3;
    if (s0 < 0 || s1 >= (int64_t)zs_.size()) return false;
    const cf32* z = zs_.data() + s0;
    const size_t L = (size_t)(s1 - s0);
    auto bitOf = [&](int k) { return (guide.bytes[k >> 3] >> (7 - (k & 7))) & 1; };
    const int nb = bitOf(0) ? 112 : 56;   // the guide's format: DF 16 and up are long, the others short (the rest of a short one is noise)
    // pulse windows (start, in local samples): the preamble's four, then one per bit
    static const double pre[4] = {0.0, 1.0, 3.5, 4.5};
    auto pulseStart = [&](int i) -> double {   // i < 4: preamble pulse, otherwise bit i - 4
        if (i < 4) return x + pre[i] * spus_ - (double)s0;
        const int k = i - 4;
        return x + (8.0 + k) * spus_ + (bitOf(k) ? 0.0 : hw) - (double)s0;
    };
    // the carrier offset: lag-1 products over the samples well inside the pulses
    std::complex<double> R(0, 0);
    for (int i = 0; i < 4 + nb; i++) {
        const double a = pulseStart(i);
        const int n0 = (int)std::ceil(a + 0.1 * hw), n1 = (int)std::floor(a + 0.9 * hw);
        for (int n = std::max(n0, 0); n < n1 && n + 1 < (int)L; n++) {
            const cf32 p = z[n + 1] * std::conj(z[n]);
            R += std::complex<double>(p.real(), p.imag());
        }
    }
    const double om = std::arg(R);    // radians per sample
    // derotate and integrate (the same straight-line model between samples as the power integral)
    ly_.resize(L); lc_.resize(L);
    {
        const std::complex<double> st = std::polar(1.0, -om);
        std::complex<double> ph(1, 0);
        for (size_t n = 0; n < L; n++) {
            const std::complex<double> v = std::complex<double>(z[n].real(), z[n].imag()) * ph;
            ly_[n] = cf32((float)v.real(), (float)v.imag());
            ph *= st;
            if ((n & 255) == 255) ph /= std::abs(ph);
        }
        lc_[0] = 0;
        for (size_t n = 0; n + 1 < L; n++) lc_[n + 1] = lc_[n] + 0.5 * (std::complex<double>(ly_[n].real(), ly_[n].imag()) + std::complex<double>(ly_[n + 1].real(), ly_[n + 1].imag()));
    }
    auto Fc = [&](double xl) {
        if (xl < 0) xl = 0;
        if (xl > (double)(L - 2)) xl = (double)(L - 2);
        const size_t j = (size_t)xl;
        const double f = xl - (double)j;
        const std::complex<double> y0(ly_[j].real(), ly_[j].imag()), y1(ly_[j + 1].real(), ly_[j + 1].imag());
        return lc_[j] + f * y0 + 0.5 * f * f * (y1 - y0);
    };
    // the two halves of every bit, and the pulses of the preamble
    std::complex<double> A1[112], A0[112], P[4 + 112];
    for (int i = 0; i < 4; i++) { const double a = pulseStart(i); P[i] = Fc(a + 0.5 * hw + 0.5 * w) - Fc(a + 0.5 * hw - 0.5 * w); }
    for (int k = 0; k < 112; k++) {
        const double cc = x + (8.0 + k) * spus_ + 0.5 * hw - (double)s0;
        A1[k] = Fc(cc + 0.5 * w) - Fc(cc - 0.5 * w);
        A0[k] = Fc(cc + hw + 0.5 * w) - Fc(cc + hw - 0.5 * w);
    }
    int bits[112];
    for (int k = 0; k < 112; k++) bits[k] = bitOf(k);
    // The lag-1 estimate is coarse (a pulse holds only a few samples: at 10 dB it is tens of kHz off, which turns the phase by radians over the
    // frame). The fine offset: the turn per microsecond that lines up the pulses of the whole frame (the preamble and the guide's pulses), searched
    // over +-1.2 rad/us (+-190 kHz) and refined on a parabola; the halves are then turned back by it at their centres.
    {
        double tc[4 + 112];
        std::complex<double> Q[4 + 112];
        for (int i = 0; i < 4; i++) { Q[i] = P[i]; tc[i] = (x + pre[i] * spus_ + 0.5 * hw - (double)s0) / spus_; }
        for (int k = 0; k < nb; k++) { Q[4 + k] = bits[k] ? A1[k] : A0[k]; tc[4 + k] = (x + (8.0 + k + (bits[k] ? 0.25 : 0.75)) * spus_ - (double)s0) / spus_; }
        const double t0 = tc[0];
        auto power = [&](double d) {
            std::complex<double> s(0, 0);
            for (int i = 0; i < 4 + nb; i++) s += Q[i] * std::polar(1.0, -d * (tc[i] - t0));
            return std::norm(s);
        };
        // the pulse centres lie on a grid of 0.25 us: one FFT of the pulses placed on that grid gives the whole search at once
        const int N = 2048;
        fftBuf_.assign(N, cf32(0, 0));
        for (int i = 0; i < 4 + nb; i++) {
            const int m = (int)std::lround(4.0 * (tc[i] - t0));
            if (m >= 0 && m < N) fftBuf_[(size_t)m] += cf32((float)Q[i].real(), (float)Q[i].imag());
        }
        fft_.forward(fftBuf_.data());
        const int K = 98;                                    // +-1.2 rad/us: d = 8 pi k / N rad/us for bin k
        double best = -1; int bi = 0;
        auto at = [&](int k) { return (double)std::norm(fftBuf_[(size_t)((k + N) % N)]); };
        for (int k = -K; k <= K; k++) { const double v = at(k); if (v > best) { best = v; bi = k; } }
        double kf = bi;
        {
            const double y0 = at(bi - 1), y1 = at(bi), y2 = at(bi + 1), den = y0 - 2 * y1 + y2;
            if (den < 0) kf += 0.5 * (y0 - y2) / den;
        }
        double d = 8.0 * M_PI * kf / N;
        if (power(-d) > power(d)) d = -d;                    // whichever sign convention the FFT has
        for (int i = 0; i < 4; i++) P[i] *= std::polar(1.0, -d * (tc[i] - t0));
        for (int k = 0; k < 112; k++) {
            A1[k] *= std::polar(1.0, -d * ((x + (8.0 + k + 0.25) * spus_ - (double)s0) / spus_ - t0));
            A0[k] *= std::polar(1.0, -d * ((x + (8.0 + k + 0.75) * spus_ - (double)s0) / spus_ - t0));
        }
    }
    std::complex<double> refs[112];
    // two passes: the reference from the guide's decisions, then from the coherent decisions
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < 112; k++) P[4 + k] = bits[k] ? A1[k] : A0[k];
        // running sum of the pulse phasors over 33 pulses centred on each bit
        const int H = 16;
        std::complex<double> win(0, 0);
        int lo = 0, hi = 0;   // window [lo, hi) in pulse indices
        for (int k = 0; k < 112; k++) {
            const int c = 4 + k;
            const int wl = std::max(0, c - H), wh = std::max(wl, std::min(4 + nb, c + H + 1));
            while (hi < wh) win += P[hi++];
            while (lo < wl) win -= P[lo++];
            const std::complex<double> ref = win - P[c];        // leave the bit itself out of its own reference
            const double m = std::abs(ref);
            refs[k] = m > 0 ? std::conj(ref) / m : std::complex<double>(0, 0);
            const double d = ((A1[k] - A0[k]) * refs[k]).real();
            bits[k] = d > 0 ? 1 : 0;
            r.conf[k] = (float)(std::fabs(d) / w);
        }
    }
    std::memset(r.bytes, 0, sizeof r.bytes);
    for (int k = 0; k < 112; k++) if (bits[k]) r.bytes[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
    r.bits = 112;
    r.levelDbfs = guide.levelDbfs; r.snrDb = guide.snrDb; r.timeSec = guide.timeSec;
    if (sep) *sep = r;
    if (!isi) return true;

    // Sequence estimation. Along the phase reference every half bit is a real number a_j = gm b_(j-1) + g0 b_j + gp b_(j+1) + noise, b being 1 where
    // a pulse is: the radio's filter spreads each pulse into the half bits next to it (much of it at 2 Msps with a 1.75 MHz filter). The three taps
    // come from the decisions just made (least squares over the half bits of the frame); a two-state Viterbi over the bits (each bit is either 10 or 01)
    // then finds the sequence that fits best. The half bit before the data and the one after it are quiet. Done twice: the second time with the
    // phase reference and the taps from the first sequence.
    auto sq = [](double e) { return e * e; };
    auto det3 = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    *isi = r;
    for (int iter = 0; iter < 2; iter++) {
        if (iter > 0) {   // the reference again, from the sequence of the first pass
            for (int k = 0; k < 112; k++) P[4 + k] = bits[k] ? A1[k] : A0[k];
            const int H = 16;
            std::complex<double> win(0, 0);
            int lo = 0, hi = 0;
            for (int k = 0; k < nb; k++) {
                const int c = 4 + k;
                const int wl = std::max(0, c - H), wh = std::max(wl, std::min(4 + nb, c + H + 1));
                while (hi < wh) win += P[hi++];
                while (lo < wl) win -= P[lo++];
                const std::complex<double> ref = win - P[c];
                const double m = std::abs(ref);
                refs[k] = m > 0 ? std::conj(ref) / m : std::complex<double>(0, 0);
            }
        }
        double a[224];
        int b[226];   // b[j + 1] = pulse in half bit j; b[0] and b[2 nb + 1] quiet
        for (int k = 0; k < nb; k++) {
            a[2 * k] = (A1[k] * refs[k]).real();
            a[2 * k + 1] = (A0[k] * refs[k]).real();
            b[2 * k + 1] = bits[k]; b[2 * k + 2] = 1 - bits[k];
        }
        b[0] = 0; b[2 * nb + 1] = 0;
        double M[3][3] = {{0}}, v[3] = {0};
        for (int j = 0; j < 2 * nb; j++) {
            const double u[3] = {(double)b[j], (double)b[j + 1], (double)b[j + 2]};
            for (int p = 0; p < 3; p++) { v[p] += u[p] * a[j]; for (int q = 0; q < 3; q++) M[p][q] += u[p] * u[q]; }
        }
        const double D = det3(M);   // the 3 x 3 system by Cramer's rule
        if (!(std::fabs(D) > 1e-9 * std::pow(M[1][1], 3))) return true;
        double g[3];
        for (int p = 0; p < 3; p++) {
            double T[3][3];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) T[i][j] = j == p ? v[i] : M[i][j];
            g[p] = det3(T) / D;
        }
        const double gm = g[0], g0 = g[1], gp = g[2];
        if (!(g0 > 0) || std::fabs(gm) > g0 || std::fabs(gp) > g0) return true;
        // Viterbi: state = the previous bit. Moving from bit k-1 = s to bit k = t settles two half bits: the second half of bit k-1 (pulse 1-s, left
        // neighbour s, right neighbour t) and the first half of bit k (pulse t, left 1-s, quiet before the data, right 1-t). The last half bit (right
        // neighbour quiet) is added at the end.
        double cost[2];
        uint8_t from[112][2];
        for (int t = 0; t < 2; t++) { cost[t] = sq(a[0] - (g0 * t + gp * (1 - t))); from[0][t] = 0; }
        for (int k = 1; k < nb; k++) {
            double nc[2] = {1e300, 1e300};
            for (int t = 0; t < 2; t++) for (int s = 0; s < 2; s++) {
                const double m = cost[s] + sq(a[2 * k - 1] - (gm * s + g0 * (1 - s) + gp * t)) + sq(a[2 * k] - (gm * (1 - s) + g0 * t + gp * (1 - t)));
                if (m < nc[t]) { nc[t] = m; from[k][t] = (uint8_t)s; }
            }
            cost[0] = nc[0]; cost[1] = nc[1];
        }
        for (int s = 0; s < 2; s++) cost[s] += sq(a[2 * nb - 1] - (gm * s + g0 * (1 - s)));
        int s = cost[1] < cost[0] ? 1 : 0;
        for (int k = nb - 1; k >= 0; k--) { bits[k] = s; s = from[k][s]; }
        std::memset(isi->bytes, 0, sizeof isi->bytes);
        for (int k = 0; k < 112; k++) if (bits[k]) isi->bytes[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
        if (!sep || iter != 1) continue;
        // Separation from another reply on the air (sep). What is left of every half bit after this frame's own model (the taps along its phase
        // reference) is either noise or a piece of the other reply, whose level is about the same all through (beta, the median of the residuals well
        // above the noise). The cost of a half bit is then its residual power up to a small fixed amount, flat (the other reply may be there) up to
        // 1.25 beta, and growing again beyond that: a wrong decision leaves this frame's whole pulse in the residual, which the other reply's level does
        // not explain. Unlike the slicers above this does not depend on the phase between the two replies (opposed phases cancel a pulse).
        std::complex<double> cz[224];
        for (int k = 0; k < nb; k++) { cz[2 * k] = A1[k] * refs[k]; cz[2 * k + 1] = A0[k] * refs[k]; }
        int bb[226];
        bb[0] = 0; bb[2 * nb + 1] = 0;
        for (int k = 0; k < nb; k++) { bb[2 * k + 1] = bits[k]; bb[2 * k + 2] = 1 - bits[k]; }
        const double sn = std::sqrt(std::max(1e-30, 1.5 * (double)n0_ * w));
        double mags[224];
        int nm = 0;
        for (int j = 0; j < 2 * nb; j++) {
            const double m = std::abs(cz[j] - (gm * bb[j] + g0 * bb[j + 1] + gp * bb[j + 2]));
            if (m > 4.0 * sn) mags[nm++] = m;
        }
        if (nm < 6) continue;   // nothing else on the air
        std::nth_element(mags, mags + nm / 2, mags + nm);
        const double B = 1.25 * mags[nm / 2], flat = 0.15 * B * B;
        auto f = [&](const std::complex<double>& e) { const double m = std::abs(e); return std::min(m * m, flat + (m > B ? (m - B) * (m - B) : 0.0)); };
        double cs[2];
        uint8_t fr[112][2];
        for (int t = 0; t < 2; t++) { cs[t] = f(cz[0] - (g0 * t + gp * (1 - t))); fr[0][t] = 0; }
        for (int k = 1; k < nb; k++) {
            double nc[2] = {1e300, 1e300};
            for (int t = 0; t < 2; t++) for (int s2 = 0; s2 < 2; s2++) {
                const double m = cs[s2] + f(cz[2 * k - 1] - (gm * s2 + g0 * (1 - s2) + gp * t)) + f(cz[2 * k] - (gm * (1 - s2) + g0 * t + gp * (1 - t)));
                if (m < nc[t]) { nc[t] = m; fr[k][t] = (uint8_t)s2; }
            }
            cs[0] = nc[0]; cs[1] = nc[1];
        }
        for (int s2 = 0; s2 < 2; s2++) cs[s2] += f(cz[2 * nb - 1] - (gm * s2 + g0 * (1 - s2)));
        int sb = cs[1] < cs[0] ? 1 : 0;
        int sbits[112];
        for (int k = 0; k < 112; k++) sbits[k] = bits[k];
        for (int k = nb - 1; k >= 0; k--) { sbits[k] = sb; sb = fr[k][sb]; }
        std::memset(sep->bytes, 0, sizeof sep->bytes);
        for (int k = 0; k < 112; k++) if (sbits[k]) sep->bytes[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
    }
    return true;
}

// Subtraction of a decoded frame (step 6). The frame is rebuilt from its bits: its carrier (lag-1 products inside its pulses, then the turn per
// microsecond that lines up all of its pulses, as in sliceCoherent), its complex gain (the mean of its pulses over +-20 us, which follows a slow
// drift) and its pulse shape, learnt from the frame itself: the mean of the samples, turned back and divided by the gain, for every pattern of three
// neighbouring half bits and every eighth of a half bit, so that neither the radio's filter nor the sampling phase needs a model. The rebuilt frame
// is subtracted from the samples, and the power, its integral, the bins and the preamble scores are worked out again from there. Undone (false) when
// it takes away less than half of the energy, which means that the frame was not where it was thought to be.
bool AdsbDemod::cancelFrame(double x, const AdsbRaw& r) {
    if (!noiseReady_) return false;
    const int nb = r.bits == 56 ? 56 : 112;
    const int nh = 16 + 2 * nb;                                   // half bits: 16 of the preamble, two for every bit
    const double hs = 0.5 * spus_;                                // samples per half bit
    const int64_t s0 = (int64_t)std::floor(x - hs) - 1;
    const int64_t s1 = (int64_t)std::ceil(x + (nh + 1) * hs) + 2;
    if (s0 < 1 || s1 + 2 >= (int64_t)zs_.size()) return false;
    uint8_t q[16 + 224] = {};                                     // q[j]: a pulse in half bit j
    auto Q = [&](int j) -> int { return j < 0 || j >= nh ? 0 : q[j]; };
    q[0] = q[2] = q[7] = q[9] = 1;
    for (int k = 0; k < nb; k++) q[16 + 2 * k + (((r.bytes[k >> 3] >> (7 - (k & 7))) & 1) ? 0 : 1)] = 1;
    cf32* z = zs_.data();
    std::complex<double> R(0, 0);
    for (int j = 0; j < nh; j++) {
        if (!q[j]) continue;
        const double a = x + j * hs;
        const int64_t n0 = (int64_t)std::ceil(a + 0.1 * hs), n1 = (int64_t)std::floor(a + 0.9 * hs);
        for (int64_t n = n0; n < n1; n++) { const cf32 p = z[n + 1] * std::conj(z[n]); R += std::complex<double>(p.real(), p.imag()); }
    }
    const size_t L = (size_t)(s1 - s0);
    rot_.resize(L); yv_.resize(L);
    auto turn = [&](double w) {                                   // rot_ = e^(-i w n), yv_ = z rot_
        const std::complex<double> st = std::polar(1.0, -w);
        std::complex<double> ph(1, 0);
        for (size_t n = 0; n < L; n++) {
            rot_[n] = cf32((float)ph.real(), (float)ph.imag());
            yv_[n] = z[s0 + (int64_t)n] * rot_[n];
            ph *= st;
            if ((n & 255) == 255) ph /= std::abs(ph);
        }
    };
    std::complex<double> P[16 + 224];
    auto phasors = [&]() {                                        // the mean of the samples well inside every pulse
        for (int j = 0; j < nh; j++) {
            P[j] = 0;
            if (!q[j]) continue;
            const double a = x + j * hs - (double)s0;
            int64_t n0 = (int64_t)std::ceil(a + 0.2 * hs), n1 = (int64_t)std::floor(a + 0.8 * hs);
            if (n1 < n0) n0 = n1 = (int64_t)std::lround(a + 0.5 * hs);
            std::complex<double> s(0, 0);
            for (int64_t n = n0; n <= n1; n++) s += std::complex<double>(yv_[(size_t)n].real(), yv_[(size_t)n].imag());
            P[j] = s / (double)(n1 - n0 + 1);
        }
    };
    const double om = std::arg(R);
    turn(om);
    phasors();
    {
        // the fine offset: the pulse centres lie on a grid of 0.25 us (half bit j at 2 j + 1), one FFT gives the turn per microsecond that lines them up
        const int N = 2048;
        fftBuf_.assign(N, cf32(0, 0));
        for (int j = 0; j < nh; j++) if (q[j]) fftBuf_[(size_t)(2 * j + 1)] += cf32((float)P[j].real(), (float)P[j].imag());
        fft_.forward(fftBuf_.data());
        const int K = 98;
        auto at = [&](int k) { return (double)std::norm(fftBuf_[(size_t)((k + N) % N)]); };
        double best = -1; int bi = 0;
        for (int k = -K; k <= K; k++) { const double v = at(k); if (v > best) { best = v; bi = k; } }
        double kf = bi;
        const double y0 = at(bi - 1), y1 = at(bi), y2 = at(bi + 1), den = y0 - 2 * y1 + y2;
        if (den < 0) kf += 0.5 * (y0 - y2) / den;
        double d = 8.0 * M_PI * kf / N;                           // radians per microsecond
        auto power = [&](double dd) {
            std::complex<double> s(0, 0);
            for (int j = 0; j < nh; j++) if (q[j]) s += P[j] * std::polar(1.0, -dd * (0.5 * j));
            return std::norm(s);
        };
        if (power(-d) > power(d)) d = -d;
        turn(om + d / spus_);
        phasors();
    }
    // the gain of every half bit: the mean pulse within +-40 half bits
    std::complex<double> S[16 + 224 + 1], G[16 + 224];
    int C[16 + 224 + 1];
    S[0] = 0; C[0] = 0;
    for (int j = 0; j < nh; j++) { S[j + 1] = S[j] + (q[j] ? P[j] : std::complex<double>(0, 0)); C[j + 1] = C[j] + q[j]; }
    for (int j = 0; j < nh; j++) {
        const int lo = std::max(0, j - 40), hi = std::min(nh, j + 41);
        G[j] = (S[hi] - S[lo]) / (double)std::max(1, C[hi] - C[lo]);
    }
    // the pulse shape per pattern of three half bits and eighth of a half bit
    std::complex<double> T[8][8] = {};
    int TC[8][8] = {};
    auto place = [&](size_t n, int& j, int& p, int& ph) {
        const double t = ((double)s0 + (double)n - x) / hs;
        j = (int)std::floor(t);
        if (j < -1 || j > nh) return false;
        ph = std::min(7, (int)((t - j) * 8.0));
        p = Q(j - 1) << 2 | Q(j) << 1 | Q(j + 1);
        return p != 0;
    };
    auto gainAt = [&](int j) { return G[std::min(nh - 1, std::max(0, j))]; };
    for (size_t n = 0; n < L; n++) {
        int j, p, ph;
        if (!place(n, j, p, ph)) continue;
        const std::complex<double> g = gainAt(j);
        const double gn = std::norm(g);
        if (!(gn > 0)) continue;
        T[p][ph] += std::complex<double>(yv_[n].real(), yv_[n].imag()) * std::conj(g) / gn;
        TC[p][ph]++;
    }
    for (int p = 0; p < 8; p++) for (int ph = 0; ph < 8; ph++) if (TC[p][ph]) T[p][ph] /= (double)TC[p][ph];
    // subtract, and keep the samples in case it does not work out
    ly_.assign(z + s0, z + s1);
    double eb = 0, ea = 0;
    for (size_t n = 0; n < L; n++) {
        cf32& v = z[s0 + (int64_t)n];
        eb += std::norm(v);
        int j, p, ph;
        if (place(n, j, p, ph) && TC[p][ph]) {
            const std::complex<double> e = gainAt(j) * T[p][ph];
            v -= cf32((float)e.real(), (float)e.imag()) * std::conj(rot_[n]);
        }
        ea += std::norm(v);
    }
    if (!(ea < 0.5 * eb)) { std::copy(ly_.begin(), ly_.end(), z + s0); return false; }
    // power, its integral, the bins and the scores again
    float* pw = pw_.data();
    double* cum = cum_.data();
    const size_t len = pw_.size();
    for (int64_t n = s0; n < s1; n++) pw[n] = std::norm(z[n]);
    for (size_t j = (size_t)s0 - 1; j + 1 < len; j++) cum[j + 1] = cum[j] + 0.5 * ((double)pw[j] + (double)pw[j + 1]);
    const double base = (double)smpBase_;
    const int64_t binEnd = binBase_ + (int64_t)bin_.size();
    const int64_t bLo = std::max(binBase_, (int64_t)std::floor(((double)s0 + base) / spb_) - 1);
    const int64_t bHi = std::min(binEnd, (int64_t)std::ceil(((double)s1 + base) / spb_) + 1);
    for (int64_t b = bLo; b < bHi; b++) {
        const double xa = (double)b * spb_ - base;
        if (xa < 0 || xa + spb_ >= (double)len - 1.0) continue;
        bin_[(size_t)(b - binBase_)] = (float)((integral(cum, pw, xa + spb_) - integral(cum, pw, xa)) / spb_);
    }
    const bool legacy = kSum_ <= 0;
    const float thr = legacy ? pulseThreshold() : n0_ * (1.0f + kMinP_ * cv_);
    const float dThr = legacy ? 0.f : 2.0f * kSum_ * 0.58f * cv_ * n0_;
    const int64_t e = std::min<int64_t>(bHi, std::min<int64_t>(scoreEnd_, binBase_ + (int64_t)score_.size()));
    for (int64_t b = std::max(binBase_, bLo - 31); b < e; b++) score_[(size_t)(b - binBase_)] = scoreAt(bin_.data() + (b - binBase_), thr, dThr);
    return true;
}

// Drop what the search is done with: samples and bins before the scan position (a little is kept for the timing search)
void AdsbDemod::compact() {
    const int64_t keepBin = std::max<int64_t>(binBase_, scanBin_ - (overlap_ ? kFrameBins + 16 : 8));   // a frame's length back: candidates that failed may be tried again
    const size_t dropBins = (size_t)(keepBin - binBase_);
    if (dropBins >= 2048) {
        bin_.erase(bin_.begin(), bin_.begin() + dropBins);
        score_.erase(score_.begin(), score_.begin() + std::min(dropBins, score_.size()));
        binBase_ = keepBin;
    }
    // samples: nothing before the first bin that is still needed (minus 1.5 us for the timing search)
    const double firstX = (double)binBase_ * spb_ - 1.5 * spus_ - (double)smpBase_;
    if (firstX > 2.0 * kPiece) {
        const size_t drop = (size_t)firstX;
        const double c = cum_[drop];
        pw_.erase(pw_.begin(), pw_.begin() + drop);
        zs_.erase(zs_.begin(), zs_.begin() + drop);
        cum_.erase(cum_.begin(), cum_.begin() + drop);
        for (double& v : cum_) v -= c;
        smpBase_ += drop;
    }
}

} // namespace dect2
