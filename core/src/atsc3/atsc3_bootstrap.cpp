#include "dect2/common/stage_prof.h"
#include "dect2/atsc3/atsc3_bootstrap.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>

namespace dect2 {
namespace atsc3 {

namespace {

constexpr int kN = kBootstrapFft;
constexpr int kNzc = 1499;
constexpr int kNh = (kNzc - 1) / 2;      // 749
constexpr int kLenB = 504, kLenC = 520;
constexpr int kZcRoot = 137;             // major version 0
constexpr int kSeeds[8] = {0x019D, 0x00ED, 0x01E8, 0x00E8, 0x00FB, 0x0021, 0x0054, 0x00EC};
constexpr int kMaxSymbols = 8;

// Pseudo-noise bits of A/321 5.2.2: 16-bit shift register, polynomial x^16 + x^15 + x^14 + x + 1, output taken before the first clock. The
// taps (g0, g1, g14, g15) are summed into the input of the register as it shifts right (a Fibonacci register: checked against a recording of a
// DekTec modulator, a Galois register with the same polynomial gives another sequence).
std::vector<uint8_t> pnBits(int seed, int count) {
    std::vector<uint8_t> p(count);
    unsigned r = (unsigned)seed & 0xFFFF;
    for (int i = 0; i < count; i++) {
        p[i] = r & 1;
        const unsigned fb = (r ^ (r >> 1) ^ (r >> 14) ^ (r >> 15)) & 1;
        r = (r >> 1) | (fb << 15);
    }
    return p;
}

// Frequency-domain values of bootstrap symbol n (before the sign flip of the last symbol): Zadoff-Chu sequence times the PN sequence,
// mirrored around the DC carrier, mapped to the 2048 IFFT bins.
void freqSymbol(const std::vector<uint8_t>& pn, int n, std::vector<cf32>& X) {
    X.assign(kN, cf32(0, 0));
    for (int k = -kNh; k <= kNh; k++) {
        if (k == 0) continue;   // the DC carrier is null
        int idx = k + kNh;
        double ph = -M_PI * kZcRoot * (double)idx * (idx + 1) / kNzc;
        int pi = k < 0 ? (n + 1) * kNh + k : (n + 1) * kNh - k;
        float c = pn[pi] ? -1.f : 1.f;
        X[(k + kN) % kN] = cf32((float)std::cos(ph) * c, (float)std::sin(ph) * c);
    }
}

// Relative cyclic shift for 8 signalling bits b[0..7] (b[0] is the most significant), with the Gray code of A/321 5.3.2.
int relativeShift(const int* b, int nb) {
    int m = 0;
    for (int i = 0; i <= 10; i++) {
        int bit;
        if (i > 10 - nb) { int s = 0; for (int k = 0; k <= 10 - i; k++) s ^= b[k]; bit = s & 1; }
        else if (i == 10 - nb) bit = 1;
        else bit = 0;
        m |= bit << i;
    }
    return m;
}

void timeSymbol(Fft& fft, const std::vector<cf32>& X, int shift, bool negate, bool cab, cf32* out) {
    std::vector<cf32> a(kN);
    for (int i = 0; i < kN; i++) a[i] = negate ? -X[i] : X[i];
    fft.inverse(a.data());
    std::vector<cf32> A(kN);
    for (int t = 0; t < kN; t++) A[t] = a[(t + shift) % kN];
    auto rot = [](double turns) { return cf32((float)std::cos(2 * M_PI * turns), (float)std::sin(2 * M_PI * turns)); };
    const int L = kBootstrapSymbolLen;
    for (int i = 0; i < L; i++) {
        if (cab) {
            if (i < kLenC) out[i] = A[i + 1528];
            else if (i < kLenC + kN) out[i] = A[i - kLenC];
            else out[i] = A[i - 1024] * rot((double)i / kN);
        } else {
            if (i < kLenB) out[i] = A[i + 1528] * rot(-(double)(i - kLenC) / kN);
            else if (i < 1024) out[i] = A[i + 1024];
            else out[i] = A[i - 1024];
        }
    }
}

std::vector<int> fieldBits(const Bootstrap& b, int symbol) {
    std::vector<int> v(8, 0);
    auto put = [&](int from, int count, int value) { for (int i = 0; i < count; i++) v[from + i] = (value >> (count - 1 - i)) & 1; };
    if (symbol == 1) { put(0, 1, (b.eaWakeUp >> 1) & 1); put(1, 5, b.minTimeToNext); put(6, 2, b.systemBandwidth); }
    else if (symbol == 2) { put(0, 1, b.eaWakeUp & 1); put(1, 7, b.bsrCoefficient); }
    else if (symbol == 3) put(0, 8, b.preambleStructure);
    return v;
}

int bitsToInt(const int* b, int n) { int v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[i]; return v; }

} // namespace

double postBootstrapRate(const Bootstrap& b) { return (b.bsrCoefficient + 16) * 384000.0; }

int minTimeToNextMs(int x) {
    if (x < 8) return 50 * x + 50;
    if (x < 16) return 100 * (x - 8) + 500;
    if (x < 24) return 200 * (x - 16) + 1300;
    return 400 * (x - 24) + 2900;
}

double bandwidthHz(int bw) { return bw == 0 ? 6e6 : bw == 1 ? 7e6 : bw == 2 ? 8e6 : 0; }

std::vector<cf32> generateBootstrap(const Bootstrap& b) {
    int ns = std::max(1, std::min(kMaxSymbols, b.numSymbols));
    auto pn = pnBits(kSeeds[b.minorVersion & 7], (kMaxSymbols + 1) * kNh);
    Fft fft(kN);
    std::vector<cf32> out((size_t)ns * kBootstrapSymbolLen), X;
    // the inverse FFT is a plain sum over the carriers; scale to unit power per sample
    float scale = 1.f / std::sqrt((float)(kNzc - 1));
    int absShift = 0;
    for (int n = 0; n < ns; n++) {
        freqSymbol(pn, n, X);
        for (auto& v : X) v *= scale;
        if (n > 0) {
            int bits[8];
            auto f = fieldBits(b, n);
            for (int i = 0; i < 8; i++) bits[i] = f[i];
            int rel = n <= 3 ? relativeShift(bits, 8) : relativeShift(bits, 0);
            absShift = (absShift + rel) % kN;
        }
        timeSymbol(fft, X, absShift, n == ns - 1, n == 0, out.data() + (size_t)n * kBootstrapSymbolLen);
    }
    return out;
}

namespace {
// The spectrum of the first bootstrap symbol of a seed, conjugated, for the correlation in blocks of F samples. It depends on nothing but the seed and
// the block size, and the search asks for it for every hypothesis of every candidate: it is made once.
struct SeedTemplate {
    std::once_flag once;
    std::vector<cf32> tf;
    double energy = 0;
};

const SeedTemplate& seedTemplate(int seed, int F) {
    static SeedTemplate cache[2][8];
    SeedTemplate& t = cache[F > 32768 ? 1 : 0][seed];
    std::call_once(t.once, [&] {
        Fft fft(kN), big(F);
        std::vector<cf32> X, sym((size_t)kBootstrapSymbolLen);
        freqSymbol(pnBits(kSeeds[seed], (kMaxSymbols + 1) * kNh), 0, X);
        const float sc = 1.f / std::sqrt((float)(kNzc - 1));
        for (auto& v : X) v *= sc;
        timeSymbol(fft, X, 0, false, true, sym.data());
        t.tf.assign((size_t)F, cf32(0, 0));
        for (int i = 0; i < kBootstrapSymbolLen; i++) { t.tf[(size_t)i] = sym[(size_t)i]; t.energy += std::norm(sym[(size_t)i]); }
        big.forward(t.tf.data());
        for (auto& v : t.tf) v = std::conj(v);
    });
    return t;
}
} // namespace

Detection detectBootstrap(const cf32* x, size_t n, int seedOnly) {
    Detection d;
    const int L = kBootstrapSymbolLen;
    if (n < (size_t)4 * L) return d;
    Fft fft(kN);
    const int s0 = seedOnly >= 0 && seedOnly < 8 ? seedOnly : 0, s1 = seedOnly >= 0 && seedOnly < 8 ? seedOnly + 1 : 8;
    // normalised correlation of the first symbol against every position (blocks with overlap, FFT based); a block as short as the data when it fits
    const int F = n <= 32768 ? 32768 : 65536;
    Fft big(F);
    std::vector<const SeedTemplate*> tpl(8, nullptr);
    for (int s = s0; s < s1; s++) tpl[(size_t)s] = &seedTemplate(s, F);
    const double Et = tpl[(size_t)s0]->energy;
    std::vector<double> csum(n + 1, 0.0);
    for (size_t i = 0; i < n; i++) csum[i + 1] = csum[i] + std::norm(x[i]);

    long bestPos = -1; int bestSeed = 0; double bestM = 0;
    size_t lastStart = n - (size_t)4 * L;   // keep room for four symbols
    std::vector<cf32> blk(F), prod(F);
#if defined(__x86_64__) || defined(_M_X64)
    // x86: the products written out (the complex operator's NaN check keeps that loop scalar) and the metric compared squared (std::abs of a
    // complex is a hypotf() call in libm there): the same products, the same order of the metric
    constexpr bool kX86 = true;
#else
    constexpr bool kX86 = false;
#endif
    double bestM2 = 0;
    for (size_t p0 = 0; p0 <= lastStart; p0 += F - L) {
        size_t m = std::min<size_t>(F, n - p0);
        std::fill(blk.begin(), blk.end(), cf32(0, 0));
        for (size_t i = 0; i < m; i++) blk[i] = x[p0 + i];
        big.forward(blk.data());
        for (int s = s0; s < s1; s++) {
            const cf32* tf = tpl[(size_t)s]->tf.data();
            if (kX86) {
                const float* a = reinterpret_cast<const float*>(blk.data());
                const float* b = reinterpret_cast<const float*>(tf);
                float* o = reinterpret_cast<float*>(prod.data());
                for (int i = 0; i < F; i++) {
                    const float ar = a[2 * i], ai = a[2 * i + 1], br = b[2 * i], bi = b[2 * i + 1];
                    o[2 * i] = ar * br - ai * bi; o[2 * i + 1] = ar * bi + ai * br;
                }
            } else
                for (int i = 0; i < F; i++) prod[i] = blk[i] * tf[i];
            big.inverse(prod.data());   // sum over e^{+j}: with the forward transforms above this is F times the cross-correlation
            const double ff = (double)F * F;
            for (int i = 0; i + L <= (int)m && i < F - L + 1 && p0 + i <= lastStart; i++) {
                double ew = csum[p0 + i + L] - csum[p0 + i];
                if (ew <= 0) continue;
                if (kX86) {
                    const double pr = prod[i].real(), pi = prod[i].imag();
                    const double m2 = (pr * pr + pi * pi) / ff / (Et * ew);
                    if (m2 > bestM2) { bestM2 = m2; bestPos = (long)(p0 + i); bestSeed = s; }
                    continue;
                }
                double met = std::abs(prod[i]) / F / std::sqrt(Et * ew);
                if (met > bestM) { bestM = met; bestPos = (long)(p0 + i); bestSeed = s; }
            }
        }
    }
    if (kX86) bestM = std::sqrt(bestM2);
    d.metric = (float)bestM;
    if (bestM < 0.2 || bestPos < 0) return d;
    d.found = true;
    d.start = bestPos;
    d.info.minorVersion = bestSeed;

    // read the symbols: the cyclic shift of each is found by correlating its spectrum with the known one
    const std::vector<std::vector<uint8_t>> pns{pnBits(kSeeds[bestSeed], (kMaxSymbols + 1) * kNh)};
    std::vector<cf32> X, Y(kN), Z(kN);
    float sc = 1.f / std::sqrt((float)(kNzc - 1));
    cf32 prevPeak(1, 0);
    int prevShift = 0;
    int nsym = 0;
    int rel[kMaxSymbols] = {0};
    bool ok = true;
    for (int s = 0; s < kMaxSymbols; s++) {
        size_t base = (size_t)bestPos + (size_t)s * L;
        if (base + L > n) { ok = false; break; }
        size_t aStart = base + (s == 0 ? kLenC : 1024);
        for (int i = 0; i < kN; i++) Y[i] = x[aStart + i];
        fft.forward(Y.data());
        freqSymbol(pns[0], s, X);
        for (auto& v : X) v *= sc;
        for (int i = 0; i < kN; i++) Z[i] = Y[i] * std::conj(X[i]);
        fft.forward(Z.data());
        int pk = 0; float pv = 0;
        for (int t = 0; t < kN; t++) { float a = std::abs(Z[t]); if (a > pv) { pv = a; pk = t; } }
        cf32 peak = Z[pk];
        if (s > 0) {
            rel[s] = ((pk - prevShift) % kN + kN) % kN;
            if ((peak * std::conj(prevPeak)).real() < 0) { nsym = s + 1; break; }   // the phase inversion marks the last symbol
        }
        prevShift = pk;
        prevPeak = peak;
    }
    if (!ok || nsym < 4) return d;
    d.info.numSymbols = nsym;
    d.valid = true;
    int fields[4][8] = {};
    for (int s = 1; s <= 3; s++) {
        // M~ = 8q + 4 for eight signalling bits: round to the nearest such value and check how far off it was
        int q = (int)std::lround((rel[s] - 4) / 8.0) & 255;
        int diff = ((rel[s] - (q * 8 + 4)) % kN + kN + kN / 2) % kN - kN / 2;
        if (std::abs(diff) > 3) d.valid = false;
        int m[8];
        for (int i = 0; i < 8; i++) m[i] = (q >> (7 - i)) & 1;   // m[0] = m10 ... m[7] = m3
        fields[s][0] = m[0];
        for (int k = 1; k < 8; k++) fields[s][k] = m[k] ^ m[k - 1];
    }
    d.info.eaWakeUp = (fields[1][0] << 1) | fields[2][0];
    d.info.minTimeToNext = bitsToInt(&fields[1][1], 5);
    d.info.systemBandwidth = bitsToInt(&fields[1][6], 2);
    d.info.bsrCoefficient = bitsToInt(&fields[2][1], 7);
    d.info.preambleStructure = bitsToInt(fields[3], 8);
    if (d.info.bsrCoefficient > 80) d.valid = false;
    return d;
}

} // namespace atsc3
} // namespace dect2
