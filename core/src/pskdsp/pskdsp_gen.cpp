// PSK test signals with a known payload: any rate, carrier offset, Doppler ramp, symbol clock error and noise (tests/pskdsp).
#include "dect2/pskdsp/pskdsp.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::pskd {

namespace {
constexpr double kPi = 3.14159265358979323846;
double rrcT(double t, double a) {   // RRC pulse, t in symbols
    if (a <= 0) return (t >= 0 && t < 1) ? 1.0 : 0.0;
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / kPi;
    if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-6)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    return (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) / (kPi * t * (1 - 16 * a * a * t * t));
}
cf point(int idx, Order o) {
    switch (o) {
    case Order::Bpsk: return cf(idx ? -1.f : 1.f, 0);
    case Order::Qpsk: case Order::Oqpsk: { const double a = kPi / 4 + idx * kPi / 2; return cf((float)std::cos(a), (float)std::sin(a)); }
    default: { const double a = idx * kPi / 4; return cf((float)std::cos(a), (float)std::sin(a)); }
    }
}
} // namespace

std::vector<cf> generate(const GenSpec& g, GenTruth* truth) {
    std::mt19937 rng(g.seed);
    const int nb = orderBits(g.order), M = orderPoints(g.order);
    const Bits seq = ccsdsSequence(1 << 16);
    Bits stream;   // logical bits as the receiver should output them
    Bits tx;       // after the randomiser (before the G3RUH scrambler and differential coding)
    GenTruth tr;
    for (int f = 0; f < g.frames; f++) {
        Bits pay((size_t)g.payloadBits);
        for (auto& b : pay) b = (uint8_t)(rng() & 1);
        tr.payloads.push_back(pay);
        stream.insert(stream.end(), g.sync.begin(), g.sync.end());
        stream.insert(stream.end(), pay.begin(), pay.end());
        tx.insert(tx.end(), g.sync.begin(), g.sync.end());
        for (size_t i = 0; i < pay.size(); i++) tx.push_back((uint8_t)(pay[i] ^ (g.scr == Scrambler::Ccsds ? seq[i % seq.size()] : 0)));
    }
    tr.stream = stream;
    if (g.scr == Scrambler::G3ruh) tx = g3ruhScramble(tx);
    while (tx.size() % (size_t)nb) tx.push_back(0);
    if (g.diff == Diff::NrzM || g.diff == Diff::NrzS) tx = diffEncode(tx, g.diff);
    std::vector<int> idx(tx.size() / (size_t)nb);
    int acc = 0;
    for (size_t k = 0; k < idx.size(); k++) {
        int v = bitsSymbol(tx.data() + k * (size_t)nb, g.order);
        if (g.diff == Diff::Dpsk) { acc = (acc + v) % M; v = acc; }
        idx[k] = v;
    }
    const double T = g.fs / g.baud * (1.0 + g.clockPpm * 1e-6);   // samples per symbol
    const int span = g.alpha > 0 ? 8 : 1;
    const int lead = 12;
    const size_t N = (size_t)((double)(idx.size() + 2 * (size_t)lead) * T) + 16;
    std::vector<cf> out(N);
    const bool oq = g.order == Order::Oqpsk;
    std::vector<cf> pts(idx.size());
    for (size_t k = 0; k < idx.size(); k++) pts[k] = point(idx[k], g.order);
    double pw = 0; size_t pn = 0;
    for (size_t i = 0; i < N; i++) {
        const double t = (double)i / T - lead;   // in symbols
        float re = 0, im = 0;
        const long k0 = (long)std::floor(t);
        for (long k = k0 - span; k <= k0 + span; k++) {
            if (k < 0 || k >= (long)idx.size()) continue;
            if (g.alpha > 0) {
                re += pts[(size_t)k].real() * (float)rrcT(t - (double)k, g.alpha);
                im += pts[(size_t)k].imag() * (float)rrcT(t - (double)k - (oq ? 0.5 : 0.0), g.alpha);
            } else {
                re += pts[(size_t)k].real() * (float)rrcT(t - (double)k, 0);
                im += pts[(size_t)k].imag() * (float)rrcT(t - (double)k - (oq ? 0.5 : 0.0), 0);
            }
        }
        out[i] = cf(re, im);
        if (t > 4 && t < (double)idx.size() - 4) { pw += std::norm(out[i]); pn++; }
    }
    pw = pn ? pw / (double)pn : 1;
    // carrier
    for (size_t i = 0; i < N; i++) {
        const double ts = (double)i / g.fs;
        const double ph = 2 * kPi * (g.offsetHz * ts + 0.5 * g.dopplerHzPerSec * ts * ts) + g.phase0;
        out[i] *= cf((float)std::cos(ph), (float)std::sin(ph));
    }
    // noise: Es/N0 = P fs / (baud sigma^2)
    const double es = std::pow(10.0, g.esn0Db / 10.0);
    const double sig = std::sqrt(pw * g.fs / (g.baud * es) / 2.0);
    std::normal_distribution<double> nd(0.0, 1.0);
    for (auto& v : out) v += cf((float)(sig * nd(rng)), (float)(sig * nd(rng)));
    if (truth) *truth = tr;
    return out;
}

} // namespace dect2::pskd
