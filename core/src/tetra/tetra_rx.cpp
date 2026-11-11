// TETRA network information receiver (tetra_rx.h): blocks of input, 25 kHz carriers found in the spectrum, slot synchronised on the
// training sequences, the broadcast channels decoded.
#include "dect2/common/thread_name.h"
#include "dect2/wirecap/wire.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_sds.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/results/results_io.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/tetra/tetra_tel.h"
#include "dect2/tetra/tetra_teds.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include "dect2/common/timecompat.h"

namespace dect2 {

ModeTuning tetraTuning() {
    ModeTuning t;
    t.stdMode = 48; t.id = "tetra"; t.name = "TETRA";
    t.minMhz = 136; t.maxMhz = 960; t.defMhz = 392.5;   // the European emergency services downlink is 390 - 395 MHz
    t.sampleRate = 2000000;      // 2 MHz of the band (80 channels) in one capture
    t.basebandHz = 1750000;
    t.bandwidthMhz = 1.8;
    t.minSampleRate = 200000;    // a few channels
    return t;
}

namespace tetra {

// ---------------------------------------------------------------- channel filter

std::vector<cf32> channelize(const cf32* x, size_t n, double fs, double shift) {
    std::vector<cf32> m(n);
    {
        const double w = -2 * M_PI * shift / fs;
        for (size_t i0 = 0; i0 < n; i0 += 4096) {
            std::complex<double> o(std::cos(w * (double)i0), std::sin(w * (double)i0));
            const std::complex<double> st(std::cos(w), std::sin(w));
            const size_t e = std::min(n, i0 + 4096);
            // the complex products written out (std::complex's operator* checks each product for NaN): the same products and sums, one per
            // statement (nothing fused), so the same bits
            double orr = o.real(), oi = o.imag();
            const double sr = st.real(), si = st.imag();
            for (size_t i = i0; i < e; i++) {
                const float a = x[i].real(), b = x[i].imag(), c = (float)orr, d = (float)oi;
                const float ac = a * c, bd = b * d, ad = a * d, bc = b * c;
                m[i] = cf32(ac - bd, ad + bc);
                const double pr = orr * sr, qi = oi * si, pi = orr * si, qr = oi * sr;
                orr = pr - qi; oi = pi + qr;
            }
        }
    }
    const int d1 = std::max(1, (int)std::floor(fs / 288e3));
    const double fs1 = fs / d1;
    std::vector<cf32> s1;
    if (d1 == 1) s1.swap(m);
    else {
        const int M = 8 * d1 + 1, H = M / 2;
        const double fc = 0.4 / d1;
        std::vector<float> h((size_t)M);
        double sum = 0;
        for (int i = 0; i < M; i++) {
            const double t = i - H, a = 2 * fc * t;
            const double s = std::fabs(a) < 1e-9 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            const double u = (double)(i - H) / (H + 1);
            h[(size_t)i] = (float)(s * (0.42 + 0.5 * std::cos(M_PI * u) + 0.08 * std::cos(2 * M_PI * u)));
            sum += h[(size_t)i];
        }
        for (auto& v : h) v = (float)(v / sum);
        const size_t no = n / (size_t)d1;
        s1.resize(no);
        for (size_t o = 0; o < no; o++) {
            const long c = (long)(o * (size_t)d1);
            cf32 acc(0, 0);
            const long lo = std::max(0L, c - H), hi = std::min((long)n - 1, c + H);
            for (long k = lo; k <= hi; k++) acc += m[(size_t)k] * h[(size_t)(k - c + H)];
            s1[o] = acc;
        }
    }
    const size_t nOut = (size_t)std::floor((double)s1.size() * kRate / fs1);
    if (nOut < 16) return {};
    return cell::resampleBlock(s1.data(), s1.size(), fs1, kRate, 0, 40e3, 0, nOut);
}

// ---------------------------------------------------------------- slot synchronisation

namespace {

struct Tpl { std::vector<int> k; std::vector<cf32> e; };   // symbol numbers in the slot (1-based; <= 0: the slot before) and their steps

Tpl makeTpl(const uint8_t* bits, int nBits, int firstSym) {
    Tpl t;
    for (int i = 0; i < nBits / 2; i++) {
        const int ph = dibitPhase(bits[2 * i], bits[2 * i + 1]);
        t.k.push_back(firstSym + i);
        t.e.push_back(cf32((float)std::cos(ph * M_PI / 4), (float)-std::sin(ph * M_PI / 4)));   // conjugated
    }
    return t;
}

const Tpl& tplQ() { static const Tpl t = makeTpl(kTrainQ, 22, -4); return t; }
const Tpl& tplN() { static const Tpl t = makeTpl(kTrainN, 22, 123); return t; }
const Tpl& tplP() { static const Tpl t = makeTpl(kTrainP, 22, 123); return t; }
const Tpl& tplY() { static const Tpl t = makeTpl(kTrainY, 38, 108); return t; }

// the pi/8-D8PSK normal training sequences (TEDS): 11 symbols of three bits at symbols 123 .. 133, as the pi/4-DQPSK n and p
Tpl makeTpl8(const uint8_t* bits) {
    Tpl t;
    for (int i = 0; i < 11; i++) {
        const int ph = tribitPhase(bits[3 * i], bits[3 * i + 1], bits[3 * i + 2]);
        t.k.push_back(kP8TrainSym + i);
        t.e.push_back(cf32((float)std::cos(ph * M_PI / 8), (float)-std::sin(ph * M_PI / 8)));
    }
    return t;
}
const Tpl& tplN8() { static const Tpl t = makeTpl8(kTrainN8); return t; }
const Tpl& tplP8() { static const Tpl t = makeTpl8(kTrainP8); return t; }

inline bool isP8(Burst b) { return b == Burst::NormalFullP8 || b == Burst::NormalHalfP8; }

const Tpl& midTpl(Burst b) {
    switch (b) {
        case Burst::Sync: return tplY();
        case Burst::NormalHalf: return tplP();
        case Burst::NormalFullP8: return tplN8();
        case Burst::NormalHalfP8: return tplP8();
        default: return tplN();
    }
}

struct Score { float v = 0; cf32 sum{0, 0}; float mag = 0; };

inline Score score(const std::vector<cf32>& dd, long s, const Tpl& t) {
    Score r;
    for (size_t i = 0; i < t.k.size(); i++) {
        const long p = s + 4L * (t.k[i] - 1);
        if (p < 0 || p >= (long)dd.size()) return Score();
        const cf32 v = dd[(size_t)p];
        r.sum += v * t.e[i];
        r.mag += std::abs(v);
    }
    r.v = r.mag > 0 ? std::abs(r.sum) / r.mag : 0.f;
    return r;
}

struct SlotFit { float total = 0; Burst type = Burst::None; float mid = 0; cf32 rot{1, 0}; };

SlotFit fitSlot(const std::vector<cf32>& dd, long s) {
    SlotFit f;
    const Score q = score(dd, s, tplQ()), n = score(dd, s, tplN()), p = score(dd, s, tplP()), y = score(dd, s, tplY());
    const Score n8 = score(dd, s, tplN8()), p8 = score(dd, s, tplP8());
    const Score* best = &n; f.type = Burst::NormalFull;
    if (p.v > best->v) { best = &p; f.type = Burst::NormalHalf; }
    if (y.v > best->v) { best = &y; f.type = Burst::Sync; }
    if (n8.v > best->v) { best = &n8; f.type = Burst::NormalFullP8; }
    if (p8.v > best->v) { best = &p8; f.type = Burst::NormalHalfP8; }
    f.mid = best->v;
    f.total = q.v + best->v;
    const cf32 s2 = q.sum + best->sum;
    const float a = std::abs(s2);
    f.rot = a > 0 ? std::conj(s2) / a : cf32(1, 0);
    return f;
}

// Coherent detection of one slot (3.9). pi/4-DQPSK as a 4-state trellis: symbol k is z_k = A exp(j (theta + k pi/4 + q_k pi/2)) + noise, the
// dibit of symbol k sets q_k - q_(k-1) (00: 0, 01: 1, 11: 2, 10: 3, the steps +pi/4, +3pi/4, -3pi/4, -pi/4 less pi/4). theta is the carrier
// phase, estimated without the data from the fourth power of z_k exp(-j k pi/4) over a sliding window and unwrapped modulo pi/2 (the
// trellis does not care which multiple of pi/2: the bits are the steps). The bit LLRs come from the forward-backward (log-MAP) recursion over
// the slot and the symbols around it. Against the differential detector (each symbol against the one before, both noisy) this gains about
// 2 dB; docs/TETRA-3.9.md.
constexpr int kCohW = 20;   // half window of the phase estimate, symbols (10 to 40 measured the same)

inline float maxStar(float a, float b) { return std::max(a, b) + std::log1p(std::exp(-std::fabs(a - b))); }

// z: symbols kZ0 .. kZ0 + z.size() - 1 of the slot (1-based symbol numbers); llr: 510 bits of symbols 1 .. 255, > 0 means a 1, extrinsic
// (the a-priori of the bit itself taken out). apri: null, or 2 * z.size() a-priori LLRs (the known training bits, the channel decoder's
// extrinsic information on a later pass), the two bits of each symbol of z.
void coherentLlr(const std::vector<cf32>& z, int kZ0, float* llr, const float* apri = nullptr) {
    const int N = (int)z.size();
    // carrier phase: (-1)^k z^4 / |z|^2 summed over +-kCohW
    std::vector<cf32> v((size_t)N);
    for (int i = 0; i < N; i++) {
        const cf32 a = z[(size_t)i], a2 = a * a;
        const float m = std::norm(a);
        v[(size_t)i] = m > 0 ? a2 * a2 / m * (((kZ0 + i) & 1) ? -1.f : 1.f) : cf32(0, 0);
    }
    std::vector<cf32> w((size_t)N);
    cf32 acc(0, 0);
    for (int i = 0; i < std::min(N, kCohW); i++) acc += v[(size_t)i];
    float prev = 0;
    bool first = true;
    for (int i = 0; i < N; i++) {
        if (i + kCohW < N) acc += v[(size_t)(i + kCohW)];
        if (i - kCohW - 1 >= 0) acc -= v[(size_t)(i - kCohW - 1)];
        float th = std::arg(acc) / 4;
        if (!first) {
            while (th - prev > (float)M_PI / 4) th -= (float)M_PI / 2;
            while (th - prev < -(float)M_PI / 4) th += (float)M_PI / 2;
        }
        first = false;
        prev = th;
        const float ph = th + (float)((((kZ0 + i) % 8) + 8) % 8) * (float)M_PI / 4;
        w[(size_t)i] = z[(size_t)i] * cf32(std::cos(ph), -std::sin(ph));
    }
    // amplitude and noise from the moments of a constant envelope in complex Gaussian noise: E|w|^2 = A^2 + s2, E|w|^4 = A^4 + 4 A^2 s2 + 2 s2^2
    double m2 = 0, m4 = 0;
    for (const cf32& a : w) { const double p = std::norm(a); m2 += p; m4 += p * p; }
    m2 /= N; m4 /= N;
    double A2 = 2 * m2 * m2 - m4;
    A2 = A2 > 0.05 * m2 * m2 ? std::sqrt(A2) : 0.2236 * m2;   // at least 13 dB below: the slot is mostly noise
    const double s2 = std::max(1e-12, m2 - A2);
    const float gain = (float)(2 * std::sqrt(A2) / s2);
    // the observation metric of each state, q = 0..3: gain * Re(w * conj(j^q))
    auto obs = [&](int i, float* o) {
        const cf32 a = w[(size_t)i] * gain;
        o[0] = a.real(); o[1] = a.imag(); o[2] = -a.real(); o[3] = -a.imag();
    };
    static const int kStep[4] = {0, 1, 3, 2};   // dibit b1 b2 (00, 01, 10, 11) -> q step
    // the a-priori term of dibit d at z index i: +-L/2 for each of its bits
    auto pri = [&](int i, int d) -> float {
        if (!apri) return 0.f;
        const float a = apri[2 * i], b = apri[2 * i + 1];
        return 0.5f * ((d & 2) ? a : -a) + 0.5f * ((d & 1) ? b : -b);
    };
    std::vector<float> alpha((size_t)N * 4), beta((size_t)N * 4);
    float o[4];
    obs(0, o);
    for (int q = 0; q < 4; q++) alpha[(size_t)q] = o[q];
    for (int i = 1; i < N; i++) {
        obs(i, o);
        float* al = &alpha[(size_t)i * 4];
        const float* ap = &alpha[(size_t)(i - 1) * 4];
        float mx = -1e30f;
        for (int q = 0; q < 4; q++) {
            float t = -1e30f;
            for (int d = 0; d < 4; d++) t = maxStar(t, ap[(q - kStep[d] + 4) & 3] + pri(i, d));
            al[q] = t + o[q];
            mx = std::max(mx, al[q]);
        }
        for (int q = 0; q < 4; q++) al[q] -= mx;
    }
    for (int q = 0; q < 4; q++) beta[(size_t)(N - 1) * 4 + (size_t)q] = 0;
    for (int i = N - 2; i >= 0; i--) {
        obs(i + 1, o);
        float* be = &beta[(size_t)i * 4];
        const float* bn = &beta[(size_t)(i + 1) * 4];
        float mx = -1e30f;
        for (int q = 0; q < 4; q++) {
            float t = -1e30f;
            for (int d = 0; d < 4; d++) { const int qn = (q + kStep[d]) & 3; t = maxStar(t, bn[qn] + o[qn] + pri(i + 1, d)); }
            be[q] = t;
            mx = std::max(mx, t);
        }
        for (int q = 0; q < 4; q++) be[q] -= mx;
    }
    for (int k = 1; k <= kSlotSyms; k++) {
        const int i = k - kZ0;
        if (i < 1 || i >= N) { llr[2 * k - 2] = llr[2 * k - 1] = 0; continue; }
        obs(i, o);
        float dib[4];
        for (int d = 0; d < 4; d++) {
            float t = -1e30f;
            for (int q = 0; q < 4; q++) { const int qn = (q + kStep[d]) & 3; t = maxStar(t, alpha[(size_t)(i - 1) * 4 + (size_t)q] + o[qn] + beta[(size_t)i * 4 + (size_t)qn]); }
            dib[d] = t + pri(i, d);
        }
        llr[2 * k - 2] = maxStar(dib[2], dib[3]) - maxStar(dib[0], dib[1]) - (apri ? apri[2 * i] : 0.f);
        llr[2 * k - 1] = maxStar(dib[1], dib[3]) - maxStar(dib[0], dib[2]) - (apri ? apri[2 * i + 1] : 0.f);
    }
}

// The known bits of a downlink slot of this burst type as a-priori LLRs over the symbols kZ0 .. kZ0 + N - 1 (2 per symbol): training
// sequence 3 at both ends (and in the slots on either side), the normal or synchronisation training sequence, the frequency correction field
void trainingApri(Burst type, int kZ0, int N, std::vector<float>& apri) {
    constexpr float kKnown = 16.f;
    apri.assign((size_t)(2 * N), 0.f);
    auto setBits = [&](int firstBit /* 0-based bit of the slot, may be < 0 or >= 510 */, const uint8_t* v, int nb) {
        for (int j = 0; j < nb; j++) {
            const int b = firstBit + j;
            const int k = (b >= 0 ? b / 2 : (b - 1) / 2) + 1, sub = ((b % 2) + 2) % 2;
            const int i = k - kZ0;
            if (i >= 0 && i < N) apri[(size_t)(2 * i + sub)] = v[j] ? kKnown : -kKnown;
        }
    };
    setBits(0, kTrainQ + 10, 12);
    setBits(500, kTrainQ, 10);
    setBits(-10, kTrainQ, 10);              // the slot before: its q1 .. q10
    setBits(kSlotBits, kTrainQ + 10, 12);   // the slot after: its q11 .. q22
    if (type == Burst::Sync) { setBits(14, kFreqCorr, 80); setBits(kTrainYAt, kTrainY, 38); }
    else if (type == Burst::NormalHalf) setBits(kTrainNAt, kTrainP, 22);
    else if (type == Burst::NormalFull) setBits(kTrainNAt, kTrainN, 22);
}

// A coded block of a slot: its bit positions (0-based in the 510 of the slot), its channel and scrambling code
struct BlockRef { int at1, n1, at2, n2; const ChanSpec* c; uint32_t scr; };

// Turbo detection of one pi/4-DQPSK slot (3.9): the coherent detector and the channel decoder exchange extrinsic information until every
// block's CRC holds or kIters passes are done. llr holds the first pass on entry (coherentLlr with the training) and the last on return.
constexpr int kIters = 3;
void refineSlot(const std::vector<cf32>& z, int kZ0, Burst type, const std::vector<BlockRef>& blocks, float* llr) {
    if (blocks.empty()) return;
    const int N = (int)z.size();
    std::vector<float> apri, base, l5, e5;
    trainingApri(type, kZ0, N, base);
    for (int it = 0; it < kIters; it++) {
        bool all = true;
        apri = base;
        for (const BlockRef& b : blocks) {
            l5.resize((size_t)b.c->K); e5.resize((size_t)b.c->K);
            std::copy(llr + b.at1, llr + b.at1 + b.n1, l5.begin());
            if (b.n2) std::copy(llr + b.at2, llr + b.at2 + b.n2, l5.begin() + b.n1);
            Bits t1;
            if (decodeChannel(l5.data(), *b.c, b.scr, t1)) continue;
            all = false;
            channelExtrinsic(l5.data(), *b.c, b.scr, e5.data());
            auto put = [&](int at, int nb, const float* e) {
                for (int j = 0; j < nb; j++) {
                    const int bit = at + j, k = bit / 2 + 1, i = k - kZ0;
                    if (i >= 0 && i < N) apri[(size_t)(2 * i + bit % 2)] = std::max(-12.f, std::min(12.f, e[j]));
                }
            };
            put(b.at1, b.n1, e5.data());
            if (b.n2) put(b.at2, b.n2, e5.data() + b.n1);
        }
        if (all) return;
        coherentLlr(z, kZ0, llr, apri.data());
    }
}

} // namespace

// ---------------------------------------------------------------- one carrier

CarrierDecode decodeCarrier(const cf32* x, size_t n, int kMcc, int kMnc, int kCc, SpeechFrameSink* sink, CallTracker* calls, double nowSec, int voiceTn,
                            VoiceTimeline* timeline) {
    CarrierDecode out;
    if (n < (size_t)kSlotSamples * 8) return out;
    // matched filter
    static const std::vector<float> h = rrcTaps(kSps, 8);
    const int H = (int)h.size() / 2;
    std::vector<cf32> y(n);
    auto filter = [&](const cf32* in) {
        for (size_t i = 0; i < n; i++) {
            cf32 acc(0, 0);
            const long lo = std::max(0L, (long)i - H), hi = std::min((long)n - 1, (long)i + H);
            for (long k = lo; k <= hi; k++) acc += in[(size_t)k] * h[(size_t)(k - (long)i + H)];
            y[i] = acc;
        }
    };
    filter(x);
    // frequency: the fourth power of the one-symbol differential is -1 times the error's rotation, at the best sampling phase
    double fTot = 0;
    for (int pass = 0; pass < 2; pass++) {
        std::complex<double> acc[kSps];
        for (size_t i = kSps; i < n; i++) {
            const cf32 d = y[i] * std::conj(y[i - kSps]);
            const std::complex<double> d2 = std::complex<double>(d) * std::complex<double>(d);
            acc[i % kSps] += d2 * d2 / (std::abs(d) + 1e-20);
        }
        int bp = 0;
        for (int p = 1; p < kSps; p++) if (std::abs(acc[p]) > std::abs(acc[bp])) bp = p;
        const double w = std::arg(-acc[bp]) / 4;   // radians per symbol
        const double f = w * kSymRate / (2 * M_PI);
        fTot += f;
        cell::mixDown(y.data(), n, f, kRate);
    }
    out.cfoHz = fTot;
    // 3.9: a tuning error of more than a few hundred hertz moved the signal against the matched filter: corrected before the filter, then
    // the filter run again (about 0.3 dB at 1.2 kHz)
    if (std::fabs(fTot) > 150) {
        std::vector<cf32> xm(x, x + n);
        cell::mixDown(xm.data(), n, fTot, kRate);
        filter(xm.data());
    }
    std::vector<cf32> dd(n, cf32(0, 0));
    for (size_t i = kSps; i < n; i++) dd[i] = y[i] * std::conj(y[i - kSps]);
    // slot grid: the scores folded over the slot length
    const long s0 = 32;
    const long avail = (long)n - s0 - kSlotSamples - 160 * kSps;
    if (avail < kSlotSamples) return out;
    const int M = (int)std::min<long>(24, avail / kSlotSamples);
    std::vector<float> fold((size_t)kSlotSamples, 0.f);
    for (int m = 0; m < M; m++)
        for (int ph = 0; ph < kSlotSamples; ph++) fold[(size_t)ph] += fitSlot(dd, s0 + (long)m * kSlotSamples + ph).total;
    const int phase = (int)(std::max_element(fold.begin(), fold.end()) - fold.begin());
    if (fold[(size_t)phase] / M < 1.0f) return out;   // random data scores about 0.6
    out.tetra = true;
    // track the slots (the clock error moves them by a fraction of a sample each)
    struct Slot { long s; SlotFit f; float mu; std::vector<cf32> d, z; };
    std::vector<Slot> slots;
    for (long s = s0 + phase; s + kSlotSamples + 8 < (long)n; s += kSlotSamples) {
        SlotFit best = fitSlot(dd, s);
        long bs = s;
        for (long d = -3; d <= 3; d++) { if (!d) continue; const SlotFit f = fitSlot(dd, s + d); if (f.total > best.total) { best = f; bs = s + d; } }
        if (best.total >= 1.1f) s = bs; else best = fitSlot(dd, s);
        slots.push_back({s, best, 0.f, {}, {}});
    }
    out.slots = (int)slots.size();
    // the fraction of a sample between the best whole sample and the true symbol instant (parabola through the scores), and the
    // differential symbols there (linear interpolation of the 4 samples per symbol), symbols -4 .. 255 of the slot
    constexpr int kD0 = -4, kZ0 = -11, kZ1 = 12;   // differential symbols from kD0; the coherent detector's symbols kZ0 .. 255 + kZ1
    auto at = [&](double p) -> cf32 {
        const long i = (long)std::floor(p);
        if (i < 0 || i + 1 >= (long)n) return cf32(0, 0);
        const float f = (float)(p - (double)i);
        return y[(size_t)i] * (1 - f) + y[(size_t)i + 1] * f;
    };
    for (auto& sl : slots) {
        const float a = fitSlot(dd, sl.s - 1).total, b = sl.f.total, c = fitSlot(dd, sl.s + 1).total;
        const float den = a - 2 * b + c;
        sl.mu = den < -1e-6f ? std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / den)) : 0.f;
    }
    // 3.9: the symbol instants of all the slots as one straight line (the two clocks differ by a constant rate over a capture). Each slot's
    // instant from the energy of the matched filter output at its 255 symbols (a parabola through three whole-sample offsets), then a
    // least-squares line over the slots, twice, dropping slots more than half a sample off it (a wrong slot found by the tracker). The
    // training-sequence estimate above uses 33 symbols of one slot; this uses every symbol of the capture.
    if (slots.size() >= 4) {
        std::vector<double> tau(slots.size());
        for (size_t i = 0; i < slots.size(); i++) {
            double e[3] = {0, 0, 0};
            for (int d = -1; d <= 1; d++)
                for (int k = 1; k <= kSlotSyms; k++) {
                    const long p = slots[i].s + 4L * (k - 1) + d;
                    if (p >= 0 && p < (long)n) e[d + 1] += std::norm(y[(size_t)p]);
                }
            const double den = e[0] - 2 * e[1] + e[2];
            const double f = den < 0 ? std::max(-1.0, std::min(1.0, 0.5 * (e[0] - e[2]) / den)) : 0.0;
            tau[i] = (double)slots[i].s + f;
        }
        std::vector<char> use(slots.size(), 1);
        double a0 = 0, a1 = kSlotSamples;
        for (int it = 0; it < 3; it++) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
            for (size_t i = 0; i < slots.size(); i++) if (use[i]) { const double xi = (double)i; sx += xi; sy += tau[i]; sxx += xi * xi; sxy += xi * tau[i]; cnt++; }
            const double den = cnt * sxx - sx * sx;
            if (cnt < 4 || den <= 0) break;
            a1 = (cnt * sxy - sx * sy) / den; a0 = (sy - a1 * sx) / cnt;
            for (size_t i = 0; i < slots.size(); i++) use[i] = std::fabs(tau[i] - (a0 + a1 * (double)i)) < (it == 0 ? 1.5 : 0.5);
        }
        if (std::fabs(a1 - kSlotSamples) < 0.01 * kSlotSamples) {
            for (size_t i = 0; i < slots.size(); i++) {
                const double t = a0 + a1 * (double)i;
                if (std::fabs(t - (double)slots[i].s) > 2.5) continue;   // the tracker's slot is elsewhere: keep its own estimate
                const long si = std::lround(t);
                if (si != slots[i].s) { slots[i].s = si; }
                slots[i].mu = (float)(t - (double)si);
            }
        }
    }
    for (auto& sl : slots) {
        sl.d.resize((size_t)(kSlotSyms - kD0 + 1));
        cf32 prev = at((double)sl.s + 4.0 * (kD0 - 2) + sl.mu);
        for (int k = kD0; k <= kSlotSyms; k++) {
            const cf32 cur = at((double)sl.s + 4.0 * (k - 1) + sl.mu);
            sl.d[(size_t)(k - kD0)] = cur * std::conj(prev);
            prev = cur;
        }
        // the symbols themselves for the coherent detector: the slot, the end of the slot before and the start of the one after
        sl.z.resize((size_t)(kSlotSyms + kZ1 - kZ0 + 1));
        for (int k = kZ0; k <= kSlotSyms + kZ1; k++) sl.z[(size_t)(k - kZ0)] = at((double)sl.s + 4.0 * (k - 1) + sl.mu);
        const Tpl* mid = &midTpl(sl.f.type);
        cf32 sum(0, 0);
        for (const Tpl* t : {&tplQ(), mid}) for (size_t i = 0; i < t->k.size(); i++) sum += sl.d[(size_t)(t->k[i] - kD0)] * t->e[i];
        if (std::abs(sum) > 0) sl.f.rot = std::conj(sum) / std::abs(sum);
    }
    // soft bits of a slot
    double mAcc = 0; for (const auto& sl : slots) for (int k = 1; k <= kSlotSyms; k += 8) mAcc += std::abs(sl.d[(size_t)(k - kD0)]);
    const float g = (float)(mAcc > 0 ? 4.0 / (mAcc / ((double)slots.size() * 32)) : 1.0);
    // soft bits of a pi/4-DQPSK slot: the coherent detector with the slot's known bits (3.9; the 3.8 receiver detected each symbol against
    // the one before)
    std::vector<float> triApri;
    auto softBits = [&](const Slot& sl, float* llr) {
        trainingApri(sl.f.type, kZ0, (int)sl.z.size(), triApri);
        coherentLlr(sl.z, kZ0, llr, triApri.data());
    };
    // A TEDS pi/8-D8PSK slot (table 9.8): differential detection, max-log over the eight steps of each symbol (or the four of the pi/4-DQPSK
    // symbols of the broadcast block). bkn1 / bkn2: 324 LLRs each, bb: 30.
    auto softBitsP8 = [&](const Slot& sl, float* bkn1, float* bb, float* bkn2) {
        auto dsym = [&](int k) { return sl.d[(size_t)(k - kD0)] * sl.f.rot * g; };
        auto tri = [&](int k, float* l) {
            const cf32 d = dsym(k);
            float m1[3] = {-1e30f, -1e30f, -1e30f}, m0[3] = {-1e30f, -1e30f, -1e30f};
            for (int v = 0; v < 8; v++) {
                const int b1 = v >> 2, b2 = (v >> 1) & 1, b3 = v & 1, ph = tribitPhase(b1, b2, b3);
                const float m = d.real() * (float)std::cos(ph * M_PI / 8) + d.imag() * (float)std::sin(ph * M_PI / 8);
                const int b[3] = {b1, b2, b3};
                for (int i = 0; i < 3; i++) { float& t = b[i] ? m1[i] : m0[i]; t = std::max(t, m); }
            }
            for (int i = 0; i < 3; i++) l[i] = m1[i] - m0[i];
        };
        auto di = [&](int k, float* l) {   // the pi/4-DQPSK symbols: as the differential detector of the 3.8 receiver
            const cf32 d = dsym(k);
            l[0] = -d.imag(); l[1] = -d.real();
        };
        for (int i = 0; i < 108; i++) { tri(kP8Bkn1Sym + i, bkn1 + 3 * i); tri(kP8Bkn2Sym + i, bkn2 + 3 * i); }
        for (int i = 0; i < 7; i++) di(116 + i, bb + 2 * i);
        for (int i = 0; i < 8; i++) di(134 + i, bb + 14 + 2 * i);
    };
    // SNR from the training symbols
    {
        double sm = 0, sv = 0; int cnt = 0;
        for (const auto& sl : slots) {
            if (sl.f.total < 1.4f) continue;
            const Tpl* mid = &midTpl(sl.f.type);
            std::vector<cf32> v;
            for (const Tpl* t : {&tplQ(), mid}) for (size_t i = 0; i < t->k.size(); i++) v.push_back(sl.d[(size_t)(t->k[i] + 4)] * t->e[i] * sl.f.rot * g);
            cf32 mu(0, 0); for (auto& a : v) mu += a; mu /= (float)v.size();
            double var = 0; for (auto& a : v) var += std::norm(a - mu);
            sm += std::norm(mu); sv += var / (double)v.size(); cnt++;
        }
        if (cnt && sv > 0) out.snrDb = (float)(10 * std::log10(sm / sv) + 3.0);
    }
    // pass 1: the synchronisation bursts
    std::vector<float> llr((size_t)kSlotBits);
    long syncSlot = -1;
    for (size_t i = 0; i < slots.size(); i++) {
        if (slots[i].f.type != Burst::Sync || slots[i].f.mid < 0.5f) continue;
        softBits(slots[i], llr.data());
        refineSlot(slots[i].z, kZ0, Burst::Sync, {{kSbSb, 120, 0, 0, &kBsch, scramblerInit(0, 0, 0)}}, llr.data());
        Bits t1;
        if (!decodeChannel(llr.data() + kSbSb, kBsch, scramblerInit(0, 0, 0), t1)) { out.blocksBad++; continue; }
        Sync s;
        if (!parseSync(t1, s)) { out.blocksBad++; continue; }
        if (wire::on(wire::Tetra)) { const wire::Bytes b = wire::packBitsMsbFirst(t1.data(), t1.size()); wire::Hub::get().publish(wire::Tetra, wire::makeGsmtap(wire::gsmtap::kTypeTetraI1, wire::gsmtap::kTetraBsch, b.data(), b.size())); }
        out.synced = true; out.sync = s; out.syncs++; out.blocksOk++;
        syncSlot = (long)i;
    }
    int mcc = kMcc, mnc = kMnc, cc = kCc;
    if (out.synced) { mcc = out.sync.mcc; mnc = out.sync.mnc; cc = out.sync.colourCode; }
    if (mcc < 0 || mnc < 0 || cc < 0) return out;
    const uint32_t scr = scramblerInit(mcc, mnc, cc);
    out.scrambler = scr;
    // pass 2: the access assignment and the signalling blocks of every slot
    auto frameOf = [&](size_t i, int& tn, int& fn, int& mn) {
        tn = 0; fn = 0; mn = 0;
        if (syncSlot < 0) return;
        const long a0 = ((long)(out.sync.mn - 1) * 18 + (out.sync.fn - 1)) * 4 + (out.sync.tn - 1);
        const long a = ((a0 + (long)i - syncSlot) % (60L * 72) + 60L * 72) % (60L * 72);
        tn = (int)(a % 4) + 1; fn = (int)((a / 4) % 18) + 1; mn = (int)(a / 72) + 1;
    };
    struct TrafficSlot { int tn; std::vector<float> t; };
    std::vector<TrafficSlot> traffic;
    // the TDMA frames of each timeslot in the capture, in order: the speech frame index (tetra_voice.h, VoiceTimeline) of frames 1 to 17 of the
    // multiframe (frame 18 carries no speech) and the traffic burst of the frame (index into traffic, -1 none)
    struct SlotFrame { long speech; int burst; };
    std::vector<SlotFrame> frames[5];
    for (size_t i = 0; i < slots.size(); i++) {
        const Slot& sl = slots[i];
        int tn, fn, mn;
        frameOf(i, tn, fn, mn);
        if (tn >= 1 && fn <= 17) frames[tn].push_back({(long)(mn - 1) * 17 + fn - 1, -1});
        if (sl.f.mid < 0.5f || sl.f.total < 1.0f) continue;
        float bb[30];
        std::vector<float> p8a, p8b;
        if (isP8(sl.f.type)) { p8a.resize(324); p8b.resize(324); softBitsP8(sl, p8a.data(), bb, p8b.data()); out.p8Slots++; }
        else {
            softBits(sl, llr.data());
            if (sl.f.type == Burst::Sync) std::copy(llr.begin() + kSbBb, llr.begin() + kSbBb + 30, bb);
            else { std::copy(llr.begin() + kNbBb1, llr.begin() + kNbBb1 + 14, bb); std::copy(llr.begin() + kNbBb2, llr.begin() + kNbBb2 + 16, bb + 14); }
        }
        uint16_t aa = 0;
        int usage = -1;
        if (decodeAach(bb, scr, aa)) {
            usage = aachDlUsage(aa, fn == 18);
            if (tn >= 1 && usage >= 0) out.tsUsage[tn - 1] = usage;
        }
        if (!isP8(sl.f.type)) {   // the signalling blocks of the slot, decoded together with the demodulator (never traffic)
            std::vector<BlockRef> br;
            if (sl.f.type == Burst::Sync) br.push_back({kSbBkn2, 216, 0, 0, &kSchHd, scr});
            else if (sl.f.type == Burst::NormalHalf) { br.push_back({kNbBkn1, 216, 0, 0, &kSchHd, scr}); br.push_back({kNbBkn2, 216, 0, 0, &kSchHd, scr}); }
            else if (usage >= 0 && usage < 4) br.push_back({kNbBkn1, 216, kNbBkn2, 216, &kSchF, scr});
            refineSlot(sl.z, kZ0, sl.f.type, br, llr.data());
            if (usage < 0 && !br.empty()) {   // the access assignment again, from the refined bits
                if (sl.f.type == Burst::Sync) std::copy(llr.begin() + kSbBb, llr.begin() + kSbBb + 30, bb);
                else { std::copy(llr.begin() + kNbBb1, llr.begin() + kNbBb1 + 14, bb); std::copy(llr.begin() + kNbBb2, llr.begin() + kNbBb2 + 16, bb + 14); }
                if (decodeAach(bb, scr, aa)) { usage = aachDlUsage(aa, fn == 18); if (tn >= 1 && usage >= 0) out.tsUsage[tn - 1] = usage; }
            }
        }
        static thread_local FragReassembler localFrag;
        auto block = [&](const float* l, const ChanSpec& c) {
            Bits t1;
            const bool p8 = c.K == kSchP8Hd.K || c.K == kSchP8F.K;
            if (!decodeChannel(l, c, scr, t1)) { out.blocksBad++; if (p8) out.p8BlocksBad++; return; }
            out.blocksOk++;
            if (p8) {
                out.p8BlocksOk++;
                // SYSINFO sent with pi/8-D8PSK (21.4.4.1): 28 reserved bits between the optional field and the MLE part (152 bits in all);
                // without them it is the pi/4-DQPSK layout the parser reads
                if (t1.size() >= 152 && t1[0] == 1 && t1[1] == 0 && t1[2] == 0 && t1[3] == 0) t1.erase(t1.begin() + 82, t1.begin() + 110);
            }
            if (wire::on(wire::Tetra) && !t1.empty()) {   // type-1 bits of the signalling block, as received: encrypted content stays encrypted (voice is not sent)
                const bool broadcast = t1.size() > 1 && t1[0] == 1 && t1[1] == 0;   // MAC PDU type 2
                const uint8_t sub = t1.size() >= 200 ? wire::gsmtap::kTetraSchF : broadcast ? wire::gsmtap::kTetraBnch : wire::gsmtap::kTetraSchHd;
                const wire::Bytes b = wire::packBitsMsbFirst(t1.data(), t1.size());
                wire::Hub::get().publish(wire::Tetra, wire::makeGsmtap(wire::gsmtap::kTypeTetraI1, sub, b.data(), b.size(), 0, (uint8_t)(tn > 0 ? tn - 1 : 0), (uint32_t)fn));
            }
            MacBlock mb;
            parseMacBlock(t1, mb);
            if (mb.fragKind) {
                Bits sdu; int a = -1, sl2 = -1, cr = -1;
                FragReassembler& fr = calls ? calls->frag_[tn >= 1 && tn <= 4 ? tn : 0] : localFrag;
                if (fr.feed(mb, sdu, a, sl2, cr)) parseTlSdu(sdu, a, sl2, cr, mb);
            }
            out.macs.push_back(std::move(mb));
        };
        if (sl.f.type == Burst::Sync) block(llr.data() + kSbBkn2, kSchHd);
        else if (sl.f.type == Burst::NormalHalf) { block(llr.data() + kNbBkn1, kSchHd); block(llr.data() + kNbBkn2, kSchHd); }
        else if (sl.f.type == Burst::NormalHalfP8) { block(p8a.data(), kSchP8Hd); block(p8b.data(), kSchP8Hd); }
        else if (sl.f.type == Burst::NormalFullP8) {
            if (usage >= 4) continue;   // pi/8-D8PSK traffic (TEDS packet data): never decoded
            std::vector<float> f(648);
            std::copy(p8a.begin(), p8a.end(), f.begin());
            std::copy(p8b.begin(), p8b.end(), f.begin() + 324);
            block(f.data(), kSchP8F);
        }
        else {
            if (usage >= 4) {   // a traffic channel: handled after the signalling of the capture is known (encryption check first)
                out.trafficSlots++;
                std::vector<float> t(432);
                std::copy(llr.begin() + kNbBkn1, llr.begin() + kNbBkn1 + 216, t.begin());
                std::copy(llr.begin() + kNbBkn2, llr.begin() + kNbBkn2 + 216, t.begin() + 216);
                if (tn >= 1 && fn <= 17) frames[tn].back().burst = (int)traffic.size();
                traffic.push_back({tn, std::move(t)});
                continue;
            }
            float f[432];
            std::copy(llr.begin() + kNbBkn1, llr.begin() + kNbBkn1 + 216, f);
            std::copy(llr.begin() + kNbBkn2, llr.begin() + kNbBkn2 + 216, f + 216);
            block(f, kSchF);
        }
    }
    // call set-up PDUs (D-SETUP / D-CONNECT / release) read from the clear signalling of this capture: the per-call encryption state
    if (calls) {
        for (const auto& m : out.macs) if (m.sysinfo) calls->setMainCarrier(m.si.mainCarrier);
        for (const auto& m : out.macs) for (const auto& e : m.calls) calls->onEvent(e, nowSec);
    }
    // The encryption gate of every traffic burst, taken before any burst is read. What the signalling of this capture says about the cell:
    // any encrypted PDU means encrypted; the air interface encryption service may be advertised. Whether the call is clear is known from
    // its set-up PDU alone (CallTracker, read from the clear MAC-RESOURCE signalling of this or an earlier capture): a capture taken during
    // a talk spurt often has no MAC-RESOURCE at all, only null PDUs, broadcasts and the traffic itself, and demanding one here refused whole
    // captures of a clear call. Unknown counts as encrypted.
    std::vector<CallEncryption> gate(traffic.size());
    if (!traffic.empty()) {
        CallEncryption cell;
        int encPdus = 0, clearPdus = 0;
        for (const auto& m : out.macs) {
            encPdus += m.encrypted; clearPdus += m.resources - m.encrypted;
            if (m.encrypted > 0) cell.encMode = 1;
            if (m.sysinfo && m.si.bsService >= 0 && (m.si.bsService & kSvcAirEncryption)) cell.cellOffersEncryption = true;
        }
        for (size_t k = 0; k < traffic.size(); k++) {
            // the per-call gate AND the cell-level gate: clear only when the call's set-up PDU said "no encryption" and no PDU of the capture is encrypted
            CallEncryption ce = calls ? calls->slotState(traffic[k].tn, nowSec) : CallEncryption{};
            ce.cellOffersEncryption = cell.cellOffersEncryption;
            // a call whose own set-up said clear is vetoed only when encrypted PDUs outnumber the clear ones (a cell mixing clear and encrypted terminals)
            if (cell.encMode != 0 && !(ce.known && ce.encMode == 0 && encPdus <= clearPdus)) ce.encMode = 1;
            gate[k] = ce;
            const VoiceVerdict v = voiceVerdict(ce);
            out.voiceStatus = voiceVerdictText(v);
            if (v == VoiceVerdict::Passed) out.voicePassed++; else out.voiceNotDecoded++;
            if (traffic[k].tn >= 1 && traffic[k].tn <= 4) {
                SlotState& st = out.slot[traffic[k].tn - 1];
                if (v == VoiceVerdict::Passed) { st = SlotState::Voice; if (calls) calls->slotIds(traffic[k].tn, nowSec, out.slotTg[traffic[k].tn - 1], out.slotIssi[traffic[k].tn - 1]); } else if (st != SlotState::Voice) st = SlotState::Encrypted;   // not decoded: encrypted or unknown, shown muted
            }
        }
    }
    for (int i = 0; i < 4; i++) if (out.slot[i] == SlotState::Idle && (out.tsUsage[i] == 1 || out.tsUsage[i] == 2)) out.slot[i] = SlotState::Control;
    if (!sink) return out;
    if (voiceTn == 0) {   // every clear burst, in the order of the air, nothing else
        for (size_t k = 0; k < traffic.size(); k++)
            if (processTrafficBurst(traffic[k].t.data(), traffic[k].t.data() + 216, scr, gate[k], sink) == VoiceVerdict::Passed) out.voiceTn = traffic[k].tn;
        return out;
    }
    // One timeslot reaches the codec: two calls (or a call and its other slots) through one speech decoder would be heard in turn. The followed
    // timeslot (the one asked for, else the first with a clear burst) goes to the sink frame by frame: its clear traffic bursts, and one silence for
    // every frame without one (a pause, a stolen half slot, a burst too weak or refused, a slot lost where two captures meet), so that the sink
    // plays the air as it was, pauses included, and never runs dry in the middle of a call.
    int ftn = voiceTn > 0 ? voiceTn : 0;
    for (size_t k = 0; k < traffic.size() && !ftn; k++) if (voiceVerdict(gate[k]) == VoiceVerdict::Passed) ftn = traffic[k].tn;
    if (ftn < 1 || ftn > 4) return out;
    constexpr long kMaxLost = 41;   // frames of the slot between two captures filled with silence: one capture; after more the sound has run dry anyway
    if (timeline && timeline->tn != ftn) timeline->next = -1;
    // The frames of one slot reach the sink in strictly forward order: a frame number at or just behind the one expected next has already been
    // given (a second decode of the same air time: a repeated slot of the capture, a capture that overlaps the last one) and is dropped, so that
    // no piece of speech is ever played twice.
    long expect = timeline ? timeline->next : -1;
    for (const SlotFrame& f : frames[ftn]) {
        if (expect >= 0) {
            const long back = (expect - f.speech + VoiceTimeline::kSpeechFrames) % VoiceTimeline::kSpeechFrames;
            if (back > 0 && back <= kMaxLost) { out.voiceDup++; continue; }
        }
        if (timeline && timeline->next >= 0) {
            const long lost = (f.speech - timeline->next + VoiceTimeline::kSpeechFrames) % VoiceTimeline::kSpeechFrames;
            if (lost <= kMaxLost) for (long k = 0; k < lost; k++) { sink->onMissingBurst(); out.voiceGaps++; }
        }
        if (f.burst >= 0 && processTrafficBurst(traffic[(size_t)f.burst].t.data(), traffic[(size_t)f.burst].t.data() + 216, scr, gate[(size_t)f.burst], sink) == VoiceVerdict::Passed) {
            out.voiceTn = ftn;
        } else { sink->onMissingBurst(); out.voiceGaps++; }
        if (timeline) { timeline->tn = ftn; timeline->next = (f.speech + 1) % VoiceTimeline::kSpeechFrames; }
        expect = (f.speech + 1) % VoiceTimeline::kSpeechFrames;
    }
    return out;
}

} // namespace tetra

// ---------------------------------------------------------------- receiver

std::vector<tetra::CarrierPeak> tetra::findCarriers(const cf32* x, size_t n, double fs, double usableBand, size_t maxCarriers) {
    std::vector<CarrierPeak> out;
    const int nfft = cell::pow2At((int)(fs / 1000.0));
    if (nfft < 64 || n < (size_t)nfft * 8) return out;
    const std::vector<float> p0 = cell::welch(x, n, nfft, 300);
    std::vector<double> p((size_t)nfft);
    for (int i = 0; i < nfft; i++) p[(size_t)i] = (p0[(size_t)std::max(0, i - 1)] + p0[(size_t)i] + p0[(size_t)std::min(nfft - 1, i + 1)]) / 3.0;
    const double bin = fs / nfft;
    // the usable band: the middle kUsableBand of the span, where a radio's decimation filter is flat (the span the application steps by);
    // the roll-off outside it is neither searched nor taken into the floor
    const double ub = usableBand > 0 && std::isfinite(usableBand) ? std::min(usableBand, 0.999) : kUsableBand;   // (a fraction: nothing else is refused)
    const int lo = std::max(1, nfft / 2 - (int)(nfft * ub / 2)), hi = std::min(nfft - 2, nfft / 2 + (int)(nfft * ub / 2) - 1);
    // the local floor: the 20th percentile of the usable bins within kFloorWindowHz / 2 of each bin, taken on a grid of nfft / 128 bins and
    // held between the grid points (a carrier is 25 kHz wide and a row of them a few hundred kHz: the floor is the noise between the rows)
    const int half = std::max(8, (int)(kFloorWindowHz / 2 / bin)), step = std::max(1, nfft / 128);
    std::vector<double> floorP((size_t)nfft, 1e-30), tmp;
    for (int g = lo; g <= hi; g += step) {
        const int a = std::max(lo, g - half), b = std::min(hi, g + half);
        tmp.assign(p.begin() + a, p.begin() + b + 1);
        std::nth_element(tmp.begin(), tmp.begin() + (std::ptrdiff_t)(tmp.size() / 5), tmp.end());
        const double f = std::max(1e-30, tmp[tmp.size() / 5]);
        for (int k = g; k <= std::min(hi, g + step - 1); k++) floorP[(size_t)k] = f;
    }
    // 4 dB over the local floor (3.8: 6 dB, which missed carriers below about 4 dB SNR that the demodulator now decodes; the Welch average
    // of up to 300 spectra keeps the noise within a fraction of a dB, and a rise must still be 9 kHz wide)
    auto above = [&](int k) { return p[(size_t)k] >= floorP[(size_t)k] * 2.5; };
    // One rise [i, j] of the spectrum: cut into 25 kHz carriers
    auto emit = [&](int i, int j) {
        const double w = (j - i + 1) * bin;
        if (w >= 9e3 && w <= 400e3) {
            const int nc = std::max(1, (int)std::lround(w / 25e3));
            for (int c = 0; c < nc; c++) {
                const int a = i + (int)std::floor((double)c * (j - i + 1) / nc), b = i + (int)std::floor((double)(c + 1) * (j - i + 1) / nc) - 1;
                double sp = 0, sf = 0, pk = 0, fl = 0;
                for (int k = a; k <= b; k++) { const double v = std::max(0.0, p[(size_t)k] - floorP[(size_t)k]); sp += v; sf += v * k; pk = std::max(pk, p[(size_t)k]); fl += floorP[(size_t)k]; }
                if (sp <= 0) continue;
                fl /= (double)(b - a + 1);
                CarrierPeak cr;
                cr.off = (sf / sp - nfft / 2.0) * bin;
                cr.levelDb = (float)(10 * std::log10(pk / fl));
                cr.powerDb = (float)(10 * std::log10((sp + fl * (b - a + 1)) / (0.375 * nfft) + 1e-30));
                cr.riseOff = ((i + j) / 2.0 - nfft / 2.0) * bin; cr.riseHz = w;
                out.push_back(cr);
            }
        }
    };
    // The rises over the threshold. A strong carrier's skirts (and the spurs of a clean, quantised signal) stay above it far beyond 25 kHz,
    // and the rise was then cut into carriers several kHz off the true centre, out of the frequency correction's reach (3.9: the TX loop
    // test failed): a rise more than 30 dB over the floor is measured again at 30 dB under its own peak.
    auto scan = [&](int lo2, int hi2, const std::function<bool(int)>& on, bool top) {
        int i = lo2;
        while (i <= hi2) {
            if (!on(i)) { i++; continue; }
            int j = i;
            while (j + 1 <= hi2 && (on(j + 1) || (j + 2 <= hi2 && on(j + 2)))) j++;
            double pk = 0, fl = 0;
            for (int k = i; k <= j; k++) { pk = std::max(pk, p[(size_t)k]); fl = std::max(fl, floorP[(size_t)k]); }
            if (top && pk > fl * 1000.0) {
                const double t = pk / 1000.0;
                std::function<bool(int)> strong = [&](int k) { return p[(size_t)k] >= std::max(t, floorP[(size_t)k] * 2.5); };
                int a = i;
                while (a <= j) {   // the parts of the rise over pk / 1000, as rises of their own
                    if (!strong(a)) { a++; continue; }
                    int b = a;
                    while (b + 1 <= j && (strong(b + 1) || (b + 2 <= j && strong(b + 2)))) b++;
                    emit(a, b);
                    a = b + 1;
                }
            } else emit(i, j);
            i = j + 1;
        }
    };
    scan(lo, hi, above, true);
    std::sort(out.begin(), out.end(), [](const CarrierPeak& a, const CarrierPeak& b) { return a.levelDb > b.levelDb; });
    if (maxCarriers > 0 && out.size() > maxCarriers) out.resize(maxCarriers);
    return out;
}

std::vector<std::vector<cf32>> tetra::channelizeMany(const cf32* x, size_t n, double fs, const std::vector<double>& offsets) {
    std::vector<std::vector<cf32>> out(offsets.size());
    if (offsets.empty() || n < 4096) return out;
    std::vector<lora::FdChannelizer::Spec> specs;
    for (double o : offsets) specs.push_back({o, 50e3});   // a 25 kHz channel and its neighbours' skirts: the same passband the single path leaves (+-40 kHz)
    lora::FdChannelizer fd(fs, specs);
    std::vector<lora::ChanBuf> bufs(offsets.size());
    std::vector<lora::ChanBuf*> ptrs;
    for (auto& b : bufs) { b.clear(0); ptrs.push_back(&b); }
    constexpr size_t kChunk = size_t(1) << 20;   // the channeliser keeps only what the next segment needs: the capture is never copied whole
    for (size_t i = 0; i < n; i += kChunk) fd.process(x + i, std::min(kChunk, n - i), ptrs);
    for (size_t k = 0; k < offsets.size(); k++) {
        const size_t m = (size_t)(bufs[k].end() - bufs[k].begin());
        if (m < 64) continue;
        out[k] = channelize(bufs[k].at(bufs[k].begin()), m, fd.outRate(k), 0.0);   // already centred: only the resampling to 4 samples per symbol is left
    }
    return out;
}

namespace {

int64_t wallNow() { return dect2::unixNow(); }

using Carrier = tetra::CarrierPeak;

} // namespace

struct TetraReceiver::Impl {
    mutable std::mutex mu;
    double rateSet = 0, sigOff = 0, centerHz = 0;
    std::function<void(const std::string&)> log;
    std::atomic<uint64_t> gen{0};
    std::atomic<bool> idle{true};
    std::atomic<bool> clearReq{false};
    std::atomic<SpeechFrameSink*> voiceSink{nullptr};
    std::atomic<uint32_t> slotSel{0};   // SlotSelect: mode | slot << 8; the id is kept apart
    std::atomic<uint32_t> slotId{0};
    tetra::SlotSelect slotSelect() const {
        const uint32_t v = slotSel.load();
        tetra::SlotSelect s; s.mode = (tetra::SlotMode)(v & 0xff); s.slot = (int)((v >> 8) & 0xff); s.id = slotId.load();
        return s;
    }
    // per-call encryption state of each carrier (analysis thread only), keyed by the carrier's 25 kHz channel
    std::map<long, tetra::CallTracker> callTrackers;
    tetra::VoiceStreamPicker voicePick;   // the one carrier and timeslot whose voice is played
    std::map<long, tetra::VoiceTimeline> timelines;   // the frames of each carrier across captures (the slots lost between two captures go to the sink as silence)
    // the user's choice (issue #267): the timeslot to listen to (0 the first with clear voice), the carrier (0 any), only the carrier at the dial frequency
    std::atomic<int> wantTn{0};
    std::atomic<double> wantCarrierHz{0.0};
    std::atomic<double> searchBand{tetra::kUsableBand};   // the part of the capture the carrier search covers (setSearchBand)
    std::atomic<bool> single{false};      // Single carrier: only the user's channel is decoded, no search of the span
    std::atomic<double> playCarrierHz{0.0};   // the stream whose voice reached the sink in the last capture (0: none)
    std::atomic<int> playTn{0};
    // A carrier's name across captures. The centre of its rise in the spectrum moves a few kHz from capture to capture (and a carrier near the
    // middle of two 25 kHz grid steps changed name every time, losing its call state and its place in the voice stream), so a carrier within
    // kSameCarrierHz of a known one is that carrier. Analysis thread only.
    static constexpr double kSameCarrierHz = 9e3;
    static constexpr double kSharedFromHz = 4e6;      // from this input rate (and this many carriers) the carriers are cut out with one shared FFT
    static constexpr size_t kSharedMinCarriers = 3;
    std::vector<std::pair<double, long>> carrierNames;
    long nextName = 1;
    long callKey(double center, double off) {
        const double f = (center > 0 ? center : 0.0) + off;
        for (auto& c : carrierNames) if (std::fabs(c.first - f) < kSameCarrierHz) { c.first += 0.25 * (f - c.first); return c.second; }
        if (carrierNames.size() > 96) carrierNames.erase(carrierNames.begin());
        carrierNames.push_back({f, nextName});
        return nextName++;
    }
    // capture (analysis thread)
    std::vector<cf32> acc;
    size_t want = 0;
    uint64_t accGen = 0;
    double accCenter = 0;
    uint64_t total = 0;
    double levelAcc = 0; uint64_t levelN = 0;
    double lastPubSec = -1;
    // worker
    std::thread th;
    std::condition_variable cv;
    bool stop = false, jobReady = false;
    std::vector<cf32> job;
    double jobCenter = 0, jobRate = 0, jobSec = 0;
    uint64_t jobGen = 0;
    // results (under mu)
    TetraTelemetry tel, pub;

    Impl() { th = dect2::namedThread("onair-tetra", [this] { run(); }); }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    void event(const std::string& s, double sec) {
        TetraEvent e; e.wallTime = wallNow(); e.sec = sec; e.text = s;
        tel.events.insert(tel.events.begin(), e);
        if (tel.events.size() > kTetraLogMax) tel.events.resize(kTetraLogMax);
        if (log) log("tetra: " + s);
    }

    void publish() { tel.seq++; pub = tel; }

    void run() {
        for (;;) {
            std::vector<cf32> x;
            double center, rate, sec; uint64_t g;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stop || jobReady; });
                if (stop) return;
                x.swap(job); jobReady = false;
                center = jobCenter; rate = jobRate; sec = jobSec; g = jobGen;
            }
            process(x, center, rate, sec, g);
            idle = true;
        }
    }

    // 25 kHz humps in the spectrum: runs of bins over the floor, split into 25 kHz carriers
    std::vector<Carrier> carriers(const std::vector<cf32>& x, double fs) { return tetra::findCarriers(x.data(), x.size(), fs, searchBand.load()); }

    void process(const std::vector<cf32>& x, double center, double fs, double sec, uint64_t g) {
        std::vector<Carrier> car;
        if (single.load()) {
            // one 25 kHz channel at the dial frequency: the others are not decoded (no copies of the call from other sites, and the capture is
            // decoded in a fraction of the time, so fewer samples are dropped while it is)
            double off0;
            { std::lock_guard<std::mutex> lk(mu); off0 = sigOff; }
            if (std::fabs(off0) < fs * 0.45) { Carrier c; c.off = off0; c.levelDb = 0; c.powerDb = 0; car.push_back(c); }
        } else car = carriers(x, fs);
        // A wide capture with several carriers: one shared FFT brings them all to 72 kHz (a mixer and a filter over the whole capture for each
        // carrier would take longer than the capture lasts). A narrow capture keeps the per-carrier path, bit for bit as before.
        std::vector<std::vector<cf32>> pre;
        const bool shared = !single.load() && fs >= kSharedFromHz && car.size() >= kSharedMinCarriers;
        if (shared) { std::vector<double> offs; for (const auto& c : car) offs.push_back(c.off); pre = tetra::channelizeMany(x.data(), x.size(), fs, offs); }
        const int tnWant = wantTn.load();
        const double carWant = wantCarrierHz.load();
        bool gaveVoice = false;
        struct Found { Carrier c; tetra::CarrierDecode d; double freq; };
        std::vector<Found> found;
        std::vector<TetraCarrierSeen> seen;
        std::vector<double> seenRise;   // per entry of seen: the rise it was cut from (NaN: a TETRA carrier)
        voicePick.newCapture();
        // One carrier's voice per capture: what each candidate gives is collected first and only the chosen one is handed to the sink, so a
        // carrier that gave silence is never heard in front of (or besides) the carrier that gave the speech
        tetra::FrameBuffer fallback;
        bool haveFallback = false, voiced = false;
        for (size_t ci = 0; ci < car.size(); ci++) {
            const auto& c = car[ci];
            if (gen.load() != g) return;   // retuned meanwhile: these samples belong to another frequency
            int kMcc = -1, kMnc = -1, kCc = -1;
            tetra::SlotInfo known[4];   // what this carrier's cell showed so far, for following a talkgroup / ISSI
            {
                std::lock_guard<std::mutex> lk(mu);
                const double f = (center > 0 ? center : 0.0) + c.off;
                for (const auto& t : tel.cells) if (std::fabs(t.freqHz - f) < 5e3 && t.mcc >= 0) {
                    kMcc = t.mcc; kMnc = t.mnc; kCc = t.colourCode;
                    for (int i = 0; i < 4; i++) known[i] = t.slots[i];
                    break;
                }
            }
            const std::vector<cf32> s = shared && ci < pre.size() && !pre[ci].empty() ? std::move(pre[ci]) : tetra::channelize(x.data(), x.size(), fs, c.off);
            if (callTrackers.size() > 64) { callTrackers.clear(); timelines.clear(); }
            const long key = callKey(center, c.off);
            int voiceTn = 0;
            bool voice = !voiced && voiceSink.load() && voicePick.offer(key, sec, voiceTn);
            if (voice && carWant > 0 && std::fabs((center > 0 ? center : 0.0) + c.off - carWant) > 12.5e3) voice = false;   // another carrier was chosen
            if (voice && tnWant >= 1 && tnWant <= 4) voiceTn = tnWant;   // the chosen timeslot only (the TETRA "listen to" row)
            else if (voice) {   // otherwise the listener's slot rule (tetra_slots.h); the encryption gate in decodeCarrier still decides what is decoded
                const int want = tetra::pickSlot(slotSelect(), known, (double)wallNow());
                if (want == tetra::kFollowNothing) voice = false; else if (want > 0) voiceTn = want;
            }
            tetra::FrameBuffer buf;
            tetra::CarrierDecode d = tetra::decodeCarrier(s.data(), s.size(), kMcc, kMnc, kCc, voice ? &buf : nullptr, &callTrackers[key], (double)wallNow(), voiceTn,
                                                          voice ? &timelines[key] : nullptr);
            if (voice) {
                // a chosen timeslot is reported as passed even when it was quiet: the carrier is followed only while its voice is clear
                voicePick.heard(key, tnWant && d.voicePassed == 0 ? 0 : d.voiceTn, sec);
                SpeechFrameSink* vs = voiceSink.load();
                if (vs && d.voiceTn > 0) {   // speech: this carrier is the capture's voice; told at once, not after the other carriers (the sink may hold it)
                    if (d.voicePassed > 0) { playCarrierHz = (center > 0 ? center : 0.0) + c.off + d.cfoHz; playTn = d.voiceTn; gaveVoice = true; }
                    buf.flush(*vs);
                    vs->onCaptureEnd();
                    voiced = true;
                } else if (vs && buf.size() > 0 && !haveFallback) { fallback = std::move(buf); haveFallback = true; }
            }
            const double freq = (center > 0 ? center : 0.0) + c.off + d.cfoHz;
            TetraCarrierSeen cs; cs.freqHz = d.tetra ? freq : (center > 0 ? center : 0.0) + c.off; cs.levelDb = c.levelDb; cs.synced = d.synced;
            const bool isTetra = d.tetra && (d.synced || d.blocksOk > 0);   // the slot structure alone is not enough: a TEDS QAM carrier can show one
            if (isTetra) { cs.kind = 1; cs.bwHz = 25e3; cs.p8Slots = d.p8Slots; }
            seen.push_back(cs);
            seenRise.push_back(isTetra ? std::nan("") : (c.riseHz > 0 ? c.riseOff : c.off));
            if (d.tetra && (d.synced || d.scrambler)) found.push_back({c, std::move(d), freq});
        }
        if (!voiced) {   // no carrier gave speech: the silence of the first followed one (pauses keep their length), then the end of the capture
            if (SpeechFrameSink* vs = voiceSink.load()) { if (haveFallback) fallback.flush(*vs); vs->onCaptureEnd(); }
        }
        if (!gaveVoice) { playCarrierHz = 0.0; playTn = 0; }
        identifyOthers(x, center, fs, car, seen, seenRise, g);
        std::lock_guard<std::mutex> lk(mu);
        if (gen.load() != g) return;
        if (clearReq.exchange(false)) { tel.cells.clear(); tel.events.clear(); tel.sds.clear(); tel.blocksOk = tel.blocksBad = 0; }
        tel.blocks++;
        tel.carriers = seen;
        for (auto& fr : found) merge(fr.c, fr.d, fr.freq, center, sec);
        std::stable_sort(tel.cells.begin(), tel.cells.end(), [](const TetraCell& a, const TetraCell& b) { return a.lastSec > b.lastSec; });
        if (tel.cells.size() > kTetraMax) tel.cells.resize(kTetraMax);
        refreshState(sec);
        publish();
    }

    // TEDS (3.9): the rises of the spectrum that gave no TETRA slot structure are identified from their physical layer (tetra_teds.h): a QAM
    // carrier (8 sub-carriers per 25 kHz, 2 400 symbols/s) becomes one entry with its bandwidth, a pi/8-D8PSK carrier is marked as such.
    // Identity only: nothing of a QAM carrier is decoded. At most kIdentifyMax rises per capture, the strongest first.
    static constexpr size_t kIdentifyMax = 8;
    void identifyOthers(const std::vector<cf32>& x, double center, double fs, const std::vector<Carrier>& car, std::vector<TetraCarrierSeen>& seen,
                        const std::vector<double>& rise, uint64_t g) {
        std::vector<size_t> todo;   // the first entry of each rise not decoded as TETRA
        for (size_t i = 0; i < seen.size() && i < rise.size(); i++) {
            if (std::isnan(rise[i])) continue;
            bool dup = false;
            for (size_t j : todo) if (std::fabs(rise[j] - rise[i]) < 1.0) dup = true;
            // a rise that also holds a TETRA carrier is a row of carriers, not one wide QAM carrier
            for (size_t j = 0; j < seen.size() && j < car.size(); j++) if (std::isnan(rise[j]) && car[j].riseHz > 0 && std::fabs(car[j].riseOff - rise[i]) < 1.0) dup = true;
            if (!dup) todo.push_back(i);
        }
        if (todo.empty()) return;
        if (todo.size() > kIdentifyMax) todo.resize(kIdentifyMax);
        const tetra::TedsIdentifier id(x.data(), x.size(), fs);
        std::vector<TetraCarrierSeen> out;
        std::vector<char> drop(seen.size(), 0);
        for (size_t i : todo) {
            if (gen.load() != g) return;
            const double w = i < car.size() && car[i].riseHz > 0 ? car[i].riseHz : 25e3;
            const tetra::TedsCarrierClass cc = id.classify(rise[i], w);
            if (cc.kind == tetra::CarrierKind::Qam) {
                for (size_t j = 0; j < seen.size() && j < rise.size(); j++) if (!std::isnan(rise[j]) && std::fabs(rise[j] - rise[i]) < 1.0) drop[j] = 1;
                TetraCarrierSeen q;
                q.freqHz = (center > 0 ? center : 0.0) + rise[i];
                q.levelDb = seen[i].levelDb;
                q.kind = 3; q.bwHz = cc.bwHz; q.subcarriers = cc.subcarriers; q.qamOrder = cc.qamOrder; q.orderSure = cc.orderSure;
                out.push_back(q);
            } else if (cc.kind != tetra::CarrierKind::Unknown) {
                seen[i].kind = (int)cc.kind; seen[i].bwHz = cc.bwHz;
            }
        }
        std::vector<TetraCarrierSeen> keep;
        for (size_t j = 0; j < seen.size(); j++) if (!drop[j]) keep.push_back(seen[j]);
        keep.insert(keep.end(), out.begin(), out.end());
        seen.swap(keep);
    }

    void refreshState(double sec) {
        const TetraCell* best = nullptr;
        bool recent = false;
        for (const auto& c : tel.cells) {
            if (sec - c.lastSec < 30) recent = true;
            if (sec - c.lastSec < 30 && (!best || c.levelDb > best->levelDb)) best = &c;
        }
        tel.dataValid = recent;
        tel.state = (tel.timeSec > 0 || tel.blocks) ? (recent ? 2 : 1) : 0;
        if (best) { tel.cfoHz = best->cfoHz; tel.snrDb = best->snrDb; }
    }

    static std::string freqText(double hz) { char b[32]; snprintf(b, sizeof b, "%.4f MHz", hz / 1e6); return b; }

    void merge(const Carrier& cr, const tetra::CarrierDecode& d, double freq, double center, double sec) {
        const int mcc = d.synced ? d.sync.mcc : -1, mnc = d.synced ? d.sync.mnc : -1, cc = d.synced ? d.sync.colourCode : -1;
        TetraCell* t = nullptr;
        for (auto& c : tel.cells) {
            if (std::fabs(c.freqHz - freq) > 5e3) continue;
            if (d.synced && (c.mcc != mcc || c.mnc != mnc || c.colourCode != cc)) continue;
            t = &c; break;
        }
        if (!t && !d.synced) return;   // an identity is needed for a new entry
        char b[320];
        const bool isNew = !t;
        if (!t) {
            tel.cells.push_back(TetraCell());
            t = &tel.cells.back();
            t->firstWall = wallNow();
        }
        t->freqHz = freq;
        if (!t->hasSysinfo || !t->isMain) t->nominalHz = freq;
        t->levelDb = cr.powerDb; t->snrDb = d.snrDb;
        t->lastWall = wallNow(); t->lastSec = sec; t->seen++;
        t->bschOk += (uint64_t)d.syncs;
        t->blocksOk += (uint64_t)d.blocksOk; t->blocksBad += (uint64_t)d.blocksBad;
        t->p8Slots += (uint64_t)d.p8Slots; t->p8BlocksOk += (uint64_t)d.p8BlocksOk;
        tel.blocksOk += (uint64_t)d.blocksOk; tel.blocksBad += (uint64_t)d.blocksBad;
        for (int i = 0; i < 4; i++) if (d.tsUsage[i] >= 0) t->tsUsage[i] = d.tsUsage[i];
        for (int i = 0; i < 4; i++) {
            tetra::updateSlot(t->slots[i], d.slot[i], (double)wallNow());
            if (d.slot[i] == tetra::SlotState::Voice) { t->slots[i].talkgroup = d.slotTg[i]; t->slots[i].issi = d.slotIssi[i]; }
            else if (d.slot[i] == tetra::SlotState::Encrypted) t->slots[i].talkgroup = t->slots[i].issi = 0;   // an encrypted call keeps no IDs
        }
        if (d.synced) {
            const tetra::Sync& s = d.sync;
            t->mcc = s.mcc; t->mnc = s.mnc; t->colourCode = s.colourCode; t->systemCode = s.systemCode; t->sharingMode = s.sharingMode;
            t->tsReservedFrames = s.tsReservedFrames; t->uplaneDtx = s.uplaneDtx; t->frame18Ext = s.frame18Ext; t->lateEntry = s.lateEntry;
            t->nbrBroadcast = s.nbrBroadcast; t->cellLoad = s.cellLoad; t->tn = s.tn; t->fn = s.fn; t->mn = s.mn;
        }
        if (isNew) {
            snprintf(b, sizeof b, "%s: MCC %d MNC %d, colour code %d%s", freqText(freq).c_str(), t->mcc, t->mnc, t->colourCode, center > 0 ? "" : " (offset from the file's centre)");
            event(b, sec);
        }
        const bool encBefore = t->encryption();
        for (const auto& m : d.macs) {
            for (const auto& c : m.cmce) {          // clear SDS and status: the message list, the positions on the map
                tetra::SdsMsg sm;
                const bool ok = c.bits.size() >= 5 && (tetra::parseSdsData(c.bits, sm) || tetra::parseStatus(c.bits, sm));
                if (!ok || !sm.valid) continue;
                TetraSdsRec r;
                r.wallTime = (int64_t)time(nullptr); r.freqHz = freq; r.fromIssi = sm.callingIssi; r.toAddr = c.addr; r.kind = sm.kind; r.text = sm.text;
                r.hasPos = sm.hasPos; r.lat = sm.lat; r.lon = sm.lon;
                auto same = std::find_if(tel.sds.begin(), tel.sds.end(), [&](const TetraSdsRec& x) { return x.fromIssi == r.fromIssi && x.text == r.text && x.toAddr == r.toAddr; });
                if (same != tel.sds.end()) { r.count = same->count + 1; tel.sds.erase(same); }
                tel.sds.insert(tel.sds.begin(), std::move(r));
                if (tel.sds.size() > 300) tel.sds.pop_back();
                char b2[220];
                snprintf(b2, sizeof b2, "SDS %s from %d to %d: %s", sm.kind.c_str(), sm.callingIssi, c.addr, sm.text.c_str());
                event(b2, sec);
            }
            t->encryptedPdus += (uint64_t)m.encrypted;
            for (int k = 0; k < 8; k++) t->clearPdus += (uint64_t)m.byPd[k];
            if (m.sysinfo) {
                const tetra::Sysinfo& s = m.si;
                const bool first = !t->hasSysinfo || t->la != s.la || t->mainCarrier != s.mainCarrier;
                t->bnchOk++;
                t->hasSysinfo = true;
                t->mainCarrier = s.mainCarrier; t->band = s.band; t->offset = s.offset; t->duplex = s.duplex; t->reverse = s.reverse;
                t->numScch = s.numScch; t->msTxPwrMax = s.msTxPwrMax; t->rxLevAccessMin = s.rxLevAccessMin; t->accessParam = s.accessParam; t->dlTimeout = s.dlTimeout;
                t->cckFlag = s.cckFlag; t->hyperframeOrCck = s.hyperframeOrCck; t->optFlag = s.optFlag; t->optValue = s.optValue;
                tetra::ExtServices es;
                if (tetra::parseExtServices(s.optFlag, s.optValue, es)) { tetra::TedsCaps caps{t->tedsD8psk, t->tedsQamMask}; caps.add(es); t->tedsD8psk = caps.d8psk; t->tedsQamMask = caps.qamMask; }
                t->la = s.la; t->subscriberClass = s.subscriberClass; t->bsService = s.bsService;
                t->aie = (s.bsService & tetra::kSvcAirEncryption) != 0;
                const tetra::Security sc = tetra::parseSecurity(s);
                if (sc.valid) { t->secValid = true; t->authRequired = sc.authRequired; t->secClass1 = sc.class1; t->secClass2 = sc.class2; t->secClass3 = sc.class3; }
                if (!t->aie) { t->secClass2 = false; t->secClass3 = false; }   // a class kept from an earlier broadcast: the service bit is the newest word
                t->mainHz = tetra::dlCarrierHz(s.band, s.mainCarrier, s.offset);
                t->uplinkHz = tetra::ulCarrierHz(s.band, s.mainCarrier, s.offset, s.duplex, s.reverse);
                // the main carrier: the announced frequency is this one, or near enough for a radio whose clock is off (up to about 150 ppm)
                // when no other carrier of the cell is nearer
                const double dm = std::fabs(t->mainHz - freq);
                bool nearer = false;
                for (const auto& o : tel.cells)
                    if (&o != t && o.mcc == t->mcc && o.mnc == t->mnc && o.colourCode == t->colourCode && std::fabs(o.freqHz - t->mainHz) < dm) nearer = true;
                const bool wasMain = t->isMain;
                t->isMain = center <= 0 || dm < 8e3 || (dm < 60e3 && !nearer);
                if (t->isMain) t->nominalHz = t->mainHz;
                t->cfoHz = center > 0 ? freq - t->nominalHz : d.cfoHz;
                if (t->isMain && !wasMain && center > 0 && dm >= 3e3) {
                    snprintf(b, sizeof b, "%s: heard %.1f kHz from the frequency the cell announces: the radio's frequency is about %.0f ppm off",
                             freqText(t->mainHz).c_str(), (freq - t->mainHz) / 1e3, (freq - t->mainHz) / t->mainHz * 1e6);
                    event(b, sec);
                }
                if (first) {
                    snprintf(b, sizeof b, "%s: LA %d, main carrier %d (%s), uplink %s, services: %s", freqText(freq).c_str(), s.la, s.mainCarrier,
                             freqText(t->mainHz).c_str(), t->uplinkHz > 0 ? freqText(t->uplinkHz).c_str() : "unknown", tetra::serviceText(s.bsService).c_str());
                    event(b, sec);
                }
            }
            for (const auto& nw : m.nwrk) {
                const bool first = !t->hasNwrk || t->neighbours.size() != nw.nbrs.size();
                t->hasNwrk = true;
                t->reselect = nw.reselect;
                if (nw.hasTime) t->netTime = tetra::networkTimeText(nw.time48);
                std::vector<TetraNeighbourInfo> v;
                for (const auto& nb : nw.nbrs) {
                    TetraNeighbourInfo ni;
                    ni.cellId = nb.cellId; ni.carrier = nb.carrier; ni.la = nb.la; ni.cellLoad = nb.cellLoad; ni.synced = nb.synced; ni.mcc = nb.mcc; ni.mnc = nb.mnc;
                    if (nb.hasExt) ni.freqHz = tetra::dlCarrierHz(nb.band, nb.carrier, nb.offset);
                    else if (t->hasSysinfo) ni.freqHz = tetra::dlCarrierHz(t->band, nb.carrier, t->offset);
                    v.push_back(ni);
                }
                // the cell may send its neighbours over several broadcasts: keep every one heard
                for (const auto& ni : v) {
                    bool have = false;
                    for (auto& o : t->neighbours) if (o.cellId == ni.cellId && o.carrier == ni.carrier) { o = ni; have = true; }
                    if (!have) t->neighbours.push_back(ni);
                }
                if (first && !v.empty()) {
                    std::string l;
                    for (const auto& ni : v) { char e[64]; snprintf(e, sizeof e, "%s%s (LA %d)", l.empty() ? "" : ", ", ni.freqHz > 0 ? freqText(ni.freqHz).c_str() : "?", ni.la); l += e; }
                    snprintf(b, sizeof b, "%s: neighbour cells %s", freqText(freq).c_str(), l.c_str());
                    event(b, sec);
                }
            }
        }
        if (!t->hasSysinfo) t->cfoHz = 0;
        if (!encBefore && t->encryption()) {
            static const char* const kind[] = {"", "", "class 2", "class 3", "service available"};
            snprintf(b, sizeof b, "%s: air interface encryption %s%s%s (nothing is decrypted)", freqText(freq).c_str(), t->airEnc() >= 2 ? "advertised: " : "", t->airEnc() >= 2 ? kind[t->airEnc()] : "not advertised",
                     t->encryptedPdus ? ", some signalling PDUs carry an encryption mode" : "");
            event(b, sec);
        }
    }
};

TetraReceiver::TetraReceiver() : p_(new Impl) {}
TetraReceiver::~TetraReceiver() = default;

void TetraReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = rate;
    p_->tel.inputRate = rate;
    p_->want = (size_t)(rate * TetraReceiver::kCaptureSec);
    p_->gen++;
}

void TetraReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->sigOff = hz; p_->gen++; }
void TetraReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (std::fabs(hz - p_->centerHz) > 1) p_->gen++;
    p_->centerHz = hz; p_->tel.centerHz = hz;
}
bool TetraReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= tetraTuning().minSampleRate - 1; }
void TetraReceiver::setVoiceSink(SpeechFrameSink* s) { p_->voiceSink.store(s); }
void TetraReceiver::setVoiceTimeslot(int tn) { p_->wantTn = tn >= 1 && tn <= 4 ? tn : 0; }
void TetraReceiver::setVoiceCarrier(double hz) { p_->wantCarrierHz = hz > 0 ? hz : 0.0; }
void TetraReceiver::setSearchBand(double f) { if (f > 0 && std::isfinite(f)) p_->searchBand.store(std::min(f, 0.999)); }
double TetraReceiver::searchBand() const { return p_->searchBand.load(); }
void TetraReceiver::setSingleCarrier(bool on) { if (p_->single.exchange(on) != on) p_->gen++; }
bool TetraReceiver::singleCarrier() const { return p_->single.load(); }
double TetraReceiver::voiceCarrierHz() const { return p_->playCarrierHz.load(); }
int TetraReceiver::voiceTimeslot() const { return p_->playTn.load(); }
void TetraReceiver::setVoiceSlot(const SlotSelect& s) {
    p_->slotId.store(s.id);
    p_->slotSel.store((uint32_t)s.mode | ((uint32_t)(s.slot & 0xff) << 8));
}
void TetraReceiver::reset() { p_->gen++; }
void TetraReceiver::clearCells() { std::lock_guard<std::mutex> lk(p_->mu); p_->tel.cells.clear(); p_->tel.events.clear(); p_->tel.blocksOk = p_->tel.blocksBad = 0; p_->publish(); }
void TetraReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool TetraReceiver::busy() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->idle.load() || p_->jobReady; }

// the cells and the event log already stay through reset() and configure() (the sample clock runs on): only the user's Clear empties them
void TetraReceiver::clearResults() { clearCells(); }
std::string TetraReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->tel.cells, p_->tel.events);
}
bool TetraReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    TetraTelemetry l;
    if (!results::unpack(blob, t0, l.cells, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->tel.timeSec - t0;    // onto this run's clock
    for (auto& c : l.cells) c.lastSec += dt;
    for (auto& e : l.events) e.sec += dt;
    p_->tel.cells = std::move(l.cells); p_->tel.events = std::move(l.events);
    p_->publish();
    return true;
}
bool TetraReceiver::telemetry(TetraTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void TetraReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    double rate, center;
    { std::lock_guard<std::mutex> lk(m.mu); rate = m.rateSet; center = m.centerHz; }
    if (rate < tetraTuning().minSampleRate - 1 || m.want == 0) return;
    {   // branch-free; the same additions in the same order as the loop with std::norm
        double lv = m.levelAcc;
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real(), im = x[i].imag();
            const float p = re * re + im * im;
            lv += (std::isfinite(re) && std::isfinite(im)) ? p : 0.f;
        }
        m.levelAcc = lv;
    }
    m.levelN += n;
    m.total += n;
    const uint64_t g = m.gen.load();
    if (g != m.accGen) { m.acc.clear(); m.accGen = g; }
    // The next capture is gathered while the last one is decoded, so the captures follow each other without a gap (voice is heard
    // whole). Samples are dropped only when a capture is complete and the decoder is still busy, i.e. when it is slower than real time.
    for (size_t i = 0; i < n;) {
        if (m.acc.empty()) { m.accCenter = center; m.acc.reserve(m.want); }
        const size_t take = std::min(n - i, m.want - m.acc.size());
        {   // one resize, then plain stores (push_back checked the capacity for every sample)
            const size_t at = m.acc.size();
            m.acc.resize(at + take);
            cf32* __restrict d = m.acc.data() + at;
            for (size_t k = 0; k < take; k++) {
                const cf32 v = x[i + k];
                d[k] = (std::isfinite(v.real()) && std::isfinite(v.imag())) ? v : cf32(0, 0);
            }
        }
        i += take;
        if (m.acc.size() < m.want || !m.idle.load()) break;
        std::lock_guard<std::mutex> lk(m.mu);
        m.job.swap(m.acc); m.acc.clear();
        m.jobCenter = m.accCenter; m.jobRate = rate; m.jobSec = (double)(m.total - n + i) / rate; m.jobGen = g;
        m.jobReady = true; m.idle = false;
        m.cv.notify_one();
    }
    const double now = (double)m.total / rate;
    if (now - m.lastPubSec >= 0.5) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.tel.timeSec = now;
        m.tel.levelDb = m.levelN ? (float)(10 * std::log10(m.levelAcc / (double)m.levelN + 1e-20)) : -200.f;
        m.levelAcc = 0; m.levelN = 0;
        m.lastPubSec = now;
        m.refreshState(now);
        if (m.tel.state == 0) m.tel.state = 1;
        m.publish();
    }
}

} // namespace dect2
