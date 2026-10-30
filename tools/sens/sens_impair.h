// Channel impairments for the sensitivity harness (tools/sens/sens_sweep.cpp). Everything is deterministic: every function takes the
// seeded generator it draws from, and Gaussian noise comes from PortableNormal (the same values with every standard library).
//
// SNR convention (docs/SENSITIVITY.md): SNR = Pa / (N0 * B), where Pa is the mean power of the ACTIVE part of the clean test signal
// (activePower(): blocks within 10 dB of the strongest, so a bursty mode's SNR is the burst's), B is the mode's noise bandwidth
// (Mode::bwHz, the channel width the standard quotes its C/N in) and N0 the white noise density. At sample rate fs the noise power per complex
// sample is therefore sigma^2 = Pa * (fs / B) / 10^(SNR/10).
#pragma once
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace sens {
using cf32 = std::complex<float>;
using Sig = std::vector<cf32>;

// splitmix64: a well mixed 64 bit value from any key; trials are seeded from (mode, scenario, snr, trial) so that results never depend on
// the order the threads run in.
inline uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
inline uint64_t hashStr(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

inline double meanPower(const cf32* x, size_t n) {
    double p = 0;
    for (size_t i = 0; i < n; i++) p += std::norm(x[i]);
    return n ? p / (double)n : 0;
}

// mean power of the blocks (512 samples) that are within 10 dB of the strongest 2 % of the blocks
inline double activePower(const Sig& x) {
    const size_t B = 512, nb = x.size() / B;
    if (nb < 4) return meanPower(x.data(), x.size());
    std::vector<double> bp(nb);
    for (size_t b = 0; b < nb; b++) bp[b] = meanPower(x.data() + b * B, B);
    std::vector<double> s = bp;
    std::sort(s.begin(), s.end(), std::greater<double>());
    const size_t top = std::max<size_t>(1, nb / 50);
    double ref = 0;
    for (size_t i = 0; i < top; i++) ref += s[i];
    ref /= (double)top;
    double sum = 0; size_t k = 0;
    for (double p : bp) if (p >= 0.1 * ref) { sum += p; k++; }
    return k ? sum / (double)k : ref;
}

inline void addNoise(Sig& x, double sigma2, std::mt19937& rng) {
    dect2::genutil::PortableNormal g((float)std::sqrt(sigma2 / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}

// carrier offset (a tuning error or an off-centre channel)
inline void shift(Sig& x, double hz, double rate, double phase0) {
    if (hz == 0 && phase0 == 0) return;
    const double w = 2 * M_PI * hz / rate;
    for (size_t o = 0; o < x.size(); o += 4096) {
        std::complex<double> r = std::polar(1.0, std::remainder(phase0 + w * (double)o, 2 * M_PI)), st = std::polar(1.0, w);
        const size_t e = std::min(x.size(), o + 4096);
        for (size_t i = o; i < e; i++) { x[i] = cf32(std::complex<double>(x[i]) * r); r *= st; }
    }
}

// sample clock error: the receiver gets (1 + ppm/1e6) times as many samples per second of signal. Windowed sinc interpolation, 24 taps (Kaiser,
// beta 9) at 1024 fractional positions: flat to 0.4 of the sample rate and 60 dB down above 0.55, because wide modes (ATSC 5.4 MHz at 8 Msps,
// DVB-T 7.6 MHz at 9.1 Msps) fill most of the band and a 4 point cubic (Catmull-Rom) interpolator damaged them by a time varying filter that moved
// with the fractional position: a clean ATSC signal with 10 ppm read 19 dB data SNR, a harness loss that looked like a receiver that cannot follow a clock.
inline Sig clockError(const Sig& x, double ppm) {
    if (ppm == 0 || x.size() < 64) return x;
    constexpr int H = 12, NPH = 1024;
    static const std::vector<float> tab = [] {
        auto i0 = [](double v) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (v / (2.0 * k)) * (v / (2.0 * k)); s += t; } return s; };
        const double beta = 9.0, den = i0(beta);
        std::vector<float> t((size_t)NPH * 2 * H);
        for (int p = 0; p < NPH; p++) {
            const double f = (double)p / NPH;
            double sum = 0; std::vector<double> w(2 * H);
            for (int k = -H + 1; k <= H; k++) {
                const double u = (double)k - f;                       // distance of tap k from the interpolation point
                const double r = u / H;
                const double win = std::fabs(r) < 1 ? i0(beta * std::sqrt(1 - r * r)) / den : 0.0;
                const double sinc = std::fabs(u) < 1e-12 ? 1.0 : std::sin(M_PI * u) / (M_PI * u);
                w[(size_t)(k + H - 1)] = sinc * win; sum += w[(size_t)(k + H - 1)];
            }
            for (int k = 0; k < 2 * H; k++) t[(size_t)p * 2 * H + k] = (float)(w[(size_t)k] / sum);
        }
        return t;
    }();
    const double step = 1.0 / (1.0 + ppm * 1e-6);
    Sig y;
    y.reserve((size_t)((double)x.size() / step) + 2);
    for (double t = H; t < (double)x.size() - H - 2; t += step) {
        const size_t i = (size_t)t;
        const int ph = std::min(NPH - 1, (int)((t - (double)i) * NPH + 0.5));
        const float* k = &tab[(size_t)ph * 2 * H];
        const cf32* p = &x[i - (H - 1)];
        float re = 0, im = 0;
        for (int j = 0; j < 2 * H; j++) { re += k[j] * p[j].real(); im += k[j] * p[j].imag(); }
        y.push_back(cf32(re, im));
    }
    return y;
}

// Wiener phase noise: a free running oscillator of the given 3 dB linewidth
inline void phaseNoise(Sig& x, double linewidthHz, double rate, std::mt19937& rng) {
    if (linewidthHz <= 0) return;
    dect2::genutil::PortableNormal g((float)std::sqrt(2 * M_PI * linewidthHz / rate));
    double ph = 0;
    for (auto& v : x) { ph += g(rng); v *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
}

struct Tap { double delayUs, powerDb; bool los; };

// Multipath profiles. static echo, and the 6 tap Typical Urban profile (COST 207 TU6: 0 .. 5 us), alone (Rayleigh) or with a line of sight (Rician).
inline std::vector<Tap> profileTaps(const std::string& name, double echoUs) {
    if (name == "echo") return {{0, 0, true}, {echoUs, -6, true}};
    static const double d[6] = {0, 0.2, 0.5, 1.6, 2.3, 5.0}, p[6] = {-3, 0, -2, -6, -8, -10};
    std::vector<Tap> t;
    for (int i = 0; i < 6; i++) t.push_back({d[i], p[i], false});
    if (name == "rician") t.push_back({0, 0, true});   // line of sight at 10 dB K factor, set in applyMultipath
    return t;
}

// Applies the profile. Fading taps move with a Jakes sum of 16 sinusoids at the Doppler frequency fdHz (gains evaluated every `knot` samples and
// interpolated). The total tap power is normalised to 1, so the mean signal power (and with it the SNR) is unchanged.
inline Sig multipath(const Sig& x, const std::vector<Tap>& taps, double rate, double fdHz, std::mt19937& rng) {
    if (taps.empty()) return x;
    const bool rician = taps.back().los && taps.size() > 2;
    const double K = std::pow(10.0, 1.0);
    std::vector<double> pw;
    double tot = 0;
    for (size_t i = 0; i < taps.size(); i++) {
        double p = std::pow(10.0, taps[i].powerDb / 10);
        if (rician) p = taps[i].los ? 0 : p;
        pw.push_back(p);
        tot += p;
    }
    for (auto& p : pw) p /= std::max(tot, 1e-12);
    if (rician) {   // total: K/(K+1) in the direct path, 1/(K+1) spread over the fading taps
        for (size_t i = 0; i < pw.size(); i++) pw[i] = taps[i].los ? K / (K + 1) : pw[i] / (K + 1);
    }
    struct Osc { double w, th; };
    const int NO = 16;
    std::vector<std::vector<Osc>> osc(taps.size());
    std::uniform_real_distribution<double> U(0, 2 * M_PI);
    for (size_t i = 0; i < taps.size(); i++) {
        if (taps[i].los) continue;
        for (int k = 0; k < NO; k++) osc[i].push_back({2 * M_PI * fdHz * std::cos(U(rng)), U(rng)});
    }
    const size_t knot = (size_t)std::min(65536.0, std::max(64.0, rate / (20 * std::max(fdHz, 1.0))));
    const size_t nk = x.size() / knot + 2;
    std::vector<std::vector<std::complex<float>>> g(taps.size(), std::vector<std::complex<float>>(nk));
    for (size_t i = 0; i < taps.size(); i++)
        for (size_t k = 0; k < nk; k++) {
            if (taps[i].los) { g[i][k] = (float)std::sqrt(pw[i]); continue; }
            const double t = (double)(k * knot) / rate;
            std::complex<double> s = 0;
            for (const auto& o : osc[i]) s += std::polar(1.0, o.w * t + o.th);
            g[i][k] = std::complex<float>(s / std::sqrt((double)NO) * std::sqrt(pw[i]));
        }
    std::vector<size_t> dl(taps.size());
    for (size_t i = 0; i < taps.size(); i++) dl[i] = (size_t)std::llround(taps[i].delayUs * 1e-6 * rate);
    Sig y(x.size());
    for (size_t n = 0; n < x.size(); n++) {
        const size_t k = n / knot;
        const float f = (float)(n - k * knot) / (float)knot;
        cf32 acc = 0;
        for (size_t i = 0; i < taps.size(); i++) {
            if (n < dl[i]) continue;
            const cf32 gg = g[i][k] * (1 - f) + g[i][k + 1] * f;
            acc += gg * x[n - dl[i]];
        }
        y[n] = acc;
    }
    return y;
}

} // namespace sens
