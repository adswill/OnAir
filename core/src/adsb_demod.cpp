// ADS-B pulse demodulator (see adsb_demod.h).
#include "dect2/adsb_demod.h"
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
    samples_ = 0;
    reset();
    pw_.reserve(kPiece + (size_t)(spus_ * 140) + 64);
    cum_.reserve(kPiece + (size_t)(spus_ * 140) + 65);
    bin_.reserve(4 * 140 + kPiece * 4 / 8 + 64);
    score_.reserve(4 * 140 + kPiece * 4 / 8 + 64);
}

void AdsbDemod::reset() {
    smpBase_ = samples_;
    pw_.clear();
    cum_.assign(1, 0.0);   // an empty buffer still has cum_[0]
    dcInit_ = false; dcRe_ = dcIm_ = 0;
    binBase_ = spb_ > 0 ? (int64_t)std::ceil((double)smpBase_ / spb_) : 0;   // first bin that starts inside the data
    bin_.clear(); score_.clear();
    scoreEnd_ = scanBin_ = skipUntil_ = binBase_;
    n0_ = 0; noiseReady_ = false;
    blockSum_ = 0; blockCount_ = 0; blockN_ = 0; blockPos_ = 0;
}

void AdsbDemod::feed(const cf32* x, size_t n) {
    if (!ready()) return;
    while (n) {
        if (up_) {
            const size_t m = std::min<size_t>(n, 2048);
            std::copy(hist_, hist_ + 11, tmp_.begin());
            for (size_t j = 0; j < m; j++) tmp_[11 + j] = cf32(clean(x[j].real()), clean(x[j].imag()));
            for (size_t j = 0; j < m; j++) {
                const cf32* w = tmp_.data() + j;
                float re = 0, im = 0;
                for (int k = 0; k < 12; k++) { re += taps_[k] * w[k].real(); im += taps_[k] * w[k].imag(); }
                z_[2 * j] = w[5];
                z_[2 * j + 1] = cf32(re, im);
            }
            std::copy(tmp_.begin() + m, tmp_.begin() + m + 11, hist_);
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
    cum_.resize(base + m);
    float* pw = pw_.data() + base;
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
        float sr = 0, si = 0, bp = 0;
        for (size_t j = 0; j < c; j++) {
            const float xr = clean(b[2 * j]), xi = clean(b[2 * j + 1]);
            const float re = xr - dr, im = xi - di;
            const float p = re * re + im * im;
            pw[i + j] = p;
            bp += p; sr += xr; si += xi;
        }
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

void AdsbDemod::updateNoise(float m) {
    const float w = 0.5f * (m + prevBin_);             // a window of 0.5 us
    prevBin_ = m;
    blockSum_ += m; blockSumW_ += w; blockSumW2_ += (double)w * w;
    if (++blockCount_ < kBlockBins) return;
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
    const float thr = pulseThreshold();
    int64_t b = scoreEnd_;
    const int64_t last = binEnd - 32;
    const size_t need = (size_t)std::max<int64_t>(0, last - binBase_);
    if (score_.size() < need) score_.resize(need, 0.f);
    for (; b < last; b++) {
        const float* p = bn + (b - binBase_);
        float s = 0;
        if (noiseReady_) {
            // windows of two bins (0.5 us): the four pulses first, the cheapest rejection
            const float h0 = p[0] + p[1], h1 = p[4] + p[5];
            if (h0 * 0.5f > thr && h1 * 0.5f > thr) {
                const float h2 = p[14] + p[15], h3 = p[18] + p[19];
                if (h2 * 0.5f > thr && h3 * 0.5f > thr) {
                    const float pulse = (h0 + h1 + h2 + h3) * 0.25f;                       // sum of two bins = twice the window mean
                    float gap = p[2] + p[3] + p[6] + p[7] + p[8] + p[9] + p[10] + p[11] + p[12] + p[13] + p[16] + p[17];   // 6 windows inside the preamble
                    float tail = 0;
                    for (int k = 20; k < 32; k++) tail += p[k];                            // 3 us of quiet before the data
                    const float q = (gap + tail) * (1.0f / 24.0f) * 2.0f;                  // mean of the 12 quiet windows, in the same units as pulse
                    const float qi = gap * (1.0f / 12.0f) * 2.0f;
                    if (pulse > kGap_ * q && pulse > kGap_ * qi) s = (pulse - q) * 0.5f;
                }
            }
        }
        score_[(size_t)(b - binBase_)] = s;
    }
    scoreEnd_ = std::max(scoreEnd_, last);
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
        if (s >= at(i - 1) && s >= at(i - 2) && s > at(i + 1) && s > at(i + 2)) {
            candidates_++;
            tryFrame(b);
        }
    }
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
    if (minH <= pulseThreshold() || pulse <= kGap_ * gap || pulse <= kGap_ * gapIn) return false;
    preambles_++;

    AdsbRaw raw;
    const double nz = std::max((double)n0_, 1e-12);
    const double sig = std::max(pulse - nz, 1e-12);
    const float levelDbfs = (float)(10.0 * std::log10(sig));
    const float snrDb = (float)(10.0 * std::log10(sig / nz));
    const bool worth = snrDb >= 6.0f;      // below this a frame that fails the CRC is not tried again with other settings

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
    auto attempt = [&](double x, double w, bool last, double tie = 0.0) {
        slice(raw, x, w, tie);
        return sink_ ? sink_(raw, last) : false;
    };

    bool good = attempt(x0, hw, !worth);
    if (!good && worth) {
        // timing again from the data: the shift that puts the most energy into the winning half of each of the first 56 bits and the preamble
        double bestE = -1, xd = x0;
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
        for (int k = 0; k < 3 && !good; k++) good = attempt(xd, hw, k == 2, ties[k]);
    }
    if (good) skipUntil_ = bin + (int64_t)(8 + raw.bits) * 4 - 4;
    return good;
}

// Drop what the search is done with: samples and bins before the scan position (a little is kept for the timing search)
void AdsbDemod::compact() {
    const int64_t keepBin = std::max<int64_t>(binBase_, scanBin_ - 8);
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
        cum_.erase(cum_.begin(), cum_.begin() + drop);
        for (double& v : cum_) v -= c;
        smpBase_ += drop;
    }
}

} // namespace dect2
