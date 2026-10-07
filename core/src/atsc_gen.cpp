#include "dect2/atsc_gen.h"
#include "dect2/dsp_compat.h"
#include "dect2/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <utility>
#include <vector>

namespace dect2 {
namespace atsc {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kBeta = 0.1152;
}

// Root-raised-cosine impulse response for the baud rate Rb = symbol rate / 2 (Nyquist frequency Rs/4 = 2.69 MHz), H(0) = 1
double rrcValue(double t) {
    const double Rb = kSymbolRate / 2.0;
    const double x = t * Rb;   // t / Ts
    if (std::fabs(x) < 1e-9) return 1.0 - kBeta + 4.0 * kBeta / kPi;
    const double den4 = 4.0 * kBeta * x;
    if (std::fabs(std::fabs(den4) - 1.0) < 1e-9) {
        const double s = kBeta / std::sqrt(2.0);
        return ((1 + 2 / kPi) * std::sin(kPi / (4 * kBeta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * kBeta))) * s;
    }
    const double num = std::sin(kPi * x * (1 - kBeta)) + 4 * kBeta * x * std::cos(kPi * x * (1 + kBeta));
    const double den = kPi * x * (1 - den4 * den4);
    return num / den;
}

std::vector<float> rrcTaps(double rate, int halfLen) {
    std::vector<float> t(2 * halfLen + 1);
    const double Rb = kSymbolRate / 2.0;
    for (int i = -halfLen; i <= halfLen; i++) t[i + halfLen] = (float)(rrcValue(i / rate));
    (void)Rb;
    return t;
}

Generator::Generator(PacketSource src, const ChannelConfig& cfg, double outRate, unsigned seed)
    : src_(std::move(src)), cfg_(cfg), outRate_(outRate), noise_(seed) {
    const double fs2 = 2.0 * kSymbolRate;
    rrc_ = rrcTaps(fs2, 96);   // +-96 samples = +-48 symbols
    hiRe_.assign(48, 0.f);     // the samples before the start are zero
    hiIm_.assign(48, 0.f);
    hiBase_ = -48;
    // output resampler: windowed sinc, cut-off just above the channel
    const int J = 48;
    const int NPH = 256;
    sinc_.resize((size_t)NPH * 2 * J);
    const double fc = 0.46 * outRate / fs2;   // cut-off in cycles per hi sample
    for (int p = 0; p < NPH; p++) {
        const double mu = (double)p / NPH;
        double sum = 0;
        for (int j = -J + 1; j <= J; j++) {
            const double x = j - mu;
            const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            const double w = 0.5 + 0.5 * std::cos(kPi * x / (J + 0.5));
            const double v = s * (std::fabs(x) <= J ? w : 0);
            sinc_[(size_t)p * 2 * J + (j + J - 1)] = (float)v;
            sum += v;
        }
        for (int j = 0; j < 2 * J; j++) sinc_[(size_t)p * 2 * J + j] /= (float)sum;
    }
    noiseSigma_ = -1;
}

void Generator::makeField() {
    constexpr int K = kFieldSyms, Kh = K / 2, E = 49, HistH = 64;   // symbols per field, per parity; taps per polyphase branch; history per parity
    std::vector<uint8_t> ts((size_t)kDataSegs * kTsBytes), sym(kFieldSyms);
    for (int p = 0; p < kDataSegs; p++) { uint8_t* q = &ts[(size_t)p * kTsBytes]; src_(q); q[0] = 0x47; }
    enc_.encode(ts.data(), fields_ & 1, sym.data());
    // Channel-centred baseband: the symbols (+ pilot) rotated by (-j)^k and shaped by the real RRC pulse at 2 samples/symbol:
    //   X[2k' + p] = (-j)^k' * sum_d a[k'-d] j^d rrc[2d + p]
    // The factor j^d is real for even d and imaginary for odd d, so the real part is a plain FIR over the even-d taps and the
    // imaginary part one over the odd-d taps. Each output is two correlations of 49 taps with the even or the odd symbols.
    if (shapeTaps_.empty()) {
        shapeTaps_.assign((size_t)4 * E, 0.f);   // [p][re/im][E], reversed for the correlation
        for (int p = 0; p < 2; p++)
            for (int part = 0; part < 2; part++)
                for (int e = 0; e < E; e++) {
                    const size_t m = (size_t)4 * e + 2 * part + p;
                    const float v = m < rrc_.size() ? rrc_[m] : 0.f;
                    shapeTaps_[((size_t)p * 2 + part) * E + (E - 1 - e)] = (e & 1) ? -v : v;
                }
        aE_.assign((size_t)Kh + HistH, 0.f);
        aO_.assign((size_t)Kh + HistH, 0.f);
        xs_.resize((size_t)4 * K);
        tmp_.resize((size_t)8 * Kh);
    }
    // the last symbols of the previous field become the history of this one
    for (int t = 0; t < HistH; t++) { aE_[t] = aE_[(size_t)Kh + t]; aO_[t] = aO_[(size_t)Kh + t]; }
    for (int m = 0; m < Kh; m++) { aE_[(size_t)HistH + m] = levelOf(sym[2 * m]) + 1.25f; aO_[(size_t)HistH + m] = levelOf(sym[2 * m + 1]) + 1.25f; }
    // eight correlations (even/odd symbol position, two samples per symbol, real and imaginary part), then interleaved into samples
    for (int q = 0; q < 2; q++)
        for (int p = 0; p < 2; p++) {
            const float* fRe = &shapeTaps_[((size_t)p * 2 + 0) * E];
            const float* fIm = &shapeTaps_[((size_t)p * 2 + 1) * E];
            convCorr((q == 0 ? aE_.data() : aO_.data()) + 16, fRe, &tmp_[(size_t)(4 * q + 2 * p) * Kh], 1, Kh, E);
            convCorr((q == 0 ? aO_.data() + 15 : aE_.data() + 16), fIm, &tmp_[(size_t)(4 * q + 2 * p + 1) * Kh], 1, Kh, E);
        }
    for (int c = 0; c < 8; c++) {
        const float* src = &tmp_[(size_t)c * Kh];
        float* dst = xs_.data() + c;
        for (int m = 0; m < Kh; m++) dst[(size_t)8 * m] = src[m];
    }
    symIndex_ += kFieldSyms;
    fields_++;
    // the rotation (-j)^k': blocks of four symbols = 16 floats (kFieldSyms is a multiple of 4, so every field starts at phase 0)
    const size_t nHi = (size_t)kFieldSyms * 2;
    {
        float* x = xs_.data();
        for (size_t g = 0; g < nHi / 8; g++, x += 16) {
            for (int l = 4; l < 8; l += 2) { const float r = x[l], i = x[l + 1]; x[l] = i; x[l + 1] = -r; }          // times -j
            for (int l = 8; l < 12; l++) x[l] = -x[l];                                                              // times -1
            for (int l = 12; l < 16; l += 2) { const float r = x[l], i = x[l + 1]; x[l] = -i; x[l + 1] = r; }       // times +j
        }
    }
    cf32* emit = reinterpret_cast<cf32*>(xs_.data());
    // multipath: the main path is delayed by the largest pre-echo, every echo adds its own delay on top
    const double fs2 = 2.0 * kSymbolRate;
    double maxPre = 0;
    for (auto& e : cfg_.echoes) maxPre = std::max(maxPre, -e.delayUs);
    const int preS = (int)std::lround(maxPre * 1e-6 * fs2);
    struct Path { int delay; cf32 g; };
    std::vector<Path> paths = {{preS, cf32(1, 0)}};
    for (auto& e : cfg_.echoes) {
        const float a = (float)std::pow(10.0, e.gainDb / 20);
        paths.push_back({preS + (int)std::lround(e.delayUs * 1e-6 * fs2), a * cf32((float)std::cos(e.phaseDeg * kPi / 180), (float)std::sin(e.phaseDeg * kPi / 180))});
    }
    const size_t histLen = 4096;
    if (echoHist_.size() != histLen) echoHist_.assign(histLen, cf32(0, 0));
    cf32* y = emit;
    if (paths.size() == 1 && paths[0].delay == 0) {   // no echo: the field passes straight through
        echoHist_.assign(emit + nHi - histLen, emit + nHi);
    } else {
        std::vector<cf32> ext(echoHist_);
        ext.insert(ext.end(), emit, emit + nHi);
        yBuf_.assign(nHi, cf32(0, 0));
        for (size_t n = 0; n < nHi; n++) {
            const size_t p = n + histLen;
            cf32 v(0, 0);
            for (auto& pa : paths) v += ext[p - pa.delay] * pa.g;
            yBuf_[n] = v;
        }
        echoHist_.assign(ext.end() - histLen, ext.end());
        y = yBuf_.data();
    }
    // carrier offset: an oscillator restarted from the exact phase every 64 samples
    if (cfg_.cfoHz != 0) {
        const double dph = 2 * kPi * cfg_.cfoHz / fs2;
        const cf32 stp((float)std::cos(dph), (float)std::sin(dph));
        for (size_t n0 = 0; n0 < nHi; n0 += 64) {
            cf32 r((float)std::cos(cfoPhase_), (float)std::sin(cfoPhase_));
            const size_t n1 = std::min(nHi, n0 + 64);
            for (size_t n = n0; n < n1; n++) { y[n] *= r; r *= stp; }
            cfoPhase_ = std::remainder(cfoPhase_ + dph * (double)(n1 - n0), 2 * kPi);
        }
    }
    // noise: SNR is data power over noise in 5.38 MHz
    if (noiseSigma_ < 0 && cfg_.snrDb < 90) {
        double pw = 0;
        for (size_t n = 0; n < nHi; n++) pw += std::norm(y[n]);
        pw /= (double)nHi;
        const double pdata = pw * 21.0 / (21.0 + 1.5625);
        const double n0 = pdata / std::pow(10.0, cfg_.snrDb / 10.0) / (kSymbolRate / 2.0);
        noiseSigma_ = std::sqrt(n0 * fs2 / 2.0);   // per real component
    }
    if (cfg_.snrDb < 90 && noiseSigma_ > 0) noise_.add(y, nHi, (float)noiseSigma_);
    // planar for the output resampler
    const size_t base = hiRe_.size();
    hiRe_.resize(base + nHi);
    hiIm_.resize(base + nHi);
    float* hr = hiRe_.data() + base;
    float* hi = hiIm_.data() + base;
    for (size_t n = 0; n < nHi; n++) { hr[n] = y[n].real(); hi[n] = y[n].imag(); }
}

void Generator::generate(size_t n, std::vector<cf32>& out) {
    const double fs2 = 2.0 * kSymbolRate;
    const double step = fs2 / outRate_ / (1.0 + cfg_.sroPpm * 1e-6);   // hi samples per output sample
    const int J = 48;
    const int NPH = 256;
    out.reserve(out.size() + n);
    for (size_t k = 0; k < n; k++) {
        const double pos = outPos_;
        const int64_t i0 = (int64_t)std::floor(pos);
        while (i0 + J + 2 >= hiBase_ + (int64_t)hiRe_.size()) makeField();
        const double mu = pos - (double)i0;
        const int ph = std::min(NPH - 1, (int)(mu * NPH));
        const float* tap = &sinc_[(size_t)ph * 2 * J];
        const size_t b0 = (size_t)(i0 - J + 1 - hiBase_);
        float s[2];
        genutil::dot2(tap, &hiRe_[b0], &hiIm_[b0], 2 * J, s);
        out.push_back(cf32(s[0], s[1]));
        outPos_ += step;
        // drop samples that are no longer needed
        if (hiRe_.size() > 4 * (size_t)kFieldSyms * 2 && i0 > hiBase_ + (int64_t)kFieldSyms * 2) {
            const size_t drop = (size_t)kFieldSyms * 2;
            hiRe_.erase(hiRe_.begin(), hiRe_.begin() + (long)drop);
            hiIm_.erase(hiIm_.begin(), hiIm_.begin() + (long)drop);
            hiBase_ += (int64_t)drop;
        }
    }
}

} // namespace atsc
} // namespace dect2
