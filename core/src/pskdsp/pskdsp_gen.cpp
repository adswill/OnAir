// PSK demodulator test signals: frames with a known payload, any order, rate, pulse shape, offset, Doppler ramp, clock error and noise.
#include "dect2/pskdsp/pskdsp.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::pskd {

namespace {
constexpr double kPi = 3.14159265358979323846;
double pulse(double t, double a) {   // root-raised cosine (a > 0) or a rectangle one symbol long (a <= 0); t in symbols
    if (a <= 0) return t >= -0.5 && t < 0.5 ? 1.0 : 0.0;
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / kPi;
    if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-6)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    return (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) / (kPi * t * (1 - 16 * a * a * t * t));
}
cf point(int idx, Order o) {
    switch (o) {
    case Order::Bpsk: return cf(idx ? -1.f : 1.f, 0.f);
    case Order::Qpsk: case Order::Oqpsk: { const double a = kPi / 4 + kPi / 2 * idx; return cf((float)std::cos(a), (float)std::sin(a)); }
    case Order::Psk8: { const double a = kPi / 4 * idx; return cf((float)std::cos(a), (float)std::sin(a)); }
    default: return cf(1, 0);
    }
}
} // namespace

std::vector<cf> generate(const GenSpec& g0, GenTruth* truth) {
    GenSpec g = g0;
    if (g.order == Order::Auto) g.order = Order::Bpsk;
    std::mt19937 rng(g.seed);
    // the bit stream: sync + payload per frame
    Bits stream;
    std::vector<Bits> payloads;
    for (int f = 0; f < g.frames; f++) {
        Bits pl((size_t)std::max(0, g.payloadBits));
        for (auto& b : pl) b = (uint8_t)(rng() & 1);
        payloads.push_back(pl);
        stream.insert(stream.end(), g.sync.begin(), g.sync.end());
        if (g.scr == Scrambler::Ccsds) {
            const Bits seq = ccsdsSequence(pl.size());
            for (size_t i = 0; i < pl.size(); i++) pl[i] ^= seq[i];
        }
        stream.insert(stream.end(), pl.begin(), pl.end());
    }
    if (truth) { truth->payloads = payloads; truth->stream = stream; }
    Bits line = stream;
    if (g.scr == Scrambler::G3ruh) line = g3ruhScramble(line);
    if (g.diff == Diff::NrzM || g.diff == Diff::NrzS) line = diffEncode(line, g.diff);
    // symbols (a short random lead-in so the loops settle before the first sync word)
    const int nb = orderBits(g.order), M = orderPoints(g.order);
    std::vector<int> idx;
    for (int i = 0; i < 64; i++) idx.push_back((int)(rng() % (unsigned)M));
    int prev = idx.back();
    for (size_t i = 0; i + (size_t)nb <= line.size(); i += (size_t)nb) {
        int k = bitsSymbol(&line[i], g.order);
        if (g.diff == Diff::Dpsk) { k = (prev + k) % M; }
        idx.push_back(k);
        prev = k;
    }
    for (int i = 0; i < 32; i++) idx.push_back((int)(rng() % (unsigned)M));
    std::vector<cf> sym(idx.size());
    for (size_t i = 0; i < idx.size(); i++) sym[i] = point(idx[i], g.order);
    // pulse shaping at the transmitter's (possibly wrong) clock
    const double T = g.fs / g.baud * (1 + g.clockPpm * 1e-6);   // samples per symbol
    const int span = g.alpha > 0 ? 8 : 1;
    const bool oq = g.order == Order::Oqpsk;
    const size_t nOut = (size_t)std::ceil((double)sym.size() * T + 2 * span * T);
    std::vector<cf> y(nOut, cf(0, 0));
    for (size_t k = 0; k < sym.size(); k++) {
        const double t0 = ((double)k + span) * T;   // centre of symbol k
        for (int part = 0; part < (oq ? 2 : 1); part++) {
            const double tc = t0 + (part == 1 ? T / 2 : 0);
            const float vi = oq ? (part == 0 ? sym[k].real() : 0.f) : sym[k].real();
            const float vq = oq ? (part == 1 ? sym[k].imag() : 0.f) : sym[k].imag();
            const long a = (long)std::floor(tc - span * T), b = (long)std::ceil(tc + span * T);
            for (long n = std::max(0L, a); n <= b && n < (long)nOut; n++) {
                const float h = (float)pulse(((double)n - tc) / T, g.alpha);
                y[(size_t)n] += cf(vi * h, vq * h);
            }
        }
    }
    // carrier: offset, Doppler ramp, initial phase
    double sp = 0;
    for (const cf& v : y) sp += std::norm(v);
    sp /= (double)std::max<size_t>(1, y.size());
    double ph = g.phase0;
    for (size_t n = 0; n < nOut; n++) {
        const double t = (double)n / g.fs;
        const double f = g.offsetHz + g.dopplerHzPerSec * t;
        y[n] *= cf((float)std::cos(ph), (float)std::sin(ph));
        ph += 2 * kPi * f / g.fs;
        if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
    }
    // noise for Es/N0 in the symbol rate's bandwidth: N0 = sigma^2 / fs, Es = power x symbol time
    if (g.esn0Db < 200) {
        const double esn0 = std::pow(10.0, g.esn0Db / 10);
        const double sigma2 = sp * (g.fs / g.baud) / esn0;
        std::normal_distribution<float> nd(0.f, (float)std::sqrt(sigma2 / 2));
        for (auto& v : y) v += cf(nd(rng), nd(rng));
    }
    return y;
}

} // namespace dect2::pskd
