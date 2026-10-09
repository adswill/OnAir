#include "dect2/atsc_rx.h"
#include "dect2/atsc.h"
#include "dect2/atsc_gen.h"   // rrcValue
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "dect2/dsp_compat.h"
#include "dect2/platform.h"
#include "dect2/simd.h"
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
#endif

namespace dect2 {
namespace atsc {

// stage A filter for outputs k0..k1, safe to split across threads
DECT2_MULTIVERSION void pfbBlock(const float* taps, int J, int NPH, const float* re, const float* im, int64_t base,
                                 double pos0, double step, size_t k0, size_t k1, float* outRe, float* outIm) {
    const int T = 2 * J;
    for (size_t k = k0; k < k1; k++) {
        const double pos = pos0 + (double)k * step;
        int64_t i0 = (int64_t)std::floor(pos);
        int phi = (int)((pos - (double)i0) * NPH + 0.5);
        if (phi >= NPH) { phi = 0; i0++; }
        const float* tp = &taps[(size_t)phi * T];
        const size_t b0 = (size_t)(i0 - base - J + 1);
        const float* pr = &re[b0];
        const float* pi_ = &im[b0];
        float sr = 0, si = 0;
        int j = 0;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
        float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0), br = ar, bi = ai;   // two partial sums per array: the chain of dependent multiply-adds is the limit
        for (; j + 8 <= T; j += 8) {
            const float32x4_t t4 = vld1q_f32(tp + j), u4 = vld1q_f32(tp + j + 4);
            ar = vfmaq_f32(ar, vld1q_f32(pr + j), t4);
            ai = vfmaq_f32(ai, vld1q_f32(pi_ + j), t4);
            br = vfmaq_f32(br, vld1q_f32(pr + j + 4), u4);
            bi = vfmaq_f32(bi, vld1q_f32(pi_ + j + 4), u4);
        }
        for (; j + 4 <= T; j += 4) {
            const float32x4_t t4 = vld1q_f32(tp + j);
            ar = vfmaq_f32(ar, vld1q_f32(pr + j), t4);
            ai = vfmaq_f32(ai, vld1q_f32(pi_ + j), t4);
        }
        sr = vaddvq_f32(vaddq_f32(ar, br)); si = vaddvq_f32(vaddq_f32(ai, bi));
#elif defined(__GNUC__) && !defined(DECT2_NO_SIMD)
        // arrays didn't vectorise, vector type does
        typedef float v8 __attribute__((vector_size(32)));
        v8 ar = {}, ai = {};
        for (; j + 8 <= T; j += 8) {
            v8 t8, r8, i8;
            std::memcpy(&t8, tp + j, sizeof t8); std::memcpy(&r8, pr + j, sizeof r8); std::memcpy(&i8, pi_ + j, sizeof i8);
            ar += r8 * t8; ai += i8 * t8;
        }
        sr = ((ar[0] + ar[4]) + (ar[1] + ar[5])) + ((ar[2] + ar[6]) + (ar[3] + ar[7]));
        si = ((ai[0] + ai[4]) + (ai[1] + ai[5])) + ((ai[2] + ai[6]) + (ai[3] + ai[7]));
#endif
        for (; j < T; j++) { sr += pr[j] * tp[j]; si += pi_[j] * tp[j]; }
        outRe[k] = sr; outIm[k] = si;
    }
}

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kPilotHz = -(3.0e6 - 0.309440559e6);   // pilot relative to the centre of the channel
constexpr int kSegSamples = 2 * kSegSyms;               // 1664 samples at 2 per symbol
constexpr int kNF = 24;                                 // equaliser taps after the cursor (future samples)
constexpr int kNP = 103;                                // taps before the cursor
constexpr int kL = kNF + kNP + 1;                       // 128
constexpr int kInterpJ = 8;                             // symbol interpolator half length
constexpr int kInterpPh = 64;

inline cf32 expj(double a) { return cf32((float)std::cos(a), (float)std::sin(a)); }

// cheaper atan2, good enough for the pll
inline float fastAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    if (mx == 0.f) return 0.f;
    const float a = mn / mx, s = a * a;
    float r = (((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s) * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

// Solves (A + lambda I) w = b for a symmetric positive definite A (dimension n, row-major), in place
bool solveSpd(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int j = 0; j < n; j++) {
        double d = A[j * n + j];
        for (int k = 0; k < j; k++) d -= A[j * n + k] * A[j * n + k];
        if (d <= 1e-12) return false;
        d = std::sqrt(d);
        A[j * n + j] = d;
        for (int i = j + 1; i < n; i++) {
            double s = A[i * n + j];
            for (int k = 0; k < j; k++) s -= A[i * n + k] * A[j * n + k];
            A[i * n + j] = s / d;
        }
    }
    for (int i = 0; i < n; i++) { double s = b[i]; for (int k = 0; k < i; k++) s -= A[i * n + k] * b[k]; b[i] = s / A[i * n + i]; }
    for (int i = n - 1; i >= 0; i--) { double s = b[i]; for (int k = i + 1; k < n; k++) s -= A[k * n + i] * b[k]; b[i] = s / A[i * n + i]; }
    return true;
}
}

struct Core {
    // ---- configuration
    double fin = 0, fs2 = 2.0 * kSymbolRate;
    bool ok = false;
    // ---- stage A: matched filter + resampler (complex in -> 2 samples/symbol out)
    int J = 24;
    int NPH = 128;
    std::vector<float> taps;
    std::vector<float> inRe, inIm;   // input samples, split
    std::vector<float> pfRe, pfIm;
    bool splitA = std::thread::hardware_concurrency() >= 2;
    uint64_t inBase = 0;
    double pos = 0, step = 0;
    // ---- stage B: pilot loop
    double theta = 0, omega = 0, omega0 = 0;
    cf32 ph{1, 0}, rot{1, 0}, pf{0, 0};
    double pfA = 0, pfAt = 0;
    bool tracking = false;
    int goodRun = 0, badRun = 0;
    int pllCnt = 0;
    double dc = 0, pw = 0;
    bool dcInit = false;
    float pilotRel = 0;
    bool pilotLock = false;
    int pilotGood = 0;
    // ---- stage C: symbol clock
    std::vector<float> d;           // 2 samples/symbol, normalised
    uint64_t dBase = 0;
    uint64_t accIdx = 0;            // next sample for the segment sync correlation
    std::vector<float> acc;         // correlation per sample position modulo 1664
    uint64_t lastSearch = 0;
    int lastPeak = -1, peakRun = 0;
    bool seg = false;
    double tau = 0, rate = 2.0, rr = 0;
    double tauSync[4] = {};
    int segPos = 0;
    std::vector<float> segSyms;
    int lowQ = 0;
    double syncQ = 0;
    std::vector<float> interp;      // [kInterpPh][2*kInterpJ]
    // ---- stage D: fields
    std::vector<float> raw;         // unequalised symbols since lock
    uint64_t rawBase = 0;           // absolute symbol index of raw[0]
    uint64_t symAbs = 0;
    uint64_t segAbs = 0;            // completed segments since the symbol clock locked
    bool fieldLock = false;
    int64_t fieldStartSeg = -1;     // absolute segment index of the next field sync
    int fsMisses = 0;
    int parity = 0;                 // 1/2: last detected polarity
    int64_t lastFsSeg = -1;
    bool fsSeen = false;
    FieldDecoder dec;
    std::function<void(const uint8_t*, size_t, double)> cb;
    std::function<void(const std::vector<float>&)> levelTap;
    // ---- statistics
    AtscTelemetry t;
    std::vector<float> lastTaps;
    std::atomic<int> tsFlow{0};
    std::chrono::steady_clock::time_point lastPub;
    std::vector<float> levelsOut;
    // equaliser working state
    std::vector<float> w;
    float bias = 0;
    double gainCorr = 1.0;   // the equaliser (a minimum-mean-square one) shrinks its output a little in noise: this puts the levels back where the decoder expects them

    void configure(double rateHz) {
        fin = rateHz;
        ok = fin >= 6.5e6;
        if (!ok) return;
        step = fin / fs2;
        J = (int)std::ceil(2.4e-6 * fin) + 2;
        taps.assign((size_t)NPH * 2 * J, 0.f);
        const double Rb = kSymbolRate / 2.0;
        for (int p = 0; p < NPH; p++) {
            const double mu = (double)p / NPH;
            for (int j = -J + 1; j <= J; j++) {
                const double x = mu - j;
                const double win = std::cos(0.5 * kPi * (x) / (J + 0.5));
                taps[(size_t)p * 2 * J + (j + J - 1)] = (float)(Rb / fin * rrcValue(x / fin) * (getenv("ATSC_NOWIN") ? 1.0 : win * win));
            }
        }
        interp.assign((size_t)kInterpPh * 2 * kInterpJ, 0.f);
        for (int p = 0; p < kInterpPh; p++) {
            const double mu = (double)p / kInterpPh;
            double sum = 0;
            for (int j = -kInterpJ + 1; j <= kInterpJ; j++) {
                const double x = j - mu;
                const double s = x == 0 ? 1.0 : std::sin(kPi * x * 0.92) / (kPi * x * 0.92);
                const double wv = 0.5 + 0.5 * std::cos(kPi * x / (kInterpJ + 0.5));
                const float v = (float)(s * wv);
                interp[(size_t)p * 2 * kInterpJ + (j + kInterpJ - 1)] = v;
                sum += v;
            }
            for (int j = 0; j < 2 * kInterpJ; j++) interp[(size_t)p * 2 * kInterpJ + j] /= (float)sum;
        }
        resetAcq();
        reset();
        startWorker();
    }

    // ---------------------------------------------------------------- stage 0: where is the pilot?
    // The pilot loop only pulls in a few tens of kHz and needs a pilot strong enough for its wide acquisition filter. Before it starts,
    // the pilot line is looked for in an averaged spectrum of the whole sample band: a channel that is not centred (a recording made
    // beside the channel) is then mixed to the centre, and a weak pilot (an echo that notches the band edge) is found to a fraction of a
    // kHz, so the loop can start narrow. Repeated while nothing locks (a retune, a signal that appears later).
    bool acqDone = false, acqPilot = false;
    std::vector<cf32> acqBuf;
    double shiftHz = 0, acqLevel = 0;   // mixer frequency (the channel's offset from the centre), pilot amplitude relative to the signal
    uint64_t mixN = 0, noLockSamples = 0;
    float weakScale = 1.f;
    void resetAcq() { weakScale = 1.f; acqDone = false; acqPilot = false; acqBuf.clear(); shiftHz = 0; acqLevel = 0; mixN = 0; noLockSamples = 0; }
    int acqLog2() const { int lg = 8; while ((double)(1 << lg) < fin / 1000.0) lg++; return lg; }
    size_t acqNeed() const { return ((size_t)1 << acqLog2()) * 48; }
    void acquirePilot() {
        const int lg = acqLog2(), N = 1 << lg, M = (int)(acqBuf.size() / (size_t)N);
        std::vector<double> P((size_t)N, 0.0);
        std::vector<float> re((size_t)N), im((size_t)N), win((size_t)N);
        for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / N));
        for (int m = 0; m < M; m++) {
            bool finite = true;
            for (int i = 0; i < N; i++) {
                const cf32 v = acqBuf[(size_t)m * N + i];
                re[(size_t)i] = v.real() * win[(size_t)i]; im[(size_t)i] = v.imag() * win[(size_t)i];
                if (!std::isfinite(re[(size_t)i]) || !std::isfinite(im[(size_t)i])) finite = false;
            }
            if (!finite) continue;
            fftSplit(re.data(), im.data(), lg, false);
            for (int k = 0; k < N; k++) P[(size_t)((k + N / 2) % N)] += (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
        }
        std::vector<double> cum((size_t)N + 1, 0.0);
        for (int i = 0; i < N; i++) cum[(size_t)i + 1] = cum[(size_t)i] + P[(size_t)i];
        const double bin = fin / N;
        auto idx = [&](double f) { return (int)std::lround(f / bin) + N / 2; };
        auto meanP = [&](int a, int b) { a = std::max(a, 0); b = std::min(b, N); return b > a ? (cum[(size_t)b] - cum[(size_t)a]) / (b - a) : -1.0; };
        // the whole channel (pilot - 0.1 MHz to pilot + 5.45 MHz) must be inside the band
        const int lo = std::max(idx(-fin / 2 + 0.1e6), 3), hi = std::min(idx(fin / 2 - 5.45e6), N - 45);
        double bestScore = 0;
        int best = -1;
        std::vector<double> nb;
        for (int i = lo; i <= hi; i++) {
            if (std::abs(i - N / 2) <= 3) continue;   // a DC spike is a line too
            if (P[(size_t)i] < P[(size_t)i - 1] || P[(size_t)i] < P[(size_t)i + 1]) continue;
            nb.assign(P.begin() + i + 6, P.begin() + i + 41);   // the data just above the pilot
            std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
            const double med = nb[nb.size() / 2];
            if (med <= 0) continue;
            const double score = P[(size_t)i] / med;
            if (score < 4.0 || score <= bestScore) continue;
            // the shape of a VSB channel: data above the pilot, (nearly) nothing just below it
            const double inside = meanP(idx((i - N / 2) * bin + 0.5e6), idx((i - N / 2) * bin + 5.0e6));
            const double below = meanP(idx((i - N / 2) * bin - 0.6e6), idx((i - N / 2) * bin - 0.35e6));
            if (inside <= 0 || (below >= 0 && inside < 2.0 * below)) continue;
            bestScore = score; best = i;
        }
        acqDone = true;
        if (best < 0) { if (dbg) fprintf(stderr, "[atsc] pilot search: no pilot line in the band\n"); return; }
        const double a = std::log(P[(size_t)best - 1]), b = std::log(P[(size_t)best]), c = std::log(P[(size_t)best + 1]);
        const double den = a - 2 * b + c, delta = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0.0;
        const double fp = ((double)(best - N / 2) + delta) * bin;
        double line = 0;
        for (int k = -2; k <= 2; k++) line += P[(size_t)(best + k)];
        nb.assign(P.begin() + best + 6, P.begin() + best + 41);
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
        line -= 5 * nb[nb.size() / 2];
        acqLevel = std::sqrt(std::max(0.0, line) / std::max(cum[(size_t)N], 1e-30));
        const double newShift = fp - kPilotHz;
        if (dbg) fprintf(stderr, "[atsc] pilot search: line at %.0f Hz (score %.1f, level %.3f), channel offset %.0f Hz\n", fp, bestScore, acqLevel, newShift);
        // a weak pilot has less signal to noise ratio in the loop: the narrow loop is narrowed further, by its power below normal (0.25)
        weakScale = (float)std::max(0.08, std::min(1.0, std::pow(acqLevel / 0.25, 2)));
        const bool restart = !acqPilot || std::fabs(newShift - shiftHz) > 1000.0;
        acqPilot = true;
        if (restart) {
            shiftHz = newShift; mixN = 0;
            reset();
            tracking = true;   // the frequency is known to a fraction of a kHz: start with the narrow loop
        }
    }
    void mixIn(std::vector<cf32>& in) {
        const double w = -2.0 * kPi * shiftHz / fin;
        for (size_t i0 = 0; i0 < in.size(); i0 += 1024) {
            const size_t e = std::min(in.size(), i0 + 1024);
            cf32 c = expj(std::fmod(w * (double)mixN, 2.0 * kPi));
            const cf32 r = expj(w);
            for (size_t i = i0; i < e; i++) { in[i] *= c; c *= r; }
            mixN += e - i0;
        }
    }

    void reset() {
        inRe.clear(); inIm.clear(); inBase = 0; pos = J;
        omega0 = 2.0 * kPi * kPilotHz / fs2; omega = omega0; theta = 0; ph = cf32(1, 0); rot = expj(-omega); pf = cf32(0, 0);
        pfA = 1.0 - std::exp(-1.0 / (fs2 * 5e-6)); pfAt = 1.0 - std::exp(-1.0 / (fs2 * 40e-6)); tracking = false; goodRun = badRun = 0;
        pllCnt = 0; dcInit = false; dc = 0; pw = 0; pilotLock = false; pilotGood = 0; pilotRel = 0;
        d.clear(); dBase = 0; accIdx = 0; acc.assign(kSegSamples, 0.f); lastSearch = 0; lastPeak = -1; peakRun = 0;
        seg = false; tau = 0; rate = 2.0; rr = 0; segPos = 0; segSyms.clear(); lowQ = 0; syncQ = 0;
        raw.clear(); rawBase = 0; symAbs = 0; segAbs = 0; gainCorr = 1.0;
        fieldLock = false; fieldStartSeg = -1; fsMisses = 0; lastFsSeg = -1; fsSeen = false; pendingStart = -1;
        workerReset();
        tsFlow = 0;
        AtscTelemetry keep;
        keep.seq = t.seq;
        t = keep;
        t.valid = true;
    }

    // ---------------------------------------------------------------- stage A + B
    double tPfb = 0, tCd = 0, tDec = 0, tLs = 0, tEq = 0, tVit = 0;
    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void processInput(std::vector<cf32>& in, size_t n_) {
        (void)n_;
        if (!acqDone) {
            acqBuf.insert(acqBuf.end(), in.begin(), in.end());
            if (acqBuf.size() < acqNeed()) return;
            acquirePilot();
            in.swap(acqBuf);
            std::vector<cf32>().swap(acqBuf);
        }
        if (fieldLock || pilotLock) noLockSamples = 0;
        else if ((noLockSamples += in.size()) > (uint64_t)(0.5 * fin)) { noLockSamples = 0; acqDone = false; }   // look again on the next block
        if (shiftHz != 0) mixIn(in);
        const double tA = now();
        for (const cf32& v : in) { inRe.push_back(v.real()); inIm.push_back(v.imag()); }
        std::vector<float> dchunk;
        const double room = (double)(inBase + inRe.size()) - J - 2 - pos;
        const size_t nOut = room > 0 ? (size_t)std::ceil(room / step) : 0;
        pfRe.resize(nOut);
        pfIm.resize(nOut);
        auto filter = [&](size_t k0, size_t k1) { pfbBlock(taps.data(), J, NPH, inRe.data(), inIm.data(), (int64_t)inBase, pos, step, k0, k1, pfRe.data(), pfIm.data()); };
        if (splitA && nOut >= 4096) {   // split across two threads
            std::thread helper([&] { setThreadPriority(ThreadPriority::Realtime); filter(nOut / 2, nOut); });
            filter(0, nOut / 2);
            helper.join();
        } else filter(0, nOut);
        pos += (double)nOut * step;
        dchunk.resize(nOut);
        // The pilot loop works in blocks of kPllN samples: its bandwidth (a few kHz, up to 30 kHz while acquiring) is far below the
        // block rate (1.3 MHz), so the phase detector runs once per block with its gains scaled by kPllN / 4 (the loop was written for a
        // detector every four samples), and the phases of the samples in a block and the pilot filter over them are computed without a chain.
        constexpr int kPllN = 16;
        constexpr float kPllScale = kPllN / 4.f;
        float pfw[kPllN];   // weights of the block's samples in the pilot filter after the block: pfa * pfb^(kPllN - 1 - i)
        const float pfa = (float)(tracking ? pfAt : pfA), pfb = 1.f - pfa;
        float pfbN = 1.f;
        for (int i = kPllN - 1; i >= 0; i--) { pfw[i] = pfa * pfbN; pfbN *= pfb; }
        auto pllUpdate = [&]() {
            float e = fastAtan2(pf.imag(), pf.real());
            e = std::max(-1.2f, std::min(1.2f, e));
            static const float kPa = getenv("ATSC_PA") ? (float)atof(getenv("ATSC_PA")) : 0.0035f;
            const float alpha = tracking ? kPa * weakScale : 0.035f, beta = alpha * alpha / 4.0f / (tracking ? 1.0f : 4.0f);
            const float xa = kPllScale * alpha * e;       // phase correction, at most about 0.17 rad: a small-angle rotation is exact enough (renormalised below)
            ph *= cf32(1.f - 0.5f * xa * xa, -xa);
            const float xb = kPllScale * beta * e;
            omega += xb;
            rot *= cf32(1.f - 0.5f * xb * xb, -xb);
            if (++renorm >= 16) {
                renorm = 0; ph /= std::abs(ph);
                const double lim = 2.0 * kPi * 60e3 / fs2;   // no real carrier offset is larger; keeps the loop from wandering off in noise
                omega = std::max(omega0 - lim, std::min(omega0 + lim, omega));
                rot = expj(-omega);
            }
            if (dbg && (++dbgCnt % 125000) == 0) fprintf(stderr, "[atsc] pll e=%.3f cfo=%.0f Hz |pf|=%.3f re=%.3f im=%.3f pw=%.3f dc=%.3f\n", e, (omega - omega0) * fs2 / (2 * kPi), std::abs(pf), pf.real(), pf.imag(), pw, dc);
        };
        // sample by sample until the phase detector is due (blocks are aligned to it), then a block at a time
        auto step1 = [&](size_t k) {
            const float r = pfRe[k], m = pfIm[k], pr = ph.real(), pi = ph.imag();
            const cf32 z(r * pr - m * pi, r * pi + m * pr);
            ph = cf32(pr * rot.real() - pi * rot.imag(), pr * rot.imag() + pi * rot.real());
            pf += pfa * (z - pf);
            dchunk[k] = z.real();
            if (++pllCnt >= kPllN) { pllCnt = 0; pllUpdate(); }
        };
        size_t k = 0;
        for (; k < nOut && pllCnt != 0; k++) step1(k);
        for (; k + kPllN <= nOut; k += kPllN) {
            float Rr[4], Ri[4], Pr[5], Pi[5];   // rot^b and ph * rot^(4a)
            Rr[0] = 1; Ri[0] = 0; Rr[1] = rot.real(); Ri[1] = rot.imag();
            for (int b = 2; b < 4; b++) { Rr[b] = Rr[b - 1] * Rr[1] - Ri[b - 1] * Ri[1]; Ri[b] = Rr[b - 1] * Ri[1] + Ri[b - 1] * Rr[1]; }
            const float r4r = Rr[2] * Rr[2] - Ri[2] * Ri[2], r4i = 2 * Rr[2] * Ri[2];
            Pr[0] = ph.real(); Pi[0] = ph.imag();
            for (int a = 1; a < 5; a++) { Pr[a] = Pr[a - 1] * r4r - Pi[a - 1] * r4i; Pi[a] = Pr[a - 1] * r4i + Pi[a - 1] * r4r; }
            const float* xr = &pfRe[k];
            const float* xi = &pfIm[k];
            float zr[kPllN], zi[kPllN];
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) {
                    const int i = 4 * a + b;
                    const float pr = Pr[a] * Rr[b] - Pi[a] * Ri[b], pi = Pr[a] * Ri[b] + Pi[a] * Rr[b];
                    zr[i] = xr[i] * pr - xi[i] * pi; zi[i] = xr[i] * pi + xi[i] * pr;
                }
            float sr = pfbN * pf.real(), si = pfbN * pf.imag();
            for (int i = 0; i < kPllN; i++) { sr += pfw[i] * zr[i]; si += pfw[i] * zi[i]; dchunk[k + i] = zr[i]; }
            pf = cf32(sr, si);
            ph = cf32(Pr[4], Pi[4]);
            pllUpdate();
        }
        for (; k < nOut; k++) step1(k);
        const int64_t keepFrom = (int64_t)std::floor(pos) - J - 1;
        if (keepFrom > (int64_t)inBase) {
            const size_t drop = std::min((size_t)(keepFrom - (int64_t)inBase), inRe.size());
            inRe.erase(inRe.begin(), inRe.begin() + drop);
            inIm.erase(inIm.begin(), inIm.begin() + drop);
            inBase += drop;
        }
        tPfb += now() - tA;
        if (dchunk.empty()) return;
        // DC (pilot) and gain
        if (!dcInit) {
            double m = 0, p = 0;
            for (float v : dchunk) m += v;
            m /= dchunk.size();
            for (float v : dchunk) p += (v - m) * (v - m);
            p /= dchunk.size();
            dc = m; pw = p; dcInit = true;
        }
        const double a = 4e-7;
        // dc and power move by a few parts in 10^5 over 64 samples: they are updated once per block of 64 (the gain already was)
        for (size_t i0 = 0; i0 < dchunk.size(); i0 += 64) {
            const size_t n = std::min<size_t>(64, dchunk.size() - i0);
            const float dcf = (float)dc, gf = (float)(4.58 / std::sqrt(std::max(pw, 1e-12)));
            float* x = &dchunk[i0];
            float sum = 0, sumsq = 0;
            for (size_t i = 0; i < n; i++) { const float c = x[i] - dcf; sum += x[i]; sumsq += c * c; x[i] = c * gf; }
            dc += a * (sum - (double)n * dc);
            pw += a * (sumsq - (double)n * pw);
        }
        // pilot lock: the filtered carrier sits on the real axis and is strong enough; once it has for a while the loop is narrowed
        {
            const double lvl = std::sqrt(std::max(pw, 1e-12));
            pilotRel = (float)(pf.real() / lvl);
            const bool good = std::fabs(pf.imag()) < (tracking ? 0.35 : 0.5) * std::fabs(pf.real()) && pf.real() > 0.06 * std::sqrt(weakScale) * lvl;
            const int nUpd = (int)(dchunk.size() / 4);
            if (good) { goodRun += nUpd; badRun = 0; } else { badRun += nUpd; goodRun = 0; }
            if (!tracking && goodRun > 60000) { tracking = true; }          // ~11 ms of a steady pilot
            if (tracking && badRun > 120000 && !acqPilot) { tracking = false; }   // a weak pilot found by the search never locks the wide loop
            pilotLock = tracking && goodRun > 20000;
        }
        d.insert(d.end(), dchunk.begin(), dchunk.end());
        const double tB = now();
        stageC();
        tCd += now() - tB;
    }
    int renorm = 0;
    int64_t fastSegs = getenv("ATSC_FAST") ? atoi(getenv("ATSC_FAST")) : 300;
    double kpT = getenv("ATSC_KP") ? atof(getenv("ATSC_KP")) : 0.003, kiT = getenv("ATSC_KI") ? atof(getenv("ATSC_KI")) : 1e-5;
    bool dbg = getenv("ATSC_DBG") != nullptr;
    long dbgCnt = 0, stageCnt = 0;

    // ---------------------------------------------------------------- stage C: symbol clock
    float interpAt(double t) const {
        const int64_t i0 = (int64_t)std::floor(t);
        const int ph = std::min(kInterpPh - 1, (int)((t - (double)i0) * kInterpPh));
        const float* tp = &interp[(size_t)ph * 2 * kInterpJ];
        const float* base = &d[(size_t)(i0 - (int64_t)dBase - kInterpJ + 1)];
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
        static_assert(2 * kInterpJ == 16, "four vectors of four taps");
        float32x4_t p = vmulq_f32(vld1q_f32(base), vld1q_f32(tp)), q = vmulq_f32(vld1q_f32(base + 4), vld1q_f32(tp + 4));
        p = vfmaq_f32(p, vld1q_f32(base + 8), vld1q_f32(tp + 8));
        q = vfmaq_f32(q, vld1q_f32(base + 12), vld1q_f32(tp + 12));
        return vaddvq_f32(vaddq_f32(p, q));
#else
        float s[4] = {0, 0, 0, 0};   // four independent partial sums: no chain of sixteen dependent additions
        for (int j = 0; j < 2 * kInterpJ; j += 4) for (int l = 0; l < 4; l++) s[l] += base[j + l] * tp[j + l];
        return (s[0] + s[2]) + (s[1] + s[3]);
#endif
    }
    uint64_t dEnd() const { return dBase + d.size(); }

    void stageC() {
        if (dbg && (++stageCnt % 40) == 0) fprintf(stderr, "[atsc] stage C: seg %d tau %.0f dBase %llu dEnd %llu rate %.5f segAbs %llu inBase %llu pos %.0f\n", (int)seg, tau, (unsigned long long)dBase, (unsigned long long)dEnd(), rate, (unsigned long long)segAbs, (unsigned long long)inBase, pos);
        if (!seg) {
            // correlate the sync pattern at every sample position, averaged over segments
            while (accIdx + 8 < dEnd()) {
                const size_t k = (size_t)(accIdx - dBase);
                const float c = d[k] - d[k + 2] - d[k + 4] + d[k + 6];
                float& A = acc[accIdx % kSegSamples];
                A += (1.f / 12.f) * (c - A);
                accIdx++;
                if (accIdx - lastSearch >= (uint64_t)kSegSamples) {
                    lastSearch = accIdx;
                    int m = 0;
                    for (int i = 1; i < kSegSamples; i++) if (acc[i] > acc[m]) m = i;
                    // noise level of the correlation: the median magnitude away from the peak (a second peak caused by an echo must not count)
                    std::vector<float> mags;
                    mags.reserve(kSegSamples);
                    for (int i = 0; i < kSegSamples; i++) {
                        int dd = std::abs(i - m); dd = std::min(dd, kSegSamples - dd);
                        if (dd > 8) mags.push_back(std::fabs(acc[i]));
                    }
                    std::nth_element(mags.begin(), mags.begin() + mags.size() / 2, mags.end());
                    const double rms = 1.4826 * mags[mags.size() / 2];
                    const bool strong = acc[m] > 8.0 && acc[m] > 5.0 * rms;
                    if (strong && lastPeak >= 0 && std::min(std::abs(m - lastPeak), kSegSamples - std::abs(m - lastPeak)) <= 1) peakRun++;
                    else peakRun = strong ? 1 : 0;
                    lastPeak = strong ? m : -1;
                    if (peakRun >= 6) {
                        const float a0 = acc[(m + kSegSamples - 1) % kSegSamples], a1 = acc[m], a2 = acc[(m + 1) % kSegSamples];
                        const double den = a0 - 2 * a1 + a2;
                        const double delta = den != 0 ? 0.5 * (a0 - a2) / den : 0.0;
                        // most recent occurrence of that position, safely inside the buffered samples
                        uint64_t idx = accIdx - 40;
                        idx -= (idx % kSegSamples + kSegSamples - m) % kSegSamples;
                        tau = (double)idx + delta;
                        rate = 2.0; rr = 0; segPos = 0; segSyms.clear(); lowQ = 0;
                        if (dbg) fprintf(stderr, "[atsc] segment sync acquired (peak %.1f, bin %d)\n", acc[m], m);
                        seg = true; segAbs = 0; symAbs = 0; raw.clear(); rawBase = 0;
                        fieldLock = false; fieldStartSeg = -1; fsMisses = 0; lastFsSeg = -1; pendingStart = -1;
                        workerReset();
                        break;
                    }
                }
            }
            trimD(accIdx > 12000 ? accIdx - 12000 : 0);
        }
        if (seg) {
            while (tau + kInterpJ + 2 < (double)dEnd() && tau - kInterpJ - 1 >= (double)dBase) {
                if (segPos < 4) tauSync[segPos] = tau;
                segSyms.push_back(interpAt(tau));
                segPos++;
                symAbs++;
                tau += rate;
                if (segPos == kSegSyms) { onSegment(); segPos = 0; segSyms.clear(); if (!seg) return; }
            }
            trimD((uint64_t)std::max(0.0, std::floor(tau) - 1900));   // keep the previous segment's sync symbols for the timing measurement
            if (tau - kInterpJ - 1 < (double)dBase) { if (dbg) fprintf(stderr, "[atsc] symbol clock fell behind the buffer\n"); seg = false; accIdx = dEnd(); }   // fell behind the buffer: reacquire
        }
    }

    void trimD(uint64_t keepFrom) {
        if (keepFrom > dBase + 8192) {
            const size_t drop = (size_t)(keepFrom - dBase);
            d.erase(d.begin(), d.begin() + std::min(drop, d.size()));
            dBase += drop;
        }
    }

    // ---------------------------------------------------------------- segment / field level
    void onSegment() {
        // timing error from the four sync symbols: early-late of the correlation with +,-,-,+
        static const float pat[4] = {1, -1, -1, 1};
        double e = 0, q = 0;
        for (int j = 0; j < 4; j++) {
            if (tauSync[j] - 0.5 - kInterpJ - 1 < (double)dBase) continue;   // not in the buffer any more
            e += pat[j] * (interpAt(tauSync[j] + 0.5) - interpAt(tauSync[j] - 0.5));
            q += pat[j] * segSyms[j];
        }
        q /= 20.0;
        syncQ += 0.1 * (q - syncQ);
        const double eps = e / -16.8 * -1.0;   // estimated lateness of the sampling instant, in samples (late = positive)
        // late sampling gives a negative e: move the next instants earlier
        // wide loop while the clock offset is being found, then a narrow one: the data symbols next to the sync pattern make each
        // single measurement noisy and a wide loop would turn that into timing jitter
        const bool fastLoop = (int64_t)segAbs < fastSegs;
        const double kp = fastLoop ? 0.12 : kpT, ki = fastLoop ? 0.01 : kiT;
        tau += kp * (e / 16.8);
        rr += ki * (e / 16.8) / kSegSyms;
        rr = std::max(-0.01, std::min(0.01, rr));
        rate = 2.0 + rr;
        (void)eps;
        if (q < 0.3) { if (++lowQ > 12) { if (dbg) fprintf(stderr, "[atsc] segment sync lost at segment %llu\n", (unsigned long long)segAbs); seg = false; accIdx = dEnd(); lastSearch = accIdx; std::fill(acc.begin(), acc.end(), 0.f); lastPeak = -1; peakRun = 0; fieldLock = false; workerReset(); t.lockLosses++; return; } }
        else lowQ = 0;
        raw.insert(raw.end(), segSyms.begin(), segSyms.end());
        segAbs++;
        checkFieldSync();
        publish(false);
    }

    // field sync match on raw symbols of the segment that just completed
    bool fieldSyncAt(const float* s, int* par) {
        double m = 0;
        for (int i = 0; i < kSegSyms; i++) m += s[i];
        m /= kSegSyms;
        int err = 0;
        for (int j = 0; j < 511 && err < 175; j++) err += ((s[4 + j] - m) >= 0) != (kPn511[j] != 0);
        if (err >= 175) return false;   // random data would give about 255 +- 11 errors, so this cannot be mistaken for a field sync even through an echo
        int e2 = 0;
        for (int j = 0; j < 63; j++) e2 += ((s[4 + 511 + 63 + j] - m) >= 0) != (kPn63[j] != 0);
        *par = e2 < 32 ? 1 : 2;
        return true;
    }

    void checkFieldSync() {
        const size_t n = raw.size();
        const float* last = &raw[n - kSegSyms];
        const int64_t thisSeg = (int64_t)segAbs - 1;   // the segment that just completed
        if (!fieldLock) {
            int par = 0;
            if (fieldSyncAt(last, &par)) {
                if (lastFsSeg >= 0 && thisSeg - lastFsSeg == kFieldSegs) {
                    // two field syncs one field apart: the field that began at lastFsSeg is complete
                    fieldLock = true; fsMisses = 0;
                    pendingStart = lastFsSeg; pendingParity = lastFsParity;
                    nextFs = thisSeg; nextParity = par; parity = par;
                    finishFieldAt(thisSeg);
                } else { lastFsSeg = thisSeg; lastFsParity = par; }
            } else if (lastFsSeg >= 0 && thisSeg - lastFsSeg > 2 * kFieldSegs) lastFsSeg = -1;
        } else if (thisSeg == nextFs) {
            int par = 0;
            if (fieldSyncAt(last, &par)) { fsMisses = 0; nextParity = par; parity = par; }
            else {
                t.fieldSyncMisses++;
                nextParity = 3 - pendingParity;   // the polarity alternates every field
                if (++fsMisses > 3) { if (dbg) fprintf(stderr, "[atsc] field sync lost at segment %lld\n", (long long)thisSeg); fieldLock = false; lastFsSeg = -1; pendingStart = -1; workerReset(); t.lockLosses++; return; }
            }
            finishFieldAt(thisSeg);
        }
        trimRaw();
    }
    int64_t pendingStart = -1, nextFs = -1, lastFsParity = 0;
    int pendingParity = 1, nextParity = 1;

    void finishFieldAt(int64_t thisSeg) {
        decodeField(pendingStart, pendingParity);
        pendingStart = thisSeg;
        pendingParity = nextParity;
        nextFs = thisSeg + kFieldSegs;
    }

    void trimRaw() {
        // keep the previous field's tail for the equaliser history plus everything not yet decoded
        const int64_t keepSeg = (pendingStart >= 0 ? pendingStart : (int64_t)segAbs) - 2;
        const int64_t dropSegs = keepSeg - (int64_t)(rawBase / kSegSyms);
        if (dropSegs > 40) {
            raw.erase(raw.begin(), raw.begin() + (size_t)dropSegs * kSegSyms);
            rawBase += (uint64_t)dropSegs * kSegSyms;
        }
    }

    float rawAt(int64_t symIndex) const {   // absolute symbol index since lock; zero outside the buffer
        const int64_t k = symIndex - (int64_t)rawBase;
        if (k < 0 || k >= (int64_t)raw.size()) return 0.f;
        return raw[(size_t)k];
    }

    // ---------------------------------------------------------------- field worker
    // The per-field work (equaliser fit, filtering, trellis decoding, Reed-Solomon) runs on its own thread so that the sample thread only
    // does the resampler, the pilot loop and the symbol clock. Fields are processed strictly in order (the de-interleaver needs that).
    struct Job { int64_t f0 = 0; int parity = 1; int64_t base = 0; std::vector<float> s; };
    struct DecOut {
        uint64_t fields = 0, segments = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0;
        double snr = 0, dataSnr = 0, segErr = 0;
        bool eqTrained = false;
        std::vector<float> taps, levels;
        uint64_t seq = 0;
    };
    std::thread worker;
    std::mutex jm;
    std::condition_variable jcv;
    std::deque<Job> jobs;
    bool stopW = false, decResetReq = true, working = false;
    std::atomic<bool> blocking{false};   // wait for the worker instead of dropping fields (files, tests)
    std::condition_variable jspace;
    DecOut decOut;
    std::mutex dmu;
    ~Core() { stopWorker(); }
    void startWorker() { if (worker.joinable()) return; stopW = false; worker = std::thread([this] { workerLoop(); }); }
    void stopWorker() {
        { std::lock_guard<std::mutex> lk(jm); stopW = true; }
        jcv.notify_all();
        jspace.notify_all();
        if (worker.joinable()) worker.join();
    }
    void flush() {
        for (int i = 0; i < 4000; i++) {
            { std::lock_guard<std::mutex> lk(jm); if (jobs.empty() && !working) return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void workerReset() { std::lock_guard<std::mutex> lk(jm); jobs.clear(); decResetReq = true; }
    void workerLoop() {
        for (;;) {
            Job j;
            bool doReset = false;
            {
                std::unique_lock<std::mutex> lk(jm);
                jcv.wait(lk, [&] { return stopW || !jobs.empty() || decResetReq; });
                if (stopW) return;
                if (decResetReq) { decResetReq = false; doReset = true; }
                if (!jobs.empty()) { j = std::move(jobs.front()); jobs.pop_front(); working = true; }
            }
            jspace.notify_all();
            if (doReset) {
                dec.reset();
                eqLast = Taps(); snrAvg = 0; dataSnrAvg = 0; segErr = 0; cleanFields = 0; havePrev = false;
                tsFlow = 0;
            }
            if (!j.s.empty()) runJob(j);
            { std::lock_guard<std::mutex> lk(jm); working = false; }
        }
    }
    void decodeField(int64_t startSeg, int fieldParity) {
        if (dbg) fprintf(stderr, "[atsc] queue field at segment %lld (segAbs %llu)\n", (long long)startSeg, (unsigned long long)segAbs);
        Job j;
        j.f0 = startSeg * kSegSyms;
        j.parity = fieldParity;
        j.base = j.f0 - kNP - 4;
        const int64_t end = j.f0 + kFieldSyms + kNF + 4;
        j.s.resize((size_t)(end - j.base));
        for (size_t k = 0; k < j.s.size(); k++) j.s[k] = rawAt(j.base + (int64_t)k);
        {
            std::unique_lock<std::mutex> lk(jm);
            if (blocking) jspace.wait(lk, [&] { return jobs.size() < 2 || stopW; });
            if (jobs.size() >= 3) { jobs.clear(); decResetReq = true; t.fieldSyncMisses++; }   // the worker fell behind: start over cleanly
            jobs.push_back(std::move(j));
        }
        jcv.notify_one();
    }

    double snrAvg = 0, dataSnrAvg = 0, segErr = 0;   // worker-owned running averages

    // ---- equaliser (worker-owned state)
    // y[t] = sum_k ff[k] r[t + NF - k] + sum_j fb[j] a[t - 1 - j] + bias, with a[] the decided symbols. With B = 0 this is the plain
    // feed-forward equaliser (long, 128 taps, trained on the field sync and the segment syncs: robust for mild channels). With
    // B > 0 it is a decision feedback equaliser (shorter feed-forward part plus B feedback taps) that copes with strong echoes; it is
    // trained on decoded symbols: those of the previous field once decoding has been clean for two fields, or the field's own
    // decisions when a field came out badly.
    static constexpr int kNFf = 24;   // feed-forward taps after the cursor
    struct Taps {
        int NP = 103, B = 0;
        std::vector<float> ff, fb;
        float bias = 0;
        bool ok = false;
        int L() const { return kNFf + NP + 1; }
    };
    Taps eqLast;               // the taps used for the telemetry
    int cleanFields = 0;
    std::vector<float> prevS, prevLv, prevY;
    int64_t prevBase = 0, prevF0 = 0;
    bool havePrev = false;
    double eqSnrNow = 0;

    static float dotf(const float* a, const float* b, int n) {
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
        float32x4_t acc = vdupq_n_f32(0);
        int i = 0;
        for (; i + 4 <= n; i += 4) acc = vfmaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
        float r = vaddvq_f32(acc);
        for (; i < n; i++) r += a[i] * b[i];
        return r;
#else
        float r = 0;
        for (int i = 0; i < n; i++) r += a[i] * b[i];
        return r;
#endif
    }

    // least squares for the rows in X (N x n, row-major) against tgt; ridge relative to the mean diagonal
    static bool solveLs(const std::vector<float>& X, const std::vector<float>& tgt, int n, double ridge, std::vector<double>& sol) {
        const int N = (int)tgt.size();
        if (N < n) return false;
        std::vector<double> A((size_t)n * n, 0.0);
        sol.assign(n, 0.0);
        {
            std::vector<float> Af((size_t)n * n, 0.f), bf(n, 0.f);
            syrkLowerT(X.data(), N, n, Af.data());
            gemvT(X.data(), N, n, tgt.data(), bf.data());
            for (size_t i = 0; i < Af.size(); i++) A[i] = Af[i];
            for (int i = 0; i < n; i++) sol[i] = bf[i];
        }
        double tr0 = 0;
        for (int i = 0; i < n; i++) tr0 += A[(size_t)i * n + i];
        for (int i = 0; i < n; i++) A[(size_t)i * n + i] += ridge * tr0 / n;
        return solveSpd(A, sol, n);
    }

    static void unpackTaps(const std::vector<double>& sol, Taps& tp) {
        const int L = tp.L();
        tp.ff.assign(L, 0.f); tp.fb.assign(tp.B, 0.f);
        for (int k = 0; k < L; k++) tp.ff[k] = (float)sol[k];
        for (int j = 0; j < tp.B; j++) tp.fb[j] = (float)sol[L + j];
        tp.bias = (float)sol[L + tp.B];
        tp.ok = true;
    }

    // One field's raw samples and the symbols known or decided for it
    struct View {
        const std::vector<float>* s; int64_t base, f0;
        float at(int64_t idx) const { const int64_t k = idx - base; return (k < 0 || k >= (int64_t)s->size()) ? 0.f : (*s)[(size_t)k]; }
    };

    // Plain feed-forward taps from the field sync and the segment syncs
    bool trainFF(const View& v, const float* fsLv, Taps& tp) {
        tp.NP = 103; tp.B = 0;
        const int n = tp.L() + 1;
        std::vector<float> X, tgt;
        X.reserve((size_t)2000 * n);
        auto row = [&](int64_t t, float target) {
            const size_t o = X.size(); X.resize(o + n);
            float* x = &X[o];
            for (int k = 0; k < tp.L(); k++) x[k] = v.at(t + kNFf - k);
            x[n - 1] = 1.f;
            tgt.push_back(target);
        };
        for (int i = 0; i < 728; i++) row(v.f0 + i, fsLv[i]);
        static const float sy[4] = {5, -5, -5, 5};
        for (int sg = 1; sg < kFieldSegs; sg++) for (int j = 0; j < 4; j++) row(v.f0 + (int64_t)sg * kSegSyms + j, sy[j]);
        std::vector<double> sol;
        if (!solveLs(X, tgt, n, 2e-5, sol)) return false;
        unpackTaps(sol, tp);
        return true;
    }

    // Decision feedback taps: feed-forward 65 + feedback 128. `dataRows` lists positions p (inside the field, >= 832 + B) used with the
    // symbol levels lvl[] (full field) and the soft values y[] to skip rows where decision and reception disagree.
    bool trainDFE(const View& v, const View& dv, const float* fsLv, const std::vector<float>& lvl, const std::vector<float>& y, double ridge, Taps& tp) {
        tp.NP = 40; tp.B = 128;
        const int L = tp.L(), B = tp.B, n = L + B + 1;
        std::vector<float> X, tgt;
        X.reserve((size_t)6000 * n);
        for (int i = B; i < 728; i++) {   // field sync: the past symbols are known too
            const size_t o = X.size(); X.resize(o + n);
            float* x = &X[o];
            for (int k = 0; k < L; k++) x[k] = v.at(v.f0 + i + kNFf - k);
            for (int j = 0; j < B; j++) x[L + j] = fsLv[i - 1 - j];
            x[n - 1] = 1.f;
            tgt.push_back(fsLv[i]);
        }
        const int first = kSegSyms + B, last = kFieldSyms - 1;
        const int step = std::max(1, (last - first) / 5000);
        for (int p = first; p <= last && !lvl.empty(); p += step) {
            if (std::fabs(y[(size_t)p] - lvl[(size_t)p]) > 1.0f) continue;
            const size_t o = X.size(); X.resize(o + n);
            float* x = &X[o];
            for (int k = 0; k < L; k++) x[k] = dv.at(dv.f0 + p + kNFf - k);   // the field the decisions belong to
            for (int j = 0; j < B; j++) x[L + j] = lvl[(size_t)(p - 1 - j)];
            x[n - 1] = 1.f;
            tgt.push_back(lvl[(size_t)p]);
        }
        std::vector<double> sol;
        if (!solveLs(X, tgt, n, ridge, sol)) return false;
        unpackTaps(sol, tp);
        return true;
    }

    // Equalises a whole field. `hist` (optional, B levels, oldest first) is the end of the previous field. Fills lv (soft values) and
    // hv (decisions, with B entries of history in front). Returns the SNR measured on the known field sync symbols.
    double equalize(const View& v, const float* fsLv, const Taps& tp, const float* hist, std::vector<float>& lv, std::vector<float>& hv) {
        const int L = tp.L(), B = tp.B;
        static const float syncLv[4] = {5, -5, -5, 5};
        lv.assign((size_t)kFieldSyms, 0.f);
        hv.assign((size_t)kFieldSyms + B, 0.f);
        if (hist && B) for (int j = 0; j < B; j++) hv[(size_t)j] = hist[j];
        std::vector<float> xr((size_t)kFieldSyms + L - 1), wr(L), ffo((size_t)kFieldSyms);
        for (int k = 0; k < L; k++) wr[k] = tp.ff[L - 1 - k];
        const int64_t first = v.f0 + kNFf - (L - 1);
        for (size_t k = 0; k < xr.size(); k++) xr[k] = v.at(first + (int64_t)k);
        convCorr(xr.data(), wr.data(), ffo.data(), 1, kFieldSyms, L);
        // The feedback part in transposed form: every decision is added, weighted by the feedback taps, to the accumulators of the next B
        // outputs (independent vector updates), so that the chain from one decision to the next is short.
        std::vector<float> fbAcc;
        if (B) {
            fbAcc.assign((size_t)kFieldSyms + B, 0.f);
            if (hist) for (int j = 0; j < B; j++)   // decisions before the field start: decision j (oldest first) reaches output i = j + B - 1 - m ... via tap fb[m]
                for (int m = 0; m < B; m++) { const int i = j + 1 + m - B; if (i >= 0 && i < B) fbAcc[(size_t)i] += tp.fb[m] * hist[j]; }
        }
        const float* fbt = tp.fb.data();
        double mse = 0; int nm = 0;
        for (int i = 0; i < kFieldSyms; i++) {
            float y = ffo[(size_t)i] + tp.bias;
            if (B) y += fbAcc[(size_t)i];
            float dec;
            if (i < 728) dec = fsLv[i];
            else if (i % kSegSyms < 4) dec = syncLv[i % kSegSyms];
            else dec = std::max(-7.f, std::min(7.f, 2.f * std::floor(y * 0.5f) + 1.f));
            if (i >= std::max(B, 8) && i < 728) { const double e = y - fsLv[i]; mse += e * e; nm++; }
            lv[(size_t)i] = y;
            hv[(size_t)B + i] = dec;
            if (B && dec != 0.f) { float* a = &fbAcc[(size_t)i + 1]; for (int m = 0; m < B; m++) a[m] += dec * fbt[m]; }
        }
        mse /= std::max(1, nm);
        return 10.0 * std::log10(25.0 / std::max(mse, 1e-6));
    }

    // the symbol levels of a decoded field: known field sync, known segment syncs, trellis path elsewhere
    void levelsFromPath(const float* fsLv, const std::vector<uint8_t>& psym, const std::vector<float>& hvDec, int B, std::vector<float>& out) {
        static const float syncLv[4] = {5, -5, -5, 5};
        out.assign((size_t)kFieldSyms, 0.f);
        for (int i = 0; i < kSegSyms; i++) out[(size_t)i] = i < 728 ? fsLv[i] : hvDec[(size_t)B + i];
        for (int i = kSegSyms; i < kFieldSyms; i++) out[(size_t)i] = (i % kSegSyms < 4) ? syncLv[i % kSegSyms] : levelOf(psym[(size_t)i]);
    }

    void runJob(const Job& job) {
        const double tD0 = now();
        struct Timer { double& acc; double t0; ~Timer() { acc += now() - t0; } } timer{tDec, tD0};
        const int64_t f0 = job.f0;
        const View view{&job.s, job.base, f0};
        uint8_t prev12[12] = {};
        uint8_t fsSym[kSegSyms];
        fieldSyncSymbols(job.parity == 2, prev12, fsSym);
        float fsLv[kSegSyms];
        for (int i = 0; i < kSegSyms; i++) fsLv[i] = levelOf(fsSym[i]);
        const double tM0 = now();
        static const bool noDfe = getenv("ATSC_NODFE") != nullptr;   // test: the plain equaliser only
        const bool usePrev = havePrev && cleanFields >= 2 && !noDfe;
        Taps tp;
        bool trained;
        std::vector<float> lv, hv;
        double snr = 0;
        if (usePrev) {
            const View pview{&prevS, prevBase, prevF0};
            trained = trainDFE(view, pview, fsLv, prevLv, prevY, 1e-5, tp);
            if (!trained) trained = trainFF(view, fsLv, tp);
        } else trained = trainFF(view, fsLv, tp);
        tLs += now() - tM0;
        const double tM1 = now();
        if (trained) snr = equalize(view, fsLv, tp, (usePrev && tp.B) ? &prevLv[(size_t)(kFieldSyms - tp.B)] : nullptr, lv, hv);
        if (!trained || snr < 9.0) {   // the plain equaliser does not fit: try the decision feedback one on the sync symbols alone (strong echoes)
            Taps tf;
            const std::vector<float> none;
            if (trainDFE(view, view, fsLv, none, none, 1e-3, tf)) {
                std::vector<float> lv2, hv2;
                const double s2 = equalize(view, fsLv, tf, nullptr, lv2, hv2);
                if (!trained || s2 > snr) { tp = tf; lv = std::move(lv2); hv = std::move(hv2); snr = s2; trained = true; }
            }
        }
        eqSnrNow = snr;
        snrAvg = snrAvg == 0 ? snr : snrAvg + 0.3 * (snr - snrAvg);
        std::vector<float> levelsOut;
        auto measureData = [&]() {
            levelsOut.clear();
            double dd = 0; int cnt = 0;
            for (int i = kSegSyms; i < kFieldSyms; i++) {
                if (i % kSegSyms < 4) continue;
                const float vv = lv[(size_t)i];
                const float nearest = std::max(-7.f, std::min(7.f, 2.f * std::floor(vv * 0.5f) + 1.f));
                dd += (vv - nearest) * (vv - nearest); cnt++;
                if ((i * 2654435761u >> 7) % 128 == 0) levelsOut.push_back(vv);
            }
            dd /= std::max(1, cnt);
            const double dsnr = 10.0 * std::log10(21.0 / std::max(dd, 1e-6));
            dataSnrAvg = dataSnrAvg == 0 ? dsnr : dataSnrAvg + 0.3 * (dsnr - dataSnrAvg);
        };
        if (trained) measureData();
        auto publishDec = [&](bool ok, const FieldStats* st) {
            std::lock_guard<std::mutex> lk(dmu);
            decOut.fields++;
            if (st) { decOut.rsClean += st->rsClean; decOut.rsCorrected += st->rsCorrected; decOut.rsFailed += st->rsFailed; decOut.segments += st->segments; }
            decOut.snr = snrAvg; decOut.dataSnr = dataSnrAvg; decOut.segErr = segErr;
            decOut.eqTrained = ok;
            decOut.taps = eqLast.ff;
            decOut.levels = levelsOut;
            decOut.seq++;
        };
        if (!trained || snr < 9.0) {   // no usable channel estimate: do not feed the decoder garbage
            if (dbg) fprintf(stderr, "[atsc] field start %lld: equaliser SNR %.1f dB (solved %d), not decoded\n", (long long)(f0 / kSegSyms), snr, (int)trained);
            dec.reset();
            tsFlow = 0;
            cleanFields = 0; havePrev = false;
            publishDec(false, nullptr);
            return;
        }
        eqLast = tp;
        tEq += now() - tM1;
        const double tM2 = now();
        FieldDecoder backup = dec;      // for the second try below
        FieldStats st;
        std::vector<uint8_t> out((size_t)kDataSegs * kTsBytes), psym((size_t)kFieldSyms, 0);
        auto scaled = [&](const std::vector<float>& v) { std::vector<float> r(v); if (gainCorr != 1.0) for (auto& x : r) x = (float)(x * gainCorr); return r; };
        const std::vector<float> lvd = scaled(lv);
        int np = dec.decode(lvd.data(), out.data(), &st, psym.data());
        bool secondTry = false;
        // A badly decoded field gets a second try with the other kind of equaliser: after the decision feedback one (which can run away
        // after a wrong decision) the plain one; after the plain one a decision feedback equaliser trained on this field's own decisions.
        if (st.segments > 0 && st.rsFailed * 5 > st.segments) {
            Taps alt;
            std::vector<float> lv2, hv2;
            bool haveAlt = false;
            if (usePrev) haveAlt = trainFF(view, fsLv, alt);
            else {
                std::vector<float> selfLv;
                levelsFromPath(fsLv, psym, hv, tp.B, selfLv);
                haveAlt = trainDFE(view, view, fsLv, selfLv, lv, 1e-4, alt);
            }
            if (haveAlt) {
                const double snr2 = equalize(view, fsLv, alt, nullptr, lv2, hv2);
                if (snr2 >= 9.0) {
                    FieldDecoder keep = dec;
                    dec = backup;
                    FieldStats st2;
                    std::vector<uint8_t> out2((size_t)kDataSegs * kTsBytes), psym2((size_t)kFieldSyms, 0);
                    const std::vector<float> lv2d = scaled(lv2);
                    const int np2 = dec.decode(lv2d.data(), out2.data(), &st2, psym2.data());
                    if (st2.rsFailed < st.rsFailed) {
                        secondTry = true;
                        st = st2; np = np2; out = std::move(out2); psym = std::move(psym2); lv = std::move(lv2); hv = std::move(hv2); tp = alt; eqLast = alt;
                        snrAvg = snr2; measureData();
                    } else dec = keep;
                }
            }
        }
        tVit += now() - tM2;
        if (dbg) fprintf(stderr, "[atsc] field start %lld parity %d: snr %.1f data %.1f  RS clean %d corr %d fail %d np %d%s%s\n", (long long)(f0 / kSegSyms), job.parity, snr, dataSnrAvg, st.rsClean, st.rsCorrected, st.rsFailed, np, usePrev ? "  (DFE, trained with the previous field)" : "", secondTry ? "  (second try with the other equaliser)" : "");
        // The gain of the equalised levels against the decoded symbols (decision directed): the next field is corrected by it.
        if (st.segments > 0 && st.rsFailed * 2 <= st.segments) {
            double sxy = 0, sxx = 0;
            for (int i = kSegSyms; i < kFieldSyms; i++) {
                if (i % kSegSyms < 4) continue;
                const double t = levelOf(psym[(size_t)i]);
                sxy += (double)lv[(size_t)i] * gainCorr * t; sxx += t * t;
            }
            if (sxx > 0 && sxy > 0) {
                const double g = sxy / sxx;   // 1 when the levels sit where the decoder expects them
                gainCorr = std::max(0.95, std::min(1.15, gainCorr / std::pow(g, 0.7)));
            }
        }
        // The noise on the equalised symbols, measured against the symbols of the decoded trellis path: unlike the distance to the nearest of the
        // eight levels (which cannot exceed one level step and so reads too good on a noisy signal) this stays honest down to low SNR.
        double pathMse = 1e9;   // mean square distance of the equalised symbols to the decoded path: the noise power while the path is right
        if (st.segments > 0) {
            double dd = 0; int cnt = 0;
            for (int i = kSegSyms; i < kFieldSyms; i++) {
                if (i % kSegSyms < 4) continue;
                const double e = lv[(size_t)i] * gainCorr - levelOf(psym[(size_t)i]);
                dd += e * e; cnt++;
            }
            if (cnt > 0) {
                pathMse = dd / cnt;
                if (st.rsFailed * 4 <= st.segments) {
                    const double dsnr = 10.0 * std::log10(21.0 / std::max(pathMse, 1e-6));
                    dataSnrAvg = dataSnrAvg == 0 ? dsnr : dataSnrAvg + 0.3 * (dsnr - dataSnrAvg);
                }
            }
        }
        if (levelTap) levelTap(lv);
        // a field whose decisions are good enough (not necessarily perfect: the trellis path is right far more often than Reed-Solomon) trains the next one
        if (st.segments == kDataSegs && pathMse < 1.6) cleanFields++; else cleanFields = 0;
        {   // this field's decisions train the next field
            prevS = job.s; prevBase = job.base; prevF0 = f0; prevY = lv;
            levelsFromPath(fsLv, psym, hv, tp.B, prevLv);
            havePrev = true;
        }
        if (st.segments) { const double r = (double)st.rsFailed / st.segments; segErr = segErr + 0.3 * (r - segErr); }
        if (np > 0 && cb) {
            const double secs = (double)np * 0.0000774;   // one packet per data segment: 832 symbols / 10.76 MHz
            cb(out.data(), (size_t)np, secs);
            tsFlow = 6;
        } else if (tsFlow > 0) tsFlow--;
        publishDec(true, &st);
    }

    void publish(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastPub < std::chrono::milliseconds(60)) return;
        lastPub = now;
        std::lock_guard<std::mutex> lk(mu);
        AtscTelemetry nt = t;
        nt.valid = true;
        nt.pilot = pilotLock;
        nt.segSync = seg;
        nt.fieldSync = fieldLock;
        nt.tsOk = tsFlow > 0;
        nt.fieldParity = parity;
        nt.cfoHz = (omega - omega0) * fs2 / (2.0 * kPi) + shiftHz;
        nt.sroPpm = rr / 2.0 * 1e6;
        nt.pilotDb = pilotRel > 0 ? 20.0 * std::log10(pilotRel) : -99;
        nt.syncQuality = syncQ;
        nt.eqCursor = kNF;   // index of the main tap; lower indices are later samples
        nt.seq = pub.seq + 1;
        pub = std::move(nt);
        t.seq = pub.seq;
    }
    std::mutex mu;
    AtscTelemetry pub;
};

} // namespace atsc

struct AtscReceiver::Impl {
    atsc::Core c;
};

AtscReceiver::AtscReceiver() : p_(new Impl) {}
AtscReceiver::~AtscReceiver() = default;
void AtscReceiver::configure(double inputRateHz) { p_->c.configure(inputRateHz); }
void AtscReceiver::reset() { if (p_->c.ok) { p_->c.resetAcq(); p_->c.reset(); } }
void AtscReceiver::flush() { p_->c.flush(); }
void AtscReceiver::setBlocking(bool b) { p_->c.blocking = b; }
bool AtscReceiver::rateOk() const { return p_->c.ok; }
void AtscReceiver::setLevelTap(std::function<void(const std::vector<float>&)> f) { p_->c.levelTap = std::move(f); }
void AtscReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->c.cb = std::move(cb); }
void AtscReceiver::feed(const cf32* x, size_t n) {
    if (!p_->c.ok || !n) return;
    std::vector<cf32> v(x, x + n);
    p_->c.processInput(v, n);
    static long cnt = 0;
    if (getenv("ATSC_PROF") && ++cnt % 195 == 0) fprintf(stderr, "[atsc] time: pfb+pll %.2f s, symbol clock+fields %.2f s, field decode %.2f s (LS fit %.2f, equalise %.2f, trellis+RS %.2f)\n", p_->c.tPfb, p_->c.tCd, p_->c.tDec, p_->c.tLs, p_->c.tEq, p_->c.tVit);
}
bool AtscReceiver::telemetry(AtscTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->c.mu);
    std::lock_guard<std::mutex> lk2(p_->c.dmu);
    const uint64_t seq = p_->c.pub.seq + p_->c.decOut.seq;
    if (seq <= lastSeq) return false;
    out = p_->c.pub;
    const auto& d = p_->c.decOut;
    out.fields = d.fields; out.segments = d.segments; out.rsClean = d.rsClean; out.rsCorrected = d.rsCorrected; out.rsFailed = d.rsFailed;
    out.snrDb = d.snr; out.dataSnrDb = d.dataSnr; out.segErrorRate = d.segErr; out.eqTrained = d.eqTrained;
    out.eqTaps = d.taps; out.levels = d.levels;
    out.seq = seq;
    return true;
}
int AtscReceiver::detectLevel() const {
    const auto& c = p_->c;
    if (c.tsFlow > 0) return 4;
    if (c.fieldLock) return 3;
    if (c.seg) return 2;
    if (c.pilotLock) return 1;
    return 0;
}

} // namespace dect2
