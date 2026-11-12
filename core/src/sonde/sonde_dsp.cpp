// Filters of the radiosonde channel (see sonde_chan.h).
#include "sonde_chan.h"
#include "dect2/common/window.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace sondedsp {

namespace {
double bessel0(double x) { return dect2::window::besselI0(x, 60, 1e-12); }
}

std::vector<float> kaiserLowpass(int taps, double fc, double beta) {
    std::vector<float> h((size_t)taps);
    const double mid = (taps - 1) / 2.0, i0b = bessel0(beta);
    double sum = 0;
    std::vector<double> t((size_t)taps);
    for (int k = 0; k < taps; k++) {
        const double x = k - mid;
        const double a = 2 * M_PI * fc * x;
        const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
        const double r = mid > 0 ? x / mid : 0;
        const double win = bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b;
        t[(size_t)k] = sinc * win;
        sum += t[(size_t)k];
    }
    for (int k = 0; k < taps; k++) h[(size_t)k] = (float)(t[(size_t)k] / sum);
    return h;
}

void Decim::init(int R, int taps, double fcIn, double beta) {
    R_ = std::max(1, R);
    taps_ = std::max(1, taps | 1);
    h_ = kaiserLowpass(taps_, fcIn, beta);
    reset();
}

void Decim::reset() {
    re_.assign((size_t)taps_ - 1, 0.f);
    im_.assign((size_t)taps_ - 1, 0.f);
    nextPos_ = (size_t)taps_ - 1;
    mixPh_ = 0;
}

void Decim::buildMixer() {
    w_.resize(kMixBlock);
    for (int k = 0; k < kMixBlock; k++) {
        const double a = -2 * M_PI * mixF_ * k;
        w_[(size_t)k] = cf32((float)std::cos(a), (float)std::sin(a));
    }
}

void Decim::setMixer(double f) {
    mixF_ = f;
    mixOn_ = f != 0;
    if (mixOn_) buildMixer();
}

void Decim::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    size_t done = 0;
    while (done < n) {
        const size_t m = std::min<size_t>(n - done, 4096);
        const size_t base = re_.size();
        re_.resize(base + m);
        im_.resize(base + m);
        float* re = re_.data() + base;
        float* im = im_.data() + base;
        const cf32* x = in + done;
        if (!mixOn_) {
            for (size_t i = 0; i < m; i++) { re[i] = x[i].real(); im[i] = x[i].imag(); }
        } else {
            for (size_t i0 = 0; i0 < m; i0 += kMixBlock) {
                const size_t cnt = std::min<size_t>(kMixBlock, m - i0);
                const double a = -2 * M_PI * mixPh_;
                const float br = (float)std::cos(a), bi = (float)std::sin(a);
                for (size_t k = 0; k < cnt; k++) {
                    const float wr = w_[k].real(), wi = w_[k].imag();
                    const float pr = br * wr - bi * wi, pi = br * wi + bi * wr;
                    const float xr = x[i0 + k].real(), xi = x[i0 + k].imag();
                    re[i0 + k] = xr * pr - xi * pi;
                    im[i0 + k] = xr * pi + xi * pr;
                }
                mixPh_ += mixF_ * (double)cnt;
                mixPh_ -= std::floor(mixPh_);
            }
        }
        done += m;
        const size_t sz = re_.size();
        while (nextPos_ < sz) {
            const size_t start = nextPos_ + 1 - (size_t)taps_;
            float o[2];
            genutil::dot2(h_.data(), re_.data() + start, im_.data() + start, taps_, o);
            out.emplace_back(o[0], o[1]);
            nextPos_ += (size_t)R_;
        }
        // keep the history the next output needs
        const size_t keepFrom = nextPos_ + 1 - (size_t)taps_;
        if (keepFrom > 0) {
            re_.erase(re_.begin(), re_.begin() + (long)keepFrom);
            im_.erase(im_.begin(), im_.begin() + (long)keepFrom);
            nextPos_ -= keepFrom;
        }
    }
}

void SymbolChain::init(double fs, double baud, bool afsk, bool midpoint, double devHz, int thrMode) {
    thrMode_ = thrMode;
    fs_ = fs; baud_ = baud; afsk_ = afsk; midpoint_ = midpoint;
    devHz_ = afsk ? 0.0 : devHz;   // 4.0 A4: midpoint chains (iMet-54, MRZ) use the tones too, centred on the middle of the extremes
    Lw_ = std::max(2, (int)std::lround(fs / baud));
    tu_.assign((size_t)Lw_, std::complex<double>(0, 0)); tl_.assign((size_t)Lw_, std::complex<double>(0, 0));
    decay_ = (float)(baud / fs / 12.0);                    // about a dozen symbols
    max_ = min_ = 0;
    dp0_ = baud / fs;
    L_ = std::max(1, (int)std::lround(0.7 * fs / baud));
    win_.assign((size_t)L_, 0.f);
    if (afsk_) {
        afLen_ = std::max(2, (int)std::lround(fs / baud));          // one bit: the DFT window
        afBuf_.assign((size_t)afLen_, 0.f);
        const double tones[2] = {1200.0, 2200.0};
        for (int t = 0; t < 2; t++) {
            const double w = 2 * M_PI * tones[t] / fs;
            afRotRe_[t] = std::cos(w); afRotIm_[t] = -std::sin(w);
            afEndRe_[t] = std::cos(w * afLen_) * std::pow(0.99999, afLen_); afEndIm_[t] = -std::sin(w * afLen_) * std::pow(0.99999, afLen_);
        }
    }
    reset();
}

void SymbolChain::reset() {
    max_ = min_ = 0; midC_ = 0;
    afPos_ = 0;
    std::fill(afBuf_.begin(), afBuf_.end(), 0.f);
    afRe_[0] = afRe_[1] = afIm_[0] = afIm_[1] = 0;
    std::fill(win_.begin(), win_.end(), 0.f);
    wpos_ = 0; wsum_ = 0;
    prev_ = 0; havePrev_ = false;
    dp_ = dp0_; ph_ = 0;
    su_ = sl_ = std::complex<double>(0, 0); phU_ = phL_ = 0; tPos_ = 0;
    std::fill(tu_.begin(), tu_.end(), std::complex<double>(0, 0)); std::fill(tl_.begin(), tl_.end(), std::complex<double>(0, 0));
    nTrans_ = 0; nOut_ = 0;
}

} // namespace sondedsp
} // namespace dect2
