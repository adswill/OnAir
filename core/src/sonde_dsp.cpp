// Filters of the radiosonde channel (see sonde_chan.h).
#include "sonde_chan.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace sondedsp {

namespace {
double bessel0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}
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

void SymbolChain::init(double fs, double baud) {
    fs_ = fs; baud_ = baud;
    dp0_ = baud / fs;
    L_ = std::max(1, (int)std::lround(0.7 * fs / baud));
    win_.assign((size_t)L_, 0.f);
    reset();
}

void SymbolChain::reset() {
    std::fill(win_.begin(), win_.end(), 0.f);
    wpos_ = 0; wsum_ = 0;
    prev_ = 0; havePrev_ = false;
    dp_ = dp0_; ph_ = 0;
    nTrans_ = 0; nOut_ = 0;
}

} // namespace sondedsp
} // namespace dect2
