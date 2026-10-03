#include "dect2/atsc_gen.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <random>
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
    : src_(std::move(src)), cfg_(cfg), outRate_(outRate), rng_(seed) {
    const double fs2 = 2.0 * kSymbolRate;
    rrc_ = rrcTaps(fs2, 96);   // +-96 samples = +-48 symbols
    acc_.assign(rrc_.size(), cf32(0, 0));
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
    std::vector<uint8_t> ts((size_t)kDataSegs * kTsBytes), sym(kFieldSyms);
    for (int p = 0; p < kDataSegs; p++) { uint8_t* q = &ts[(size_t)p * kTsBytes]; src_(q); q[0] = 0x47; }
    enc_.encode(ts.data(), fields_ & 1, sym.data());
    // channel-centred baseband: symbols (+ pilot) rotated by (-j)^k, shaped by the real RRC at 2 samples/symbol
    const size_t nHi = (size_t)kFieldSyms * 2;
    std::vector<cf32> x(nHi + rrc_.size(), cf32(0, 0));
    for (size_t i = 0; i < acc_.size() && i < x.size(); i++) x[i] = acc_[i];
    static const cf32 rot[4] = {cf32(1, 0), cf32(0, -1), cf32(-1, 0), cf32(0, 1)};
    for (int k = 0; k < kFieldSyms; k++) {
        const float a = levelOf(sym[k]) + 1.25f;
        const cf32 b = rot[(symIndex_ + k) & 3] * a;
        const int c = 2 * k;                  // centre of this symbol's pulse inside x (offset by `half` at the start)
        for (int m = 0; m < (int)rrc_.size(); m++) {
            x[c + m] += b * rrc_[m];          // the stream is delayed by half a pulse; the tail past the field goes to acc_
        }
    }
    // the first `half` samples of the pulses of the first symbols reach before the field start; they were already emitted:
    // account for that with a one-field latency (the tail below carries the late part only)
    std::vector<cf32> emit(x.begin(), x.begin() + nHi);
    acc_.assign(x.begin() + nHi, x.begin() + nHi + rrc_.size());
    symIndex_ += kFieldSyms;
    fields_++;
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
    std::vector<cf32> ext(echoHist_);
    ext.insert(ext.end(), emit.begin(), emit.end());
    std::vector<cf32> y(emit.size(), cf32(0, 0));
    for (size_t n = 0; n < emit.size(); n++) {
        const size_t p = n + histLen;
        cf32 v(0, 0);
        for (auto& pa : paths) v += ext[p - pa.delay] * pa.g;
        y[n] = v;
    }
    echoHist_.assign(ext.end() - histLen, ext.end());
    // carrier offset
    const double dph = 2 * kPi * cfg_.cfoHz / fs2;
    for (size_t n = 0; n < y.size(); n++) {
        y[n] *= cf32((float)std::cos(cfoPhase_), (float)std::sin(cfoPhase_));
        cfoPhase_ += dph;
        if (cfoPhase_ > kPi) cfoPhase_ -= 2 * kPi;
    }
    // noise: SNR is data power over noise in 5.38 MHz
    if (noiseSigma_ < 0 && cfg_.snrDb < 90) {
        double pw = 0;
        for (auto& v : y) pw += std::norm(v);
        pw /= y.size();
        const double pdata = pw * 21.0 / (21.0 + 1.5625);
        const double n0 = pdata / std::pow(10.0, cfg_.snrDb / 10.0) / (kSymbolRate / 2.0);
        noiseSigma_ = std::sqrt(n0 * fs2 / 2.0);   // per real component
    }
    if (cfg_.snrDb < 90 && noiseSigma_ > 0) {
        std::normal_distribution<float> nd(0.f, (float)noiseSigma_);
        for (auto& v : y) v += cf32(nd(rng_), nd(rng_));
    }
    hi_.insert(hi_.end(), y.begin(), y.end());
}

void Generator::generate(size_t n, std::vector<cf32>& out) {
    const double fs2 = 2.0 * kSymbolRate;
    const double step = fs2 / outRate_ / (1.0 + cfg_.sroPpm * 1e-6);   // hi samples per output sample
    const int J = 48;
    const int NPH = 256;
    for (size_t k = 0; k < n; k++) {
        const double pos = outPos_;
        const int64_t i0 = (int64_t)std::floor(pos);
        while ((uint64_t)(i0 + J + 2) >= hiBase_ + hi_.size() + 0) makeField();
        const double mu = pos - (double)i0;
        const int ph = std::min(NPH - 1, (int)(mu * NPH));
        const float* tap = &sinc_[(size_t)ph * 2 * J];
        cf32 acc(0, 0);
        for (int j = -J + 1; j <= J; j++) {
            const int64_t idx = i0 + j - (int64_t)hiBase_;
            if (idx < 0) continue;
            acc += hi_[(size_t)idx] * tap[j + J - 1];
        }
        out.push_back(acc);
        outPos_ += step;
        // drop samples that are no longer needed
        if (hi_.size() > 4 * (size_t)kFieldSyms * 2 && (uint64_t)i0 > hiBase_ + (uint64_t)kFieldSyms * 2) {
            const size_t drop = (size_t)kFieldSyms * 2;
            hi_.erase(hi_.begin(), hi_.begin() + drop);
            hiBase_ += drop;
        }
    }
}

} // namespace atsc
} // namespace dect2
