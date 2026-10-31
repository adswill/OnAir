#include "dect2/ofdm/csi_erase.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/dvbt/dvbt_fec.h"
#include "dect2/dsp/gpu_viterbi.h"
#include "dect2/common/stage_pipeline.h"
#include "dect2/common/stage_prof.h"
#include "dect2/channels/chan_locate.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/resampler.h"
#include "dect2/t2/t2.h"
#include "dect2/t2/t2ofdm.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace dect2 {

using namespace dvbt;
using cd = std::complex<double>;

struct DvbtReceiver::Impl {
    // ---- input
    double inRate = 0, fn = 0, bwMhz = 8;
    RationalResampler resampler;
    bool decimate = false, rateOk = true;
    std::vector<cf32> dcOut;      // the input with the radio's DC offset taken out
    cf32 dc = cf32(0, 0);         // slow estimate of that offset
    // Sample clock correction. An 8K symbol is 10240 samples: a clock 80 ppm off moves the window by 0.8 samples per symbol, and inside one
    // symbol it spreads carrier k by k * 80e-6 of a carrier (a third of a carrier at the band edges), which the timing loop, moving only the
    // FFT window, cannot undo. Once the loop has learnt the drift it is handed to this resampler, which corrects the samples themselves.
    // Off (no cost) until the clock is measurably off. Local to this receiver; a shared clock_correct.h can replace it.
    // Merged (a radio rate that has to be resampled anyway, 10 Msps for 8 MHz): one ExactResampler takes the radio's samples straight to the native
    // rate and the clock correction only steers its step, instead of the rational resampler followed by a second, near-unity resampler: one
    // filter per sample instead of two (32 taps instead of 32 + 32 once the clock is corrected, which with a real radio is all the time).
    struct ClockCorrector {
        ExactResampler rs;
        bool on = false;
        bool merged = false;
        double nominal = 1.0;   // merged: input samples per output sample at the nominal rates
        std::vector<cf32> out;
        // 14 lobes: 32 taps at 10 -> 9.14 Msps (a whole number of 8-tap blocks; worst in-band error -42 dB, at the band-edge carriers only)
        void setMerged(double inRate, double outRate) { merged = rs.configure(inRate, outRate, 14) && !rs.passthrough(); nominal = rs.step(); }
        double ppm() const { return merged ? (rs.step() / nominal - 1.0) * 1e6 : on ? (rs.step() - 1.0) * 1e6 : 0.0; }
        void reset() { on = false; out.clear(); if (merged) { rs.reset(); rs.setStep(nominal); } }
        // the samples run fast by rel (positive: more samples than nominal): take that much more input per output sample
        void adjust(double rel, double rate) {
            if (merged) { rs.scaleStep(1.0 + rel); return; }
            if (!on) { rs.configure(rate, rate * (1.0 + 1e-9)); rs.scaleStep(1.0 + 1e-9); on = true; }
            rs.scaleStep(1.0 + rel);
        }
        void process(std::vector<cf32>& io) { if (!on || merged) return; out.clear(); rs.process(io.data(), io.size(), out); io.swap(out); }
    } clk;
    std::vector<cf32> stage;      // the input at the native rate, before the clock correction
    std::vector<cf32> buf;
    int64_t base = 0;
    int64_t end() const { return base + (int64_t)buf.size(); }
    // outside the buffered range reads as silence (a misplaced symbol must not crash the receiver)
    const cf32& at(int64_t i) const {
        static const cf32 kZero(0.f, 0.f);
        const int64_t k = i - base;
        return (k >= 0 && k < (int64_t)buf.size()) ? buf[(size_t)k] : kZero;
    }

    // ---- geometry / state
    int state = 0;                // 0 searching, 1 tracking (CP locked, waiting for TPS), 2 TPS locked and decoding
    int mode = -1, gi = -1, N = 0, G = 0, K = 0, kc = 0;
    double symStart = 0;          // absolute index of the CP start of the next symbol (kept integer; steps are rare)
    int tOff = 0;                 // where the cyclic-prefix correlation peak sits against symStart: moved off zero when the impulse response
                                  // shows paths outside the guard interval of the window that the peak gives (strong echoes, a pre-echo)
    double timingAcc = 0;         // fractional timing error accumulated by the loop
    double timingDrift = 0;       // samples per symbol the window drifts (sample clock offset), learnt by the loop
    int64_t prevWin = INT64_MIN;  // start of the previous symbol's FFT window
    int winShift = 0;             // this window's start minus (previous start + one symbol)
    double accPhi = 0, accSlope = 0;   // running common phase and phase ramp relative to the continual-pilot reference
    double epsFrac = 0;           // fractional carrier-frequency offset in subcarrier spacings (tracked)
    double rhoAvg = 0;            // usual strength of the cyclic-prefix correlation (|sum| / energy), from the symbols that were fine
    int edgeRun = 0;              // symbols in a row whose correlation peak sits at the edge of the tracking range
    int lowRun = 0;               // symbols in a row whose correlation has collapsed: the receiver has lost the symbol timing
    int intShift = 0;
    int intRange = 20;            // whole carriers the integer search covers each way: 50 ppm at the top of UHF (43 kHz), with margin
    bool intLocked = false;
    std::vector<double> intScore; // accumulated pilot-coherence metric per candidate shift
    std::vector<double> intScoreM;// the same for a mirrored spectrum (I and Q swapped by the radio or the file format)
    bool mirror = false;          // the input is conjugated on the way in (set when the mirrored pilot pattern wins)
    int intSymbols = 0;
    uint64_t absSym = 0;          // symbols processed since lock
    int back = 0;
    std::unique_ptr<Fft> fft;
    std::vector<cf32> fbuf;
    std::vector<float> fre, fim;   // transform() works on these split arrays
    std::atomic<int> detect{0};
    std::atomic<bool> lpReq{false};
    std::atomic<int> computeMode{2};
    int agreeCount = 0, agreeMode = -1, agreeGi = -1;
    std::vector<double> acqCumR, acqCumI, acqEn, acqRr, acqF, acqMed;   // scratch of acquire()
    uint64_t searchSamples = 0;   // samples the acquisition has gone through without finding a signal (reduced search rate after 2 s)
    int64_t acqLastEnd = 0;

    // ---- TPS / frame sync
    std::vector<int8_t> tpsBitsSeen;  // per processed symbol (index = absSym - tpsBase)
    uint64_t tpsBase = 0;
    std::vector<cf32> prevTps;        // previous symbol's TPS carriers
    std::vector<cf32> prevCp;         // and its continual pilots (for the common phase turn between the two)
    bool prevValid = false;
    bool tpsOk = false;
    Params prm;
    int frameStartMod = 0;            // (absSym - frameStart) mod 68 = symbol index inside the frame
    uint64_t frameStartAbs = 0;
    int frameIdx = 0;
    uint64_t frameRefAbs = 0;         // the start of a frame whose index (in the super-frame of four) is frameRefIdx
    int frameRefIdx = 0;
    // After a resync the scattered pilots tell how many symbols were lost only modulo four. The symbols that follow are held back from the
    // error correction until their TPS bits have said the rest, then go in behind the right number of erased symbols.
    struct HeldSym { std::vector<cf32> eq; std::vector<float> n0; int symIdx; };
    std::vector<HeldSym> held;
    bool holding = false;
    uint64_t holdFrom = 0;
    int tpsFailures = 0;
    double secSinceTps = 0;
    int hypVotes[4] = {0, 0, 0, 0};   // scattered-pilot phase votes
    double hypScore[4] = {0, 0, 0, 0};

    // ---- channel estimation
    std::vector<cf32> grid;           // pilot-grid estimates, spacing 3 carriers
    std::vector<uint8_t> gridAge;
    std::vector<cf32> cpRef;          // continual-pilot reference (channel at the continual pilot carriers)
    bool cpRefValid = false;
    GridInterpolator interp;
    std::vector<cf32> H;
    std::vector<cf32> Y;
    int gridFilled = 0;
    double sigma2 = 0.05;             // noise variance of Y (per complex carrier)
    double snrDb = 0;
    FecDecoder fec;
    uint64_t symbols = 0;
    std::function<void(const uint8_t*, size_t, double)> cb;

    // ---- telemetry
    std::mutex mu;
    RxTelemetry tel;
    uint64_t seq = 0;
    std::vector<cf32> eqShow, rawShow, tpsShow, pilotShow;
    // scratch of estimateAndDecode, kept between symbols (no allocation per symbol)
    std::vector<cf32> scrGrid, scrEq;
    std::vector<float> scrN0;
    ofdm::CarrierInterference carrierErase;   // erases carriers hit by a narrowband interferer (core/include/dect2/ofdm/csi_erase.h)
    const bool flatN0 = ofdm::envOn("DECT2_OFDM_FLAT_N0");   // A/B tests only: no channel-state weighting
    // the plots are read about 30 times a second: they are rebuilt every ~25 ms of signal, not every six symbols, and only copied into the telemetry when new
    uint64_t showSeq = 0, pubShowSeq = ~0ull;
    uint64_t lastShowSym = 0; bool showEver = false;
    std::vector<float> chMag, chPh;
    std::vector<float> irDb;
    int irTauMin = 0;
    double cfoHz = 0;
    int64_t lastPublishSym = -100;

    ChannelLocator locator;       // where the channel sits in the input band (not reset with the acquisition)
    ChannelMixer mixer;
    void restartAt(double hz) {
        const cf32 d = dc;
        reset();
        dc = d;
        mixer.set(hz, inRate);
    }

    void reset() {
        state = 0; mode = gi = -1; N = G = K = 0; symStart = 0; epsFrac = 0; intShift = 0; intLocked = false; rhoAvg = 0; lowRun = 0; streaming = false; phaseCheck = false; postResync = 0;
        intScore.clear(); intSymbols = 0; absSym = 0; fft.reset(); agreeCount = 0; agreeMode = agreeGi = -1; acqLastEnd = 0; searchSamples = 0;
        tpsBitsSeen.clear(); tpsBase = 0; prevTps.clear(); prevCp.clear(); prevValid = false; prevWin = INT64_MIN; tpsOk = false; tpsFailures = 0; secSinceTps = 0;
        for (int i = 0; i < 4; i++) { hypVotes[i] = 0; hypScore[i] = 0; }
        grid.clear(); gridAge.clear(); cpRef.clear(); cpRefValid = false; gridFilled = 0;
        fecReset(); held.clear(); holding = false; symbols = 0; detect = 0; pend.clear();
        buf.clear(); base = 0; resampler.reset(); dc = cf32(0, 0); clk.reset(); mirror = false;
        eqShow.clear(); rawShow.clear(); chMag.clear(); chPh.clear(); irDb.clear();
    }

    // ------------------------------------------------------------------ acquisition: FFT size and guard interval from the cyclic prefix
    bool acquire() {
        DECT2_STAGE("dvbt.acquire");
        struct Cand { double score; int mode, gi; int64_t pos; cd corr; };
        Cand best{0, -1, -1, 0, cd(0, 0)};
        const int64_t b0 = base;
        for (int m = 0; m < 2; m++) {
            const int n = fftN(m);
            // 6 symbols of the longest guard plus the correlation lag
            const int64_t avail = (int64_t)buf.size();
            if (avail < (int64_t)(6.0 * (n + n / 4) + n + n / 4)) continue;
            const int64_t B = std::min<int64_t>(avail, (int64_t)(8 * (n + n / 4) + n));
            const int64_t Dn = B - n;
            // products y[i] conj(y[i+n]) and energies, as cumulative sums (split real / imaginary rows, kept between calls: they were
            // allocated, 1.3 MB for 8K, on every search)
            acqCumR.resize((size_t)Dn + 1); acqCumI.resize((size_t)Dn + 1); acqEn.resize((size_t)Dn + 1);
            double* cumR = acqCumR.data(); double* cumI = acqCumI.data(); double* en = acqEn.data();
            {
                // the product written out: the same multiplications and sums as cd(a) * conj(cd(b)), one per statement (nothing fused), without
                // std::complex's NaN check per product
                double accR = 0, accI = 0, ea = 0;
                cumR[0] = 0; cumI[0] = 0; en[0] = 0;
                const cf32* pa = buf.data() + (buf.size() - B);
                const cf32* pb = pa + n;
                for (int64_t i = 0; i < Dn; i++) {
                    const double ar = pa[i].real(), ai = pa[i].imag(), br = pb[i].real(), bi = -(double)pb[i].imag();
                    const double ac = ar * br, bd = ai * bi, ad = ar * bi, bc = ai * br;
                    const double pr = ac - bd, pi = ad + bc;
                    accR += pr; accI += pi;
                    ea += 0.5 * (std::norm(pa[i]) + std::norm(pb[i]));
                    cumR[i + 1] = accR; cumI[i + 1] = accI; en[i + 1] = ea;
                }
            }
            for (int g = 0; g < 4; g++) {
                const int G_ = dvbt::guardSamples(m, g), P = n + G_;
                const int64_t D = Dn - G_;
                if (D < P * 3) continue;
                acqF.assign((size_t)P, 0.0);
                double* F = acqF.data();
                // runs on every block while searching, so keep it lean: the normalised magnitudes in one plain loop the compiler can
                // vectorise (sqrt of the norm: std::abs goes through the much slower overflow-safe hypot), then the fold by symbol phase
                // one period at a time (each F[ph] gets its terms in the same order as a walk over d, and the loop over ph vectorises)
                acqRr.resize((size_t)D);
                double* rr = acqRr.data();
                for (int64_t d = 0; d < D; d++) {
                    const double cr = cumR[d + G_] - cumR[d], ci = cumI[d + G_] - cumI[d];
                    const double e = en[d + G_] - en[d];
                    rr[d] = e > 1e-12 ? std::sqrt(cr * cr + ci * ci) / e : 0.0;
                }
                for (int64_t b = 0; b < D; b += P) {
                    const int64_t len = std::min<int64_t>(P, D - b);
                    const double* r = rr + b;
                    for (int64_t ph = 0; ph < len; ph++) F[ph] += r[ph];
                }
                const int64_t full = D / P, rem = D % P;   // how many d fall on each phase
                auto cntOf = [&](int i) { return (int)(full + (i < rem ? 1 : 0)); };
                double mx = 0, mean = 0; int arg = 0;
                for (int i = 0; i < P; i++) { const int c = cntOf(i); if (c) F[i] /= c; mean += F[i]; if (F[i] > mx) { mx = F[i]; arg = i; } }
                mean /= P;
                // against the floor (the median), not the mean: under a strong echo the correlation is a plateau as wide as the echo delay
                // (a 0 dB echo at 0.9 of the guard: 0.4 of the symbol at the plateau's level), which pulled the mean up to within 3 times
                // the peak and the signal was never found. Noise alone stays near its median (ratio 1.2 to 1.6).
                acqMed.assign(F, F + P);
                std::nth_element(acqMed.begin(), acqMed.begin() + P / 2, acqMed.end());
                const double floorF = acqMed[(size_t)P / 2];
                const double ratio = floorF > 0 ? mx / floorF : mean > 0 ? mx / mean : 0;
                // a clean cyclic prefix gives a folded peak far above the floor; wrong (N, G) hypotheses stay flat
                const double score = mx * std::min(ratio, 12.0) / 12.0;
                if (mx > 0.12 && ratio > 3.0 && score > best.score) {
                    best.score = score; best.mode = m; best.gi = g;
                    // symbol start in absolute samples: the first occurrence of the peak phase after the oldest used sample
                    const int64_t first = b0 + (int64_t)buf.size() - B;
                    best.pos = first + arg;
                    // the mean correlation at the peak phase, summed in the order of d (only for the phase that is used)
                    cd fc(0, 0);
                    for (int64_t d = arg; d < D; d += P) fc += cd(cumR[d + G_], cumI[d + G_]) - cd(cumR[d], cumI[d]);
                    best.corr = fc / (double)std::max(1, cntOf(arg));
                }
            }
        }
        if (best.mode < 0) { agreeCount = 0; return false; }
        detect = std::max(detect.load(), 1);
        if (best.mode == agreeMode && best.gi == agreeGi) agreeCount++; else { agreeMode = best.mode; agreeGi = best.gi; agreeCount = 1; }
        if (agreeCount < 2) return false;
        mode = best.mode; gi = best.gi; N = fftN(mode); G = dvbt::guardSamples(mode, gi); K = carriersK(mode); kc = (K - 1) / 2;
        const int P = N + G;
        // place the symbol start somewhere ahead of the oldest sample we still hold
        int64_t s = best.pos;
        while (s < base + (int64_t)buf.size() - (int64_t)(6 * P)) s += P;
        while (s - P >= base + 16) s -= P;
        symStart = (double)s;
        timingAcc = 0; timingDrift = 0; accPhi = accSlope = 0; rhoAvg = 0; lowRun = 0; tOff = 0; profN = 0; plateauKnown = false;
        epsFrac = -std::arg(best.corr) / (2 * M_PI);   // c = sum y[n] conj(y[n+N]) has phase -2 pi eps
        back = std::min(G / 4, 24);
        fft = std::make_unique<Fft>(N);
        fbuf.assign(N, cf32(0, 0));
        // a radio 50 ppm off at 860 MHz is 43 kHz away: 38 carriers in 8K at 8 MHz, 62 at 5 MHz. Search +-55 kHz.
        intRange = std::max(20, (int)std::ceil(55e3 / (fn / N)));
        intScore.assign((size_t)(2 * intRange + 1), 0.0); intScoreM.assign(intScore.size(), 0.0); intSymbols = 0; intLocked = false; intShift = 0;
        tpsBitsSeen.clear(); tpsBase = 0; prevValid = false; prevWin = INT64_MIN; tpsOk = false; absSym = 0;
        grid.assign((size_t)(K + 2) / 3 + 1, cf32(0, 0)); gridAge.assign(grid.size(), 255);
        cpRef.assign(continualPilots(mode).size(), cf32(1, 0)); cpRefValid = false; gridFilled = 0; cpHist = 0; ceStats = 0; ceAlpha = 1; ceDelayKnown = false;
        for (int i = 0; i < 4; i++) { hypVotes[i] = 0; hypScore[i] = 0; }
        fecReset(); streaming = false; phaseCheck = false; postResync = 0; pend.clear();
        state = 1;
        return true;
    }

    // ------------------------------------------------------------------ one symbol
    // Transforms the symbol whose CP starts at `s` (integer) into carriers Y[0..K-1]
    void transform(int64_t s) {
        DECT2_STAGE("dvbt.fft");
        const int64_t w = s + G - back;
        const double ph0 = -2.0 * M_PI * epsFrac * (double)(w % 1048576) / N;
        const double dph = -2.0 * M_PI * epsFrac / N;
        // fractional carrier-offset correction: an exact phasor at the start of every block of 64 samples and a table for the
        // offsets inside it, so no phasor recurrence runs through the whole symbol (and the multiplications vectorise)
        constexpr int B = 64;
        cf32 tab[B];
        for (int i = 0; i < B; i++) tab[i] = cf32((float)std::cos(dph * i), (float)std::sin(dph * i));
        const cf32* src = &at(w);
        // straight into the transform's split real / imaginary arrays (the same values Fft::forward made from fbuf, without its two copies)
        fre.resize((size_t)N); fim.resize((size_t)N);
        float* __restrict re = fre.data();
        float* __restrict im = fim.data();
        for (int i0 = 0; i0 < N; i0 += B) {
            const double a = ph0 + dph * i0;
            const cf32 c0((float)std::cos(a), (float)std::sin(a));
            for (int j = 0; j < B; j++) { const cf32 v = src[i0 + j] * (c0 * tab[j]); re[i0 + j] = v.real(); im[i0 + j] = v.imag(); }
        }
        int lg = 0;
        while ((1 << lg) < N) lg++;
        fftSplit(re, im, lg, false);
        if (!intLocked) for (int i = 0; i < N; i++) fbuf[(size_t)i] = cf32(re[i], im[i]);   // the integer search reads the whole unshifted transform
        Y.resize(K);
        // carrier k is bin (k + intShift - kc) mod N: at most two contiguous runs of bins (no two divisions per carrier)
        {
            const int b0 = (((intShift - kc) % N) + N) % N;
            const int run1 = std::min(K, N - b0);
            for (int k = 0; k < run1; k++) Y[(size_t)k] = cf32(re[b0 + k], im[b0 + k]);
            for (int k = run1; k < K; k++) Y[(size_t)k] = cf32(re[k - run1], im[k - run1]);
        }
    }

    // CP correlation around the expected position: timing error and fractional CFO
    // sum over n samples of a[i] * conj(b[i]), in single precision with eight lanes (the compiler vectorises it)
    static cd corrSum(const cf32* a, const cf32* b, int n) {
        float re[8] = {}, im[8] = {};
        const int n8 = n & ~7;
        for (int i = 0; i < n8; i += 8)
            for (int j = 0; j < 8; j++) {
                const float ar = a[i + j].real(), ai = a[i + j].imag(), br = b[i + j].real(), bi = b[i + j].imag();
                re[j] += ar * br + ai * bi; im[j] += ai * br - ar * bi;
            }
        double sr = 0, si = 0;
        for (int j = 0; j < 8; j++) { sr += re[j]; si += im[j]; }
        for (int i = n8; i < n; i++) { sr += (double)a[i].real() * b[i].real() + (double)a[i].imag() * b[i].imag(); si += (double)a[i].imag() * b[i].real() - (double)a[i].real() * b[i].imag(); }
        return cd(sr, si);
    }
    void cpTrack(int64_t s, double& err, double& epsRaw, double& mag, double& rho) {
        DECT2_STAGE("dvbt.sync");
        double bestM = -1; int bestD = 0; cd bestC = 0;
        double ms[13];
        // the 13 windows (offsets -6..6) overlap in all but two samples: one full sum, then each step drops one product and adds one
        const cf32* x = &at(s - 6);
        cd c = corrSum(x, x + N, G);
        for (int d = -6; d <= 6; d++) {
            if (d > -6) {
                const cf32 o = x[d + 5], on = x[d + 5 + N], n1 = x[d + 5 + G], n1n = x[d + 5 + G + N];
                c += cd(n1.real(), n1.imag()) * std::conj(cd(n1n.real(), n1n.imag())) - cd(o.real(), o.imag()) * std::conj(cd(on.real(), on.imag()));
            }
            const double m = std::abs(c);
            ms[d + 6] = m;
            if (m > bestM) { bestM = m; bestD = d; bestC = c; }
        }
        cpEdgeGain = ms[6] > 0 ? bestM / ms[6] : 1.0;
        double frac = 0;
        if (bestD > -6 && bestD < 6) {
            const double a = ms[bestD + 5], b = ms[bestD + 6], c = ms[bestD + 7];
            const double den = a - 2 * b + c;
            if (den < 0) frac = 0.5 * (a - c) / den;
        }
        err = bestD + frac;
        epsRaw = -std::arg(bestC) / (2 * M_PI);
        mag = bestM;
        // correlation coefficient: |sum| against the energy of the two copies; near 1 on a clean signal, near 0 where there is none
        double e = 0;
        {
            const cf32* pa = &at(s + bestD);
            float ea[8] = {};
            for (int i = 0; i < G; i += 8) for (int j = 0; j < 8; j++) ea[j] += std::norm(pa[i + j]) + std::norm(pa[i + j + N]);
            for (int j = 0; j < 8; j++) e += 0.5 * (double)ea[j];
        }
        rho = e > 1e-12 ? bestM / e : 0.0;
    }

    // The cyclic-prefix correlation has collapsed for good: the symbol timing is wrong by more than the tracking loop can pull in (samples were
    // lost, the clock jumped) or the signal is gone. Look for the symbol boundary anywhere in one symbol length, using running sums so that the
    // whole search is about as cheap as one symbol. Found: carry on at the new timing, keeping the frame position and the channel type. Not
    // found: start the acquisition at once instead of waiting for three failed TPS frames (a quarter of a second).
    // Returns false when more samples are needed first.
    bool resync(int64_t s, bool early = false) {
        const int P = N + G;
        const int64_t need = s + 2 * (int64_t)P + N + 64;
        if (need > end()) return false;
        std::vector<cd> cum((size_t)(P + G) + 1);
        std::vector<double> en((size_t)(P + G) + 1);
        cum[0] = 0; en[0] = 0;
        for (int i = 0; i < P + G; i++) {
            const cf32 a = at(s + i), b = at(s + i + N);
            cum[(size_t)i + 1] = cum[(size_t)i] + cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
            en[(size_t)i + 1] = en[(size_t)i] + 0.5 * ((double)std::norm(a) + (double)std::norm(b));
        }
        int bestD = 0; double bestRho = -1;
        for (int d = 0; d < P; d++) {
            const double e = en[(size_t)(d + G)] - en[(size_t)d];
            const double r = e > 1e-12 ? std::abs(cum[(size_t)(d + G)] - cum[(size_t)d]) / e : 0.0;
            if (r > bestRho) { bestRho = r; bestD = d; }
        }
        const bool found = bestRho >= std::max(0.3, 0.5 * rhoAvg);
        if (early) {
            // Two symbols in a row have collapsed while the receiver was locked: most likely the radio dropped samples (a USB stall) and the
            // timing jumped. Act at once when the boundary has clearly moved; otherwise (a fade, a burst of interference) leave everything as
            // it is and let the normal rule (a dozen collapsed symbols) decide.
            int off = bestD - tOff;
            off = ((off % P) + P) % P; if (off >= P / 2) off -= P;
            if (!found || std::abs(off) <= 8) return true;
            // (bounded by the channel's measured delay spread: on a single path a move like this is a loss of whole symbols plus a few samples)
            const int spread = ceDelayKnown ? (int)std::lround((ceDelayHalf - 48) / 0.7) : 0;
            if (std::abs(off) <= std::max(16, std::min(G / 16, spread + 16)) && lowRun == 0) {
                // The peak has walked to the edge of the tracking range, a few dozen samples: under fading multipath the strongest path changes
                // (a TU6 profile spans 46 samples). No samples are lost and no symbol is skipped: move the window (the channel estimate and
                // the TPS differential follow its known phase ramp) and carry on, without the hold and the checks that follow a sample loss.
                // (a gap of whole symbols plus a few samples looks the same: the scattered pilots of the next good symbol tell, and only then
                // does the full recovery run)
                symStart += off; placedShift = off; edgeRun = 0; phaseCheck = true; phaseCheckLight = true;
                if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] timing peak moved by %d samples at sym %llu: window follows\n", off, (unsigned long long)absSym);
                return true;
            }
            // under a strong echo the correlation has a flat top: the point tracked is as good as the best one
            if (tOff != 0 || plateauKnown) {
                const int d0 = ((tOff % P) + P) % P;
                const double e0 = en[(size_t)(d0 + G)] - en[(size_t)d0];
                const double r0 = e0 > 1e-12 ? std::abs(cum[(size_t)(d0 + G)] - cum[(size_t)d0]) / e0 : 0.0;
                if (r0 >= 0.85 * bestRho) return true;
            }
        }
        profN = 0;
        const int run = lowRun;   // a jump caught on the first collapsed symbol: the boundary one step back still straddles the gap
        lowRun = 0; edgeRun = 0;
        if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] resync%s at sym %llu (rx %lld): best offset %d (rho %.2f, usual %.2f)\n", early ? " (early)" : "", (unsigned long long)absSym, (long long)s, bestD >= P / 2 ? bestD - P : bestD, bestRho, rhoAvg);
        if (found) {
            // The boundary repeats every P samples. Take the shift nearest to zero, not the first one ahead: if some samples (less than half a
            // symbol) went missing, the symbol that follows starts a little earlier than expected, and stepping back keeps the receiver's count of
            // symbols (and so the frame position and the transport stream alignment) right. A step ahead by almost a whole symbol would skip one.
            // Soon after the gap, though, the symbol one step back can still straddle it: then go ahead (the scattered-pilot check below puts the
            // frame position right, it counts up to three symbols).
            if (bestD >= P / 2) {
                const int64_t c = s + bestD - P;
                double e = 0;
                const cd cc = corrSum(&at(c), &at(c + N), G);
                for (int i = 0; i < G; i++) e += 0.5 * ((double)std::norm(at(c + i)) + (double)std::norm(at(c + i + N)));
                const double rc = e > 1e-12 ? std::abs(cc) / e : 0.0;
                if (!early || (run != 1 && rc >= 0.7 * bestRho)) bestD -= P;
            }
            // the symbols still waiting for the pilots after them go out now, with what there is: the pilots after a jump are taken at
            // another window and would not fit them
            drainAll();
            symStart = (double)(s + bestD - tOff);
            timingAcc = 0;
            prevValid = false; prevWin = INT64_MIN; winShift = 0;
            if (early) {
                // Caught within two symbols of the jump: the channel and the carrier are what they were, only the window moved. Put it back on
                // the correlation peak to the sample, where the tracking loop held it before, and keep the channel estimate and the pilot phase
                // reference: the symbols after the gap decode at once instead of after four symbols of rebuilding.
                double terr, eraw, mag, rho;
                cpTrack(s + bestD, terr, eraw, mag, rho);
                symStart += std::round(terr);
            } else {
                drainAll();   // the symbols still waiting for their next pilots: with what there is
                accPhi = accSlope = 0;
                cpRefValid = false; std::fill(gridAge.begin(), gridAge.end(), (uint8_t)255); gridFilled = 0; cpHist = 0; ceDelayKnown = false;
            }
            resyncs++;
            phaseCheck = true; postResync = 2;
            return true;
        }
        tpsOk = false; state = 0; detect = 1; agreeCount = 0;
        return true;
    }
    uint64_t resyncs = 0;
    double cpEdgeGain = 1;        // the CP correlation's peak against its centre value in the last cpTrack
    // FFT window placement from the shape of the cyclic-prefix correlation, averaged over symbols. One path: a peak with slopes G wide each
    // side. A strong echo tau samples later: a flat top tau wide, and the peak sits anywhere on it; a window placed there overlaps the next
    // symbol of the first path (a 0 dB echo at 0.9 of the guard never decoded). The earliest path starts where the top begins.
    std::vector<double> prof;
    std::vector<cd> profCum;
    int profN = 0, profSkip = 0, placedShift = 0;
    int64_t profRef = 0; uint64_t profRefSym = 0;
    bool plateauKnown = false;    // the last profile showed a flat top (a strong echo): edge peaks and resync offsets on it mean nothing
    void placeWindow(int64_t s) {
        if (ceNoPlace) return;
        if (profSkip > 0) { profSkip--; return; }   // the previous symbol still sat at the old window
        const int R = 2 * G + G / 8;   // a strong echo up to G late puts the first path's rise up to 2 G before the tracked point
        // The previous symbol's neighbourhood (all of it is in the buffer), on a grid fixed in the input samples from the first symbol of
        // the average: the window may move meanwhile (the timing loop, a small resync) without smearing what is averaged.
        if (profN == 0) { profRef = s - (N + G) + tOff; profRefSym = absSym; }
        const int64_t c = profRef + (int64_t)(absSym - profRefSym) * (N + G);
        if (c - R < base || c + R + G + N + 1 > end()) return;
        profCum.resize((size_t)(2 * R + G) + 1);
        profCum[0] = 0;
        for (int i = 0; i < 2 * R + G; i++) {
            const cf32 a = at(c - R + i), b = at(c - R + i + N);
            profCum[(size_t)i + 1] = profCum[(size_t)i] + cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
        }
        prof.resize((size_t)(2 * R + 1), 0.0);
        if (profN == 0) std::fill(prof.begin(), prof.end(), 0.0);
        for (int d = 0; d <= 2 * R; d++) prof[(size_t)d] += std::abs(profCum[(size_t)(d + G)] - profCum[(size_t)d]);
        if (++profN < (placements == 0 && !plateauKnown ? 12 : 24)) return;   // (the first answer sooner)
        profN = 0;
        int am = 0;
        for (int d = 1; d <= 2 * R; d++) if (prof[(size_t)d] > prof[(size_t)am]) am = d;
        const double m = prof[(size_t)am];
        // The top's level wanders by several per cent over the symbols averaged, so its edge is not found from the top: the earliest path's
        // correlation rises from zero G samples before its symbol starts, steeply and alone. Where the profile (over its floor: the first
        // G/8 samples, outside every path's ramp) first reaches a quarter of the peak, a single path is 0.25 G into that ramp.
        double floorP = 0;
        for (int d = 0; d < G / 8; d++) floorP += prof[(size_t)d];
        floorP /= std::max(1, G / 8);
        if (m < 3 * floorP) return;   // too little signal to tell
        // (the floor is the magnitude of a sum of unrelated products: it adds to the correlation in power, not in magnitude)
        const double lvl = std::sqrt(floorP * floorP + 0.0625 * (m * m - floorP * floorP));
        int d = 0, run = 0;
        for (; d < am; d++) { run = prof[(size_t)d] >= lvl ? run + 1 : 0; if (run == 3) break; }
        if (run < 3) return;
        d -= 2;   // the first of the three
        if (d < 1) return;   // already above at the left edge: the profile does not reach back far enough
        const double a = prof[(size_t)(d - 1)], b = prof[(size_t)d];
        const double dl = d - 1 + (b > a ? (lvl - a) / (b - a) : 1.0);
        const double x = dl + 0.75 * G - R;
        const int xi = (int)std::lround(x);
        const int delta = (int)(c + xi + (N + G) - s);   // the next symbol's start, from the earliest path of the previous one
        // One path: the rise is 0.75 G before the peak, and the window stays where the peak put it (as it always did). Only a top wider
        // than that (a strong echo) or a path before the peak moves it the first time.
        // Once moved, the point tracked wanders freely on the flat top: from then on the window follows the rise alone.
        plateauKnown = (double)am - dl >= 0.75 * G + std::max(8, G / 16);   // a top wider than one path's: a strong echo
        if (tOff == 0 && !plateauKnown) return;
        if (std::abs(delta) < std::max(8, G / 32)) return;
        if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] earliest path's symbol starts %d samples from the tracked point (peak %d, rise %.0f, sym %llu): FFT window moved by %d\n", xi, am - R, dl - R, (unsigned long long)absSym, delta);
        // the move is known, so the channel estimate and the TPS differential carry over (they turn by its phase ramp, as for a step of the
        // timing loop)
        symStart += delta; tOff -= delta; placements++; profSkip = 1; placedShift = delta;
        if (std::abs(delta) > G) {   // (cannot happen with a top at most G wide; kept as a guard) rebuild the channel estimate
            drainAll();
            accPhi = accSlope = 0;
            cpRefValid = false; std::fill(gridAge.begin(), gridAge.end(), (uint8_t)255); gridFilled = 0; cpHist = 0; ceDelayKnown = false;
        }
    }
    bool streaming = false;       // the error correction has been fed symbols without a break since the last acquisition
    bool phaseCheckLight = false; // the check after a small move of the window (only a loss it finds starts the hold)
    bool phaseCheck = false;      // after a resync: check once which of the four scattered-pilot positions the next good symbol has
    int postResync = 0;           // frame checks still to come after a resync: the first one is expected to fail (that frame holds the gap)

    static double wrap(double v) { while (v > 0.5) v -= 1; while (v < -0.5) v += 1; return v; }

    // integer CFO: continual pilots keep their value in every symbol, so Y_l[k+s] conj(Y_{l-1}[k+s]) has one common phase when s is right
    std::vector<cf32> prevRawFull;     // previous symbol's full FFT (for the integer search over a range of shifts)
    void integerSearch(int64_t s) {
        DECT2_STAGE("dvbt.sync");
        // transform without carrier shift into fbuf for the next symbol, keep the previous one
        if (prevRawFull.size() != (size_t)N) prevRawFull.assign(N, cf32(0, 0));
        std::vector<cf32> cur(fbuf.begin(), fbuf.end());
        (void)s;
        if (intSymbols > 0) {
            const auto& cp = continualPilots(mode);
            for (int sh = -intRange; sh <= intRange; sh++) {
                cd acc = 0; double mag = 0;
                for (int k : cp) {
                    const int idx = (((k + sh - kc) % N) + N) % N;
                    const cd z = cd(cur[idx].real(), cur[idx].imag()) * std::conj(cd(prevRawFull[idx].real(), prevRawFull[idx].imag()));
                    acc += z; mag += std::abs(z);
                }
                intScore[sh + intRange] += mag > 0 ? std::abs(acc) / mag : 0;
                // mirrored: carrier k sits at bin -(k - kc) + sh
                acc = 0; mag = 0;
                for (int k : cp) {
                    const int idx = (((sh - (k - kc)) % N) + N) % N;
                    const cd z = cd(cur[idx].real(), cur[idx].imag()) * std::conj(cd(prevRawFull[idx].real(), prevRawFull[idx].imag()));
                    acc += z; mag += std::abs(z);
                }
                intScoreM[sh + intRange] += mag > 0 ? std::abs(acc) / mag : 0;
            }
        }
        prevRawFull = cur;
        intSymbols++;
        if (intSymbols >= 12) {
            int best = 0; double bm = -1, second = -1;
            const int nS = 2 * intRange + 1;
            for (int i = 0; i < nS; i++) if (intScore[i] > bm) { bm = intScore[i]; best = i; }
            for (int i = 0; i < nS; i++) if (i != best && intScore[i] > second) second = intScore[i];
            const double bmM = *std::max_element(intScoreM.begin(), intScoreM.end());
            if (bmM > 1.5 * bm) {
                // the continual pilots line up only with the spectrum turned round: conjugate the input from here on (and what is buffered)
                // and start the acquisition again, since the carrier offset and the timing were measured on the mirror image
                mirror = !mirror;
                for (auto& v : buf) v = std::conj(v);
                tpsOk = false; state = 0; detect = 1; agreeCount = 0;
                return;
            }
            if (bm > 0 && bm > 1.5 * second) { intShift = best - intRange; intLocked = true; }
            else { std::fill(intScore.begin(), intScore.end(), 0.0); std::fill(intScoreM.begin(), intScoreM.end(), 0.0); intSymbols = 0; }
        }
    }

    // process the next symbol; returns false if the data is not yet available
    bool step() {
        const int P = N + G;
        int64_t s = (int64_t)std::llround(symStart);
        if (s + P + N + 32 > end()) return false;
        if (s - 16 < base) { symStart += P; return true; } // fell behind the buffer: skip
        double terr, eraw, mag, rho;
        cpTrack(s + tOff, terr, eraw, mag, rho);
        // how reliable is this symbol's correlation? Against its usual strength, from the symbols that were fine
        const bool collapsed = rhoAvg > 0 && absSym > 40 && rho < 0.3 * rhoAvg;
        if (!collapsed) rhoAvg = rhoAvg == 0 ? rho : rhoAvg + (absSym < 200 ? 0.05 : 0.005) * (rho - rhoAvg);
        lowRun = collapsed ? lowRun + 1 : 0;
        if (lowRun >= 12) {
            if (!resync(s)) return false;
            return true;
        }
        // The correlation peak at the edge of the tracking range in two symbols running: the timing has jumped by more than the loop can follow
        // but less than collapses the correlation (a few hundred samples lost, the rest of a whole symbol), the window now overlaps the next
        // symbol and everything decodes wrong. Same remedy.
        // (once the window is placed under a strong echo, an edge peak that stands barely above the centre is the flat top of its
        // correlation, not a jump; elsewhere a jump of a few hundred samples puts both on the slope, barely apart)
        edgeRun = !collapsed && std::fabs(terr) >= 5.99 && ((tOff == 0 && !plateauKnown) || cpEdgeGain > 1.05) ? edgeRun + 1 : 0;
        if ((lowRun == 2 || edgeRun == 2) && tpsOk) {
            const double before = symStart;
            if (!resync(s, true)) { if (lowRun) lowRun = 1; else edgeRun = 1; return false; }   // more samples first; this symbol comes round again
            if (symStart != before) return true;
        }
        // timing loop (the CP correlation peak sits at the symbol start). Proportional plus integral: the integral learns the drift
        // of the sample clock, so the proportional part can stay small. With echoes the CP correlation peak is broad and noisy, and a
        // fast loop dithered the window by a sample tens of times a second, each step disturbing the channel estimate. It runs fast
        // for the first symbols after acquisition (to learn a clock offset of tens of ppm) and then slow.
        // A symbol whose correlation has collapsed says nothing about timing or frequency: noise would walk both loops away (a swing of half a
        // subcarrier was seen on a real recording), so they hold still until the correlation is back or the receiver has resynchronised.
        const bool pullIn = absSym < 500;
        // Under a strong echo (the window placed from the correlation's shape, tOff != 0) the peak sits anywhere on a flat top: following it
        // would walk the window (and the loop's drift estimate) at random. The placement keeps the window there; the loop only holds the drift.
        if (!collapsed && tOff == 0) {
            timingDrift += (pullIn ? 0.0016 : 0.000025) * terr;
            timingAcc += (pullIn ? 0.08 : 0.01) * terr + timingDrift;
        } else timingAcc += timingDrift;
        if (std::fabs(timingAcc) >= 1.0) { const double stp = std::round(timingAcc); symStart += stp; timingAcc -= stp; }
        // hand the learnt drift to the clock correction (every 16 symbols, once it says more than a couple of ppm); the loop then measures what is left
        if (!collapsed && absSym >= 60 && absSym % 16 == 0 && std::fabs(timingDrift) > 2e-6 * P) {
            clk.adjust(timingDrift / P, fn);
            timingDrift = 0;
        }
        // fractional CFO loop
        const double d = wrap(eraw - wrapFrac(epsFrac));
        if (!collapsed) epsFrac += 0.15 * d;
        if (epsFrac > 0.5 || epsFrac < -0.5) {
            // roll the integer part into the carrier shift
            const double step = epsFrac > 0 ? 1.0 : -1.0;
            epsFrac -= step;
            if (intLocked) intShift += (int)step;
        }
        // how far this window moved against the previous one beyond one symbol (the timing loop's steps), for the TPS differential
        winShift = prevWin == INT64_MIN ? 0 : (int)(s - prevWin - P);
        if (std::abs(winShift - placedShift) > 16) { winShift = 0; prevValid = false; }   // a skipped symbol: no valid previous one
        placedShift = 0;
        prevWin = s;
        // (takes effect from the next symbol; an average over a collapsed symbol, samples perhaps lost, says nothing: start it again)
        if (collapsed) profN = 0;
        else if (tpsOk && absSym % 2 == 0) placeWindow(s);
        transform(s);
        if (!intLocked) {
            // search needs the unshifted transform: the shifted Y is only a re-indexing of fbuf, which is intact after transform()
            integerSearch(s);
            if (state == 0) return true;
            symStart += P;
            absSym++;
            if (intLocked) { prevValid = false; tpsBitsSeen.clear(); tpsBase = absSym; }
            return true;
        }
        processCarriers();
        symStart += P;
        absSym++;
        symbols++;
        return true;
    }
    static double wrapFrac(double v) { return v; }

    // ------------------------------------------------------------------ carriers: TPS, frame sync, channel estimation, equalisation
    void processCarriers() {
        const auto& tpsK = tpsCarriers(mode);
        const auto& cpK = continualPilots(mode);
        {
        DECT2_STAGE("dvbt.tps");
        // ---- TPS bit for this symbol: differential BPSK against the previous symbol
        if (prevValid && prevTps.size() == tpsK.size() && prevCp.size() == cpK.size()) {
            // When the timing loop moved the FFT window by d samples since the previous symbol, carrier f turned by 2 pi f d / N:
            // turn the previous symbol's cells the same way, or the differential products partly cancel and the bit can flip.
            auto prevTurned = [&](const cf32& v, int k) {
                if (winShift == 0) return v;
                const double a = 2 * M_PI * (double)(k + intShift - kc) * winShift / N;
                return v * cf32((float)std::cos(a), (float)std::sin(a));
            };
            // All carriers also turn by one common phase from one symbol to the next. Most of it is the whole-carrier part of the carrier
            // offset: it is taken out by moving the carrier index, not by turning the samples, so its phase still advances by
            // 2 pi intShift (N + G) / N per symbol. With guard 1/4 that is a quarter turn for every carrier of offset: at 7 carriers (8 kHz
            // in 8K, a real capture) the real part of the differential is only noise, at 2 carriers every bit comes out inverted, and the
            // TPS never decodes. The rest is what the fractional loop has not taken out yet. The continual pilots carry the same value in
            // every symbol, so their differential measures that common turn directly: take it out of the TPS differential.
            cd turn = 0;
            for (size_t i = 0; i < cpK.size(); i++) {
                const cf32 z = Y[cpK[i]] * std::conj(prevTurned(prevCp[i], cpK[i]));
                turn += cd(z.real(), z.imag());
            }
            const double tm = std::abs(turn);
            const cf32 unturn = tm > 1e-12 ? cf32((float)(turn.real() / tm), (float)(-turn.imag() / tm)) : cf32(1, 0);
            double acc = 0;
            for (size_t i = 0; i < tpsK.size(); i++) acc += (Y[tpsK[i]] * std::conj(prevTurned(prevTps[i], tpsK[i])) * unturn).real();
            tpsBitsSeen.push_back(acc < 0 ? 1 : 0);
            if (symbols % 3 == 0) {
                tpsShow.clear();
                for (size_t i = 0; i < tpsK.size(); i++) { const cf32 z = Y[tpsK[i]] * std::conj(prevTurned(prevTps[i], tpsK[i])) * unturn; const float m = std::abs(z); if (m > 1e-9f) tpsShow.push_back(z / m); }
                showSeq++;   // new TPS cells for the plot
            }
        } else tpsBitsSeen.push_back(-1);
        prevTps.resize(tpsK.size());
        for (size_t i = 0; i < tpsK.size(); i++) prevTps[i] = Y[tpsK[i]];
        prevCp.resize(cpK.size());
        for (size_t i = 0; i < cpK.size(); i++) prevCp[i] = Y[cpK[i]];
        prevValid = true;
        if (tpsBitsSeen.size() > 4000) { tpsBitsSeen.erase(tpsBitsSeen.begin(), tpsBitsSeen.begin() + 2000); tpsBase += 2000; }
        }

        // ---- frame sync from the TPS block (sync word + BCH)
        if (!tpsOk) {
            findTps();
        } else {
            secSinceTps += (double)(N + G) / fn;
        }
        // ---- the scattered pilots tell the position inside the 4-symbol cycle even before TPS is known
        const int hypBefore = tpsOk ? (int)((absSym - frameStartAbs) % 68) : -1;
        (void)hypBefore;
        int symIdx = tpsOk ? (int)((absSym - frameStartAbs) % 68) : -1;
        if (!tpsOk && !pend.empty()) drainAll();
        if (!tpsOk && holding) flushHeld(0);
        if (!tpsOk) {
            // channel estimate not meaningful yet: only accumulate the pilot-phase votes and wait for TPS
            return;
        }
        if (phaseCheck && lowRun == 0) {
            drainAll();   // the symbols from before the gap go out first, ahead of the erasures and the hold
            // the first good symbol after a resync: whole symbols may have gone missing with the lost samples. Move the frame position on by that
            // many and give the error correction the same number of erased symbols, so that its bit stream stays aligned.
            phaseCheck = false;
            const int delta = scatteredPhaseDelta(symIdx);
            const bool light = phaseCheckLight;
            phaseCheckLight = false;
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] scattered pilot check at sym %llu: the real stream is %d symbol(s) ahead of the count\n", (unsigned long long)absSym, delta);
            if (delta > 0) {
                if (streaming) for (int i = 0; i < delta; i++) pushErasure((symIdx + i) % 68);
                frameStartAbs -= (uint64_t)delta; frameRefAbs -= (uint64_t)delta;
                symIdx = (symIdx + delta) % 68;
            }
            if (streaming && !(light && delta == 0)) { holding = true; holdFrom = absSym; }
        }
        if (lowRun > 0) { if (streaming) pushErasure(symIdx); }   // this symbol's correlation has collapsed: nothing to decode, nothing to learn from
        else estimateAndDecode(symIdx);
        if (holding && held.size() >= 24) realign();
        // per-frame TPS re-check at the frame boundary
        if (symIdx == 67) verifyFrame();
    }

    void findTps() {
        // look for the 16-bit sync word in the bit history
        const size_t n = tpsBitsSeen.size();
        if (n < 68 + 2) return;
        // search the newest 68+ window start positions
        for (size_t p = n >= 140 ? n - 140 : 0; p + 70 <= n; p++) {
            // bits s1..s16 live at symbols p+1..p+16 when the frame starts at p
            if (tpsBitsSeen[p + 1] < 0) continue;
            uint8_t b[68];
            bool ok = true;
            for (int i = 0; i < 68; i++) { const int v = tpsBitsSeen[p + i]; if (v < 0 && i > 0) { ok = false; break; } b[i] = v < 0 ? 0 : (uint8_t)v; }
            if (!ok) continue;
            Params q; int fi; bool odd;
            if (tpsDecode(b, q, fi, odd)) {
                if (q.mode != mode || q.guard != gi) { continue; }
                prm = q;
                frameStartAbs = tpsBase + p;
                frameIdx = fi;
                frameRefAbs = frameStartAbs; frameRefIdx = fi;
                tpsOk = true;
                secSinceTps = 0;
                tpsFailures = 0;
                fecPrm = prm;
                // everything before this point has been consumed; the symbols after p+67 are processed from now on, but the
                // symbol index of the current symbol is derived from the frame start
                return;
            }
        }
    }

    void verifyFrame() {
        // after a full frame: re-read its TPS bits (symbols frameStart.. +67 of the frame that just ended)
        const uint64_t f0 = absSym - 67;
        if (f0 < tpsBase || f0 - tpsBase + 68 > tpsBitsSeen.size()) return;
        uint8_t b[68];
        for (int i = 0; i < 68; i++) { const int v = tpsBitsSeen[f0 - tpsBase + i]; b[i] = v < 0 ? 0 : (uint8_t)v; }
        Params q; int fi; bool odd;
        const bool frameOk = tpsDecode(b, q, fi, odd, 2) && q.mode == mode && q.guard == gi;
        if (frameOk) {
            postResync = 0;
            tpsFailures = 0;
            secSinceTps = 0;
            frameIdx = fi;
            frameRefAbs = f0; frameRefIdx = fi;
            if (!(q == prm)) { prm = q; fecPrm = prm; }
        } else if (postResync > 0) {
            // After a resync the first frame that comes round holds the gap and is expected to fail. If the second one fails too, whole symbols went
            // missing (more than the scattered-pilot check can tell, which is only the position modulo four): look for the TPS block again at once,
            // keeping the symbol timing, the carrier offset and the channel, instead of waiting for three failures and starting from scratch.
            if (--postResync == 0) { tpsOk = false; tpsFailures = 0; if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] TPS still wrong after the resync at sym %llu: look for it again\n", (unsigned long long)absSym); }
        } else if (++tpsFailures >= 3) {
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] TPS verification failed 3 times at sym %llu: start over\n", (unsigned long long)absSym);
            // lost: start over with a fresh acquisition
            tpsOk = false; state = 0; detect = 1; agreeCount = 0;
        }
    }

    // A symbol that carries no information (its correlation has collapsed, or the channel estimate is still being rebuilt after a resync) must
    // still go through the error correction as a symbol of erased cells. Leaving it out would shift the bit stream by a whole symbol, and the
    // sync bytes, which bypass the interleaver, would still look right while every block fails.
    // Which of the four scattered-pilot positions does this symbol (in Y) have, against the one the receiver's symbol count says? The pilots sit on
    // carriers 3 * (symbol index mod 4) + 12 j with known signs, so the right position shows as pilots whose neighbours (12 carriers apart) agree
    // in phase, and a wrong one does not. Returns how many symbols the real stream is ahead of the count (0..3), or 0 when it cannot tell.
    int scatteredPhaseDelta(int symIdx) const {
        double score[4];
        for (int h = 0; h < 4; h++) {
            cd sum = 0, prev = 0; double mag = 0; bool have = false;
            for (int k = 3 * h; k < K; k += 12) {
                const cd v = cd(Y[k].real(), Y[k].imag()) * (double)pilotValue(k);
                if (have) { const cd z = v * std::conj(prev); sum += z; mag += std::abs(z); }
                prev = v; have = true;
            }
            score[h] = mag > 0 ? std::abs(sum) / mag : 0;
        }
        int best = 0;
        for (int h = 1; h < 4; h++) if (score[h] > score[best]) best = h;
        double second = 0;
        for (int h = 0; h < 4; h++) if (h != best) second = std::max(second, score[h]);
        if (score[best] < 0.5 || score[best] < 1.5 * second) return 0;
        return ((best - symIdx % 4) % 4 + 4) % 4;
    }

    void pushErasure(int symIdx) {
        if (ti && !pend.empty()) { Pend p = takePend(); p.symIdx = symIdx; p.erase = true; p.t = (int64_t)absSym; pend.push_back(std::move(p)); return; }
        pushErasureNow(symIdx);
    }
    void pushErasureNow(int symIdx) {
        const int Nd = dataCarriers(mode);
        std::vector<cf32> eq((size_t)Nd, cf32(0, 0));
        std::vector<float> n0((size_t)Nd, 1e6f);
        fecPush(eq.data(), n0.data(), symIdx);
    }
    // one symbol into the error correction (or into the hold after a resync), and the packets that come out to the callback
    void fecPush(const cf32* eq, const float* n0, int symIdx) {
        const int Nd = dataCarriers(mode);
        if (holding) { held.push_back({std::vector<cf32>(eq, eq + Nd), std::vector<float>(n0, n0 + Nd), symIdx}); return; }
        FecJob j = takeJob();
        j.eq.assign(eq, eq + Nd); j.n0.assign(n0, n0 + Nd);
        j.symIdx = symIdx; j.prm = fecPrm; j.lp = lpReq.load(); j.secs = (double)(N + G) / fn; j.seq = fecSeqOut++;
        fecPipe->push(std::move(j));
    }
    void flushHeld(int extra) {
        holding = false;
        if (held.empty()) return;
        const int Nd = dataCarriers(mode);
        if (extra > 0) {
            std::vector<cf32> eq((size_t)Nd, cf32(0, 0));
            std::vector<float> n0((size_t)Nd, 1e6f);
            for (int i = 0; i < extra; i++) fecPush(eq.data(), n0.data(), (held[0].symIdx + i) % 68);
        }
        for (auto& h : held) fecPush(h.eq.data(), h.n0.data(), (h.symIdx + extra) % 68);
        held.clear();
    }
    // Which whole number of four-symbol steps (beyond what the scattered pilots said) puts the TPS bits seen since the resync where they belong?
    // Every bit of the 68-bit block is known from the parameters and the frame number, so two dozen symbols leave one answer. Not clear: none.
    void realign() {
        std::array<std::array<uint8_t, 68>, 4> tb;
        for (int f = 0; f < 4; f++) tb[(size_t)f] = tpsBits(prm, f);
        int best = 0, bestMis = 1 << 30, secondMis = 1 << 30;
        for (int extra = 0; extra < 68; extra += 4) {
            int mis = 0;
            for (uint64_t a = holdFrom; a <= absSym; a++) {
                if (a < tpsBase || a - tpsBase >= tpsBitsSeen.size()) continue;
                const int v = tpsBitsSeen[(size_t)(a - tpsBase)];
                if (v < 0) continue;
                const int64_t rel = (int64_t)a - (int64_t)frameRefAbs + extra + 68 * frameRefIdx;
                const int pos = (int)(((rel % 272) + 272) % 272);
                if (pos % 68 == 0) continue;
                mis += v != tb[(size_t)(pos / 68)][(size_t)(pos % 68)];
            }
            if (mis < bestMis) { secondMis = bestMis; bestMis = mis; best = extra; }
            else if (mis < secondMis) secondMis = mis;
        }
        const bool sure = best > 0 && bestMis <= 2 && secondMis >= bestMis + 4;
        if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] TPS realignment at sym %llu: %d more symbol(s) lost (%d wrong bits, next best %d)%s\n", (unsigned long long)absSym, best, bestMis, secondMis, sure ? "" : ", kept");
        if (!sure) { flushHeld(0); return; }
        frameStartAbs -= (uint64_t)best; frameRefAbs -= (uint64_t)best;
        flushHeld(best);
    }

    void estimateAndDecode(int symIdx) {
        DECT2_STAGE_VAR(est, "dvbt.chanest");
        const auto& cp = continualPilots(mode);
        const auto& tpsK = tpsCarriers(mode);
        // ---- per-symbol common phase and timing slope from the continual pilots, relative to their running reference
        // (the work buffers are members, kept from symbol to symbol: nothing is allocated per symbol)
        std::vector<cf32>& c = ceC;
        c.resize(cp.size());
        for (size_t i = 0; i < cp.size(); i++) c[i] = Y[cp[i]] / pilotValue(cp[i]);
        if (cpRefValid) {
            // The symbol's common phase and phase ramp (timing) relative to the continual-pilot reference. The running totals
            // accumulate small per-symbol corrections, so nothing ever has to be unwrapped even when the clock drifts for minutes.
            // A step of the timing loop is not small: moving the window by d samples turns carrier f by 2 pi f d / N, nearly +-pi at
            // the band edges in 8K, so the fit below wraps and misses most of it, and the next symbols decode against a wrong channel.
            // The step is known, so add its ramp first and let the fit measure only what is left.
            if (winShift != 0) {
                accSlope += 2 * M_PI * winShift / N;
                accPhi += 2 * M_PI * (double)intShift * winShift / N;
            }
                        std::vector<cd>& z = ceZ;
            z.resize(cp.size());
            cd tot = 0;
            for (size_t i = 0; i < cp.size(); i++) {
                const double a = accPhi + accSlope * (double)(cp[i] - kc);
                z[i] = cd(c[i].real(), c[i].imag()) * std::polar(1.0, -a) * std::conj(cd(cpRef[i].real(), cpRef[i].imag()));
                tot += z[i];
            }
            const double phi0 = std::arg(tot);
            const cd rot0 = std::polar(1.0, -phi0);
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t i = 0; i < cp.size(); i++) {
                const cd zr = z[i] * rot0;
                const double w = std::norm(zr), x = (double)(cp[i] - kc), y = std::arg(zr);
                sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
            }
            double dSlope = 0, dPhi = phi0;
            const double det = sw * sxx - sx * sx;
            if (det > 1e-12) { dSlope = (sw * sxy - sx * sy) / det; dPhi = phi0 + (sy - dSlope * sx) / sw; }
            accPhi += dPhi; accSlope += dSlope;
        }
        const double phi = accPhi, slope = accSlope;
        // derotate the whole symbol by (phi + slope*(k-kc)): an exact phasor at the start of every block of 64 carriers and a table for the
        // steps inside it (a sine and a cosine per carrier cost more than the rest of the channel estimate)
        std::vector<cf32>& Yc = ceYc;
        Yc.resize(K);
        {
            constexpr int B = 64;
            cf32 tab[B];
            for (int j = 0; j < B; j++) tab[j] = cf32((float)std::cos(-slope * j), (float)std::sin(-slope * j));
            for (int k0 = 0; k0 < K; k0 += B) {
                const double a = -(phi + slope * (k0 - kc));
                const cf32 c0((float)std::cos(a), (float)std::sin(a));
                const int e = std::min(B, K - k0);
                for (int j = 0; j < e; j++) Yc[k0 + j] = Y[k0 + j] * (c0 * tab[j]);
            }
        }
        for (size_t i = 0; i < cp.size(); i++) {
            const cf32 v = Yc[cp[i]] / pilotValue(cp[i]);
            cpRef[i] = cpRefValid ? cpRef[i] * 0.85f + v * 0.15f : v;
        }
        cpRefValid = true;
        // ---- noise from the continual pilots against their smoothed reference
        {
            double e = 0;
            for (size_t i = 0; i < cp.size(); i++) e += std::norm(Yc[cp[i]] / pilotValue(cp[i]) - cpRef[i]);
            e /= (double)cp.size();
            const double s2 = std::max(1e-6, e * (16.0 / 9.0) * 1.2); // Y-domain variance; mild correction for the reference's own noise
            sigma2 = 0.9 * sigma2 + 0.1 * s2;
        }
        // ---- how fast does the channel change against the noise? The continual pilots one and two symbols back (in the same derotated
        // frame) answer it: E|c_l - c_l-1|^2 = 2 r + q and E|c_l - c_l-2|^2 = 2 r + 4 q for a channel that changes smoothly (q: its change
        // over one symbol, r: the noise of one pilot). That sets how much the scattered pilots are averaged over time below.
        {
            ceCv.resize(cp.size());
            for (size_t i = 0; i < cp.size(); i++) ceCv[i] = Yc[cp[i]] / pilotValue(cp[i]);
            if (cpHist >= 2 && ceH1.size() == cp.size() && ceH2.size() == cp.size()) {
                double d1 = 0, d2 = 0;
                for (size_t i = 0; i < cp.size(); i++) { d1 += std::norm(ceCv[i] - ceH1[i]); d2 += std::norm(ceCv[i] - ceH2[i]); }
                d1 /= (double)cp.size(); d2 /= (double)cp.size();
                if (ceStats == 0) { ceD1 = d1; ceD2 = d2; } else { ceD1 += 0.08 * (d1 - ceD1); ceD2 += 0.08 * (d2 - ceD2); }
                ceStats++;
            }
            ceH2.swap(ceH1); ceH1 = ceCv; cpHist = std::min(cpHist + 1, 2);
            if (ceStats >= 8) {
                const double r = std::max(1e-12, (4 * ceD1 - ceD2) / 6), q = std::max(0.0, (ceD2 - ceD1) / 3);
                // The gain a of the average over a grid point's pilots (one every fourth symbol): its noise is r a / (2 - a), and a channel
                // moving by v per pilot (|v|^2 = 16 q: a smooth change) leaves it (1 - a) / a pilots behind, an error of |v|^2 ((1 - a) / a)^2.
                // The gain with the least of the two (a Kalman gain for a random walk lags a moving receiver's channel: at 10 Hz in 8K the
                // MER stopped at 27 dB).
                double best = 1, bestE = 1e300;
                for (int i = 1; i <= 20; i++) {
                    const double a = i / 20.0;
                    const double e = 16 * q * ((1 - a) / a) * ((1 - a) / a) + r * a / (2 - a);
                    if (e < bestE) { bestE = e; best = a; }
                }
                ceAlpha = std::max(1.0 / 10, best);
            } else ceAlpha = 1;
        }
        // ---- scattered pilots of this symbol refresh one quarter of the grid (carriers 3 * (4j + l mod 4)). A still channel: averaged over
        // time with the gain above (noise of the estimate down by up to 10 dB, worth up to 1.9 dB of threshold); a fast one: the newest pilot.
        const int l4 = symIdx % 4;
        if (gridCnt.size() != grid.size()) { gridCnt.assign(grid.size(), 0); gHv.assign(grid.size() * kHist, cf32(0, 0)); gHt.assign(grid.size() * kHist, -1); }
        for (int k = 3 * l4; k < K; k += 12) {
            const int n = k / 3;
            const cf32 v = Yc[k] / pilotValue(k);
            if (ceOld || gridAge[n] > 8) { grid[n] = v; gridCnt[n] = 1; }
            else {
                gridCnt[n] = (uint16_t)std::min(1000, gridCnt[n] + 1);
                const float a = (float)std::max(ceAlpha, 1.0 / gridCnt[n]);
                grid[n] += a * (v - grid[n]);
            }
            {   // the last kHist estimates of this grid point and their symbols, newest last (a stale point starts again)
                cf32* hv = &gHv[(size_t)n * kHist]; int64_t* ht = &gHt[(size_t)n * kHist];
                if (gridAge[n] > 8) for (int j = 0; j < kHist; j++) ht[j] = -1;
                for (int j = 0; j + 1 < kHist; j++) { hv[j] = hv[j + 1]; ht[j] = ht[j + 1]; }
                hv[kHist - 1] = grid[n]; ht[kHist - 1] = (int64_t)absSym;
            }
            gridAge[n] = 0;
        }
        // grid points that coincide with continual pilots get the better (smoothed) estimate (not with the time interpolation: the smoothed
        // reference lags a fast channel, and those points get a scattered pilot every fourth symbol like all others)
        if (!ti) for (size_t i = 0; i < cp.size(); i++) if (cp[i] % 3 == 0) { grid[cp[i] / 3] = cpRef[i]; gridAge[cp[i] / 3] = 0; }
        for (auto& a : gridAge) if (a < 250) a++;
        gridFilled++;
        if (gridFilled < 4) { est.stop(); if (streaming || !pend.empty()) pushErasure(symIdx); return; }
        if (!ti) { equalise(Yc, symIdx, (int64_t)absSym); return; }
        // Time interpolation: every grid point gets a pilot every fourth symbol. The symbol kTiDelay back has two pilots on each grid point
        // before it and two after it (by now): its channel is interpolated between them, not the newest pilot, which for a moving receiver is
        // up to three symbols stale (at 100 Hz Doppler in 2K: a third of a radian, enough to wreck 16-QAM).
        Pend pe = takePend();
        pe.Yc.assign(Yc.begin(), Yc.end()); pe.symIdx = symIdx; pe.t = (int64_t)absSym; pe.erase = false;
        pend.push_back(std::move(pe));
        drainPend((int64_t)absSym - kTiDelay);
    }
    // the delay line of the time interpolation: symbols (or erasures) in the order they go to the error correction
    struct Pend { std::vector<cf32> Yc; int symIdx = 0; int64_t t = 0; bool erase = false; };
    std::deque<Pend> pend;
    std::vector<Pend> pendSpare;
    Pend takePend() { if (pendSpare.empty()) return Pend(); Pend p = std::move(pendSpare.back()); pendSpare.pop_back(); return p; }
    void drainPend(int64_t upTo) {
        while (!pend.empty() && (pend.front().erase || pend.front().t <= upTo)) {
            Pend p = std::move(pend.front());
            pend.pop_front();
            if (p.erase) pushErasureNow(p.symIdx);
            else {
                const size_t M = grid.size();
                std::vector<cf32>& gi = scrGridT;
                gi.resize(M);
                for (size_t n = 0; n < M; n++) {
                    gi[n] = timeInterp(n, p.t);
                }
                equalise(p.Yc, p.symIdx, p.t, &gi);
            }
            pendSpare.push_back(std::move(p));
        }
    }
    void drainAll() { drainPend(INT64_MAX); }
    std::vector<cf32> scrGridT;
    // Each grid point's last estimates (one every fourth symbol) and when they were taken. The delayed symbol's channel is a cubic through
    // the two estimates before it and the two after (a straight line between two loses 16 dB of MER at 40 Hz Doppler in 8K: the pilots are
    // 4.5 ms apart, a path turns by more than a radian in between); a line where fewer are known, the newest where nothing follows.
    static constexpr int kHist = 5, kTiDelay = 8;
    std::vector<cf32> gHv;
    std::vector<int64_t> gHt;
    cf32 timeInterp(size_t n, int64_t m) const {
        const cf32* hv = &gHv[n * kHist]; const int64_t* ht = &gHt[n * kHist];
        int j1 = -1;   // the first estimate after m
        for (int j = 0; j < kHist; j++) if (ht[j] >= 0 && ht[j] > m) { j1 = j; break; }
        if (j1 < 0) return grid[n];
        if (j1 == 0 || ht[j1 - 1] < 0) return hv[j1];
        if (j1 >= 2 && j1 + 1 < kHist && ht[j1 - 2] >= 0 && ht[j1 + 1] >= 0) {
            const double t[4] = {(double)ht[j1 - 2], (double)ht[j1 - 1], (double)ht[j1], (double)ht[j1 + 1]};
            cd acc = 0;
            for (int a = 0; a < 4; a++) {
                double w = 1;
                for (int b = 0; b < 4; b++) if (b != a) w *= ((double)m - t[b]) / (t[a] - t[b]);
                const cf32 v = hv[j1 - 2 + a];
                acc += cd(v.real(), v.imag()) * w;
            }
            return cf32((float)acc.real(), (float)acc.imag());
        }
        const double f = (double)(m - ht[j1 - 1]) / (double)(ht[j1] - ht[j1 - 1]);
        return hv[j1 - 1] + (hv[j1] - hv[j1 - 1]) * (float)f;
    }
    const bool ti = !getenv("DECT2_DVBT_NOTI");   // A/B switch: DECT2_DVBT_NOTI=1 uses the newest pilot on each grid point (as before)

    // ---- equalise one symbol with the channel interpolated from the grid (the current one, or the time-interpolated one for a delayed symbol)
    void equalise(const std::vector<cf32>& Yc, int symIdx, int64_t tSym, const std::vector<cf32>* gridAt = nullptr) {
        DECT2_STAGE_VAR(est, "dvbt.chanest");
        (void)tSym;
        const auto& cp = continualPilots(mode);
        const auto& tpsK = tpsCarriers(mode);
        const int l4 = symIdx % 4;
        const std::vector<cf32>& gsrc = gridAt ? *gridAt : grid;
        // The carriers were turned by the running phase ramp (accSlope), which carries every step of the window since the reference was set:
        // in the turned carriers a path at delay tau sits at tau + accSlope N / 2 pi. The delay window must move with it, or after the window
        // has moved by a few dozen samples (the timing loop over minutes of clock drift, or one placement under a strong echo) a path falls out
        // of it and the channel estimate folds it to the wrong delay.
        const double slopeDelay = accSlope * (double)N / (2 * M_PI);
        const int slopeD = (int)std::lround(slopeDelay);
        const double tau0 = (double)back + (double)G / 2.0 + slopeDelay;
        std::vector<cf32>& g = scrGrid;
        g.assign(gsrc.begin(), gsrc.begin() + (ptrdiff_t)std::min(gsrc.size(), (size_t)(K + 2) / 3));
        g.resize((size_t)(K + 2) / 3, cf32(0, 0));
        // the delay window: by default G/2 +- N/6 around the main path (all the pilot grid can tell apart). Once the channel's real delay
        // spread is known (from its impulse response, measured again every 16 symbols) only that much, with margin: the interpolator then
        // passes less noise (a single path in 8K: a window of ~100 samples instead of 2730, the estimate's noise down by more than 10 dB).
        if (!ceNoNarrow && ceDelayKnown) interp.run(g, 3, K, N, (double)back + slopeDelay + ceDelayCentre, H, std::min(1.0, ceDelayHalf / ((double)N / 6.0)));
        else interp.run(g, 3, K, N, tau0, H, 1.0);
        if (!ceNoNarrow && (gridFilled == 8 || (gridFilled > 8 && gridFilled % 16 == 0))) {
            // measured on the full-window estimate (a narrowed one cannot show an echo that has just appeared outside it)
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] chanest sym %llu: alpha %.3f d1 %.3g d2 %.3g sigma2 %.3g window %s %.0f +- %.0f\n", (unsigned long long)absSym, ceAlpha, ceD1, ceD2, sigma2, ceDelayKnown ? "narrow" : "full", ceDelayCentre, ceDelayHalf);
            std::vector<cf32>& Hw = ceHw;
            if (ceDelayKnown) interp.run(g, 3, K, N, tau0, Hw, 1.0); else Hw = H;
            int tMin, tMax;
            const int lo = std::max(-std::max(G / 2, 64), G / 2 - N / 6), hi = std::min(G * 3 / 2, G / 2 + N / 6);
            if (delaySpan(Hw, N, lo, hi, back + slopeD, tMin, tMax)) {
                const double centre = 0.5 * (tMin + tMax), half = 0.5 * (tMax - tMin) * 1.4 + 48;
                ceDelayKnown = half / ((double)N / 6.0) < 0.9;
                ceDelayCentre = centre; ceDelayHalf = half;
            } else ceDelayKnown = false;
        }
        // ---- equalise the data carriers
        if (roleMode != mode) {   // the pattern repeats every 4 symbols: the data carriers of each of the four, in order
            for (int l = 0; l < 4; l++) {
                std::vector<uint8_t> roles;
                carrierRoles(mode, l, roles);
                dataIdx[l].clear();
                for (int k = 0; k < K && k < (int)roles.size(); k++) if (roles[k] == 0) dataIdx[l].push_back(k);
            }
            roleMode = mode;
            carrierErase.reset(K);
        }
        const std::vector<int>& dk = dataIdx[symIdx % 4];
        const int Nd = dataCarriers(mode);
        if ((int)dk.size() < Nd) return;
        std::vector<cf32>& eq = scrEq;
        std::vector<float>& n0 = scrN0;
        eq.resize((size_t)Nd); n0.resize((size_t)Nd);
        // the data carriers' channel and value side by side first, then one plain loop over them (the same operations, which now vectorise)
        hD.resize((size_t)Nd); yD.resize((size_t)Nd);
        for (int i = 0; i < Nd; i++) { hD[(size_t)i] = H[(size_t)dk[(size_t)i]]; yD[(size_t)i] = Yc[(size_t)dk[(size_t)i]]; }
        {
            const cf32* __restrict hp = hD.data();
            const cf32* __restrict yp = yD.data();
            cf32* __restrict ep = eq.data();
            float* __restrict np = n0.data();
            // one reciprocal per carrier for the cell and its noise (instead of two float and one double division; the values move in
            // the last bits, the decoded stream does not)
            float s2h = (float)(0.5 * sigma2);
            if (flatN0) { double sr = 0; for (int i = 0; i < Nd; i++) sr += 1.0 / std::max(1e-9f, std::norm(hp[i])); s2h *= (float)(sr / Nd); }   // A/B: the mean noise variance for all
            for (int i = 0; i < Nd; i++) {
                const float r = 1.f / std::max(1e-9f, std::norm(hp[i]));
                ep[i] = yp[i] * std::conj(hp[i]) * r;
                np[i] = flatN0 ? s2h : s2h * r;
            }
        }
        carrierErase.apply(eq.data(), n0.data(), dk.data(), Nd, K);
        est.stop();
        fecPush(eq.data(), n0.data(), symIdx);
        streaming = true;
        // ---- display data
        const uint64_t showEvery = (uint64_t)std::max(6.0, std::ceil(0.025 * fn / (double)(N + G)));
        if (!showEver || symbols - lastShowSym >= showEvery) {
            DECT2_STAGE("dvbt.telemetry");
            showEver = true; lastShowSym = symbols; showSeq++;
            eqShow.clear();
            const int step = std::max(1, Nd / 1500);
            for (int i = 0; i < Nd; i += step) eqShow.push_back(eq[i]);
            rawShow.clear();
            const int st2 = std::max(1, K / 1500);
            for (int k = 0; k < K; k += st2) rawShow.push_back(Yc[k] / std::max(1e-3f, std::abs(H[k])));
            chMag.clear(); chPh.clear();
            const int dec = std::max(1, K / 2048);
            for (int k = 0; k < K; k += dec) { chMag.push_back(20.f * std::log10(std::max(1e-6f, std::abs(H[k])))); chPh.push_back(std::arg(H[k])); }
            chDecimV = dec;
            pilotShow.clear();   // equalised pilots (BPSK, +-1 after removing the 4/3 boost): a quick picture of the channel estimate's quality
            for (int k = 3 * l4; k < K; k += 12) pilotShow.push_back(Yc[k] / H[k] * 0.75f);
            for (int k : cp) if (k % 12 != 3 * l4) pilotShow.push_back(Yc[k] / H[k] * 0.75f);
            const int back0 = G / 8;
            irTauMin = -N / 16;
            impulseResponse(H, N, irTauMin, G + std::max(8, G / 4), back + back0 + (int)std::lround(accSlope * (double)N / (2 * M_PI)), irDb);
            double sn = 0;
            for (int k = 0; k < K; k += 7) sn += std::norm(H[k]);
            sn /= (double)((K + 6) / 7);
            snrDb = 10 * std::log10(std::max(1e-9, sn / sigma2));
        }
        lastTpsK = tpsK.size();
    }
    int chDecimV = 1;
    size_t lastTpsK = 0;
    std::vector<cf32> ceC, ceYc, ceCv;   // estimateAndDecode()'s work buffers
    std::vector<cf32> ceH1, ceH2;     // the continual pilots one and two symbols back (for the noise / channel-change estimate)
    int cpHist = 0, ceStats = 0;
    double ceD1 = 0, ceD2 = 0, ceAlpha = 1;
    std::vector<uint16_t> gridCnt;    // pilots averaged into each grid point since it was last fresh
    const bool ceOld = getenv("DECT2_DVBT_CE_OLD") != nullptr;   // A/B: the newest pilot only (as before 3.0)
    const bool ceNoNarrow = getenv("DECT2_DVBT_NONARROW") != nullptr;   // A/B: the full delay window always
    const bool ceNoPlace = getenv("DECT2_DVBT_NOPLACE") != nullptr;      // A/B: the window where the cyclic-prefix peak puts it
    uint64_t placements = 0;
    bool ceDelayKnown = false;
    double ceDelayCentre = 0, ceDelayHalf = 0;
    std::vector<cf32> ceHw;
    std::vector<cd> ceZ;
    std::vector<int> dataIdx[4];   // the data carriers of the four symbol patterns (estimateAndDecode)
    int roleMode = -1;
    std::vector<cf32> hD, yD;

    // ------------------------------------------------------------------ the error correction stage
    // Demapping, Viterbi and Reed-Solomon of one symbol after another. Synchronous by default (feed() returns with everything decoded); with
    // setPipelined(true) (live radios) on a thread of its own behind a bounded queue, so the thread that feeds the receiver (the one the radio's
    // ring waits for) only resamples, synchronises and equalises: in 8K 64-QAM 2/3 the decoder is more than half of the work. A queue that
    // overflows (the decoder is too slow for this machine) drops the oldest symbols; the decoder then starts afresh at the next one (it finds
    // the packet sync again within a block) and the drops are counted (fecDrops, logged with DECT2_DEBUG).
    struct FecJob { std::vector<cf32> eq; std::vector<float> n0; int symIdx = 0; Params prm; bool lp = false; double secs = 0; uint64_t seq = 0; };
    Params fecPrm;                   // what the decoder is configured with: applied in order with the symbols (producer)
    uint64_t fecSeqOut = 0;          // producer: sequence number of the next symbol
    uint64_t fecSeqIn = 0;           // decoder: the one it expects
    std::atomic<uint64_t> fecDrops{0};
    std::vector<uint8_t> pktBuf;     // decoder thread from here on
    double streamSecs = 0;
    uint64_t packetsOut = 0;
    std::mutex fecMu;                // guards fecSnap and spare
    FecStats fecSnap;
    std::vector<FecJob> spare;       // jobs whose buffers can be used again
    FecJob takeJob() {
        std::lock_guard<std::mutex> lk(fecMu);
        if (spare.empty()) return FecJob();
        FecJob j = std::move(spare.back());
        spare.pop_back();
        return j;
    }
    void fecRun(FecJob&& j) {
        if (j.seq != fecSeqIn) {   // the queue overflowed: symbols are missing
            fecDrops += j.seq - fecSeqIn;
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] DVB-T decoder too slow (stages dvbt.viterbi, dvbt.demap, dvbt.rs): %llu symbols dropped\n", (unsigned long long)(j.seq - fecSeqIn));
            fec.reset();
        }
        fecSeqIn = j.seq + 1;
        fec.configure(j.prm);
        if (j.lp != fec.lowPriority()) fec.setLowPriority(j.lp);
        fec.setComputeMode(computeMode.load(std::memory_order_relaxed));
        fec.pushSymbol(j.eq.data(), j.n0.data(), j.symIdx);
        streamSecs += j.secs;
        fec.takePackets(pktBuf);
        if (!pktBuf.empty()) {
            packetsOut += pktBuf.size() / 188;
            detect = 3;
            if (cb) cb(pktBuf.data(), pktBuf.size() / 188, streamSecs);
            streamSecs = 0;
            pktBuf.clear();
        } else if (detect.load() < 2) detect = 2;
        std::lock_guard<std::mutex> lk(fecMu);
        fecSnap = fec.stats();
        if (spare.size() < 16) spare.push_back(std::move(j));
    }
    // drops what is queued, waits for the decoder to be idle, then resets it (producer)
    void fecReset() {
        if (fecPipe) fecPipe->reset();
        fec.reset();
        fecSeqIn = fecSeqOut;
        streamSecs = 0; packetsOut = 0; pktBuf.clear();
        std::lock_guard<std::mutex> lk(fecMu);
        fecSnap = fec.stats();
    }
    bool fecBlocking = false;
    void makeFecPipe(bool threaded, bool blocking) {
        fecPipe.reset();
        fecBlocking = blocking;
        fecPipe = std::make_unique<StagePipeline<FecJob>>("onair-dvbt-fec", [this](FecJob&& j) { fecRun(std::move(j)); }, 192,
                                                          blocking ? QueuePolicy::Block : QueuePolicy::DropOldest, threaded);
    }
    std::unique_ptr<StagePipeline<FecJob>> fecPipe;   // last: destroyed first, so no job runs on a half-destroyed receiver

    // ------------------------------------------------------------------ telemetry
    void publish() {
        DECT2_STAGE("dvbt.telemetry");
        RxTelemetry t;
        t.standard = 1;
        t.rateOk = rateOk;
        t.decimating = decimate;
        t.inputRate = inRate; t.nativeRate = fn;
        t.state = state == 0 ? 0 : tpsOk ? 2 : 1;
        t.fftN = N; t.guard = G; t.giIdx = gi; t.carriers = K;
        t.cfoHz = (epsFrac + intShift) * fn / std::max(1, N) + mixer.hz;
        t.symbolsPerFrame = 68;
        t.frameMs = N ? 68.0 * (N + G) / fn * 1e3 : 0;
        t.symbols = symbols;
        t.dataValid = tpsOk && gridFilled >= 4;
        t.dataSnrDb = (float)snrDb;
        t.cpSnrDb = (float)snrDb;
        const bool fresh = showSeq != pubShowSeq;
        if (fresh) {
            t.eqData = eqShow;
            t.rawCells = rawShow;
            t.eqCells = pilotShow;
            t.p1Const = tpsShow;
            t.chMagDb = chMag; t.chPhase = chPh;
            t.irDb = irDb;
        }
        t.chValid = !chMag.empty();
        t.chDecim = chDecimV; t.chCarriers = K;
        t.irTauMin = irTauMin;
        t.secSinceP1 = secSinceTps;
        t.l1preOk = tpsOk; t.l1postOk = tpsOk;
        t.dvbt.tpsOk = tpsOk; t.dvbt.mode = mode; t.dvbt.guard = gi;
        if (tpsOk) { t.dvbt.mod = prm.mod; t.dvbt.hier = prm.hier; t.dvbt.crHp = prm.crHp; t.dvbt.crLp = prm.crLp; t.dvbt.cellId = prm.cellId; t.dvbt.frameIdx = frameIdx; }
        FecStats fs;
        { std::lock_guard<std::mutex> lk(fecMu); fs = fecSnap; }
        t.dvbt.fecSync = fs.syncLocked; t.dvbt.packets = fs.packets; t.dvbt.rsClean = fs.rsClean; t.dvbt.rsCorrected = fs.rsCorrected; t.dvbt.rsFailed = fs.rsFailed;
        t.dvbt.viterbiMargin = fs.viterbiMargin; t.dvbt.punctPhase = fs.punctPhase; t.dvbt.secSinceTps = secSinceTps;
        t.dvbt.symbolIdx = tpsOk ? (int)((absSym - frameStartAbs) % 68) : -1;
        t.dvbt.fecDropped = fecDrops.load();
        t.dvbt.viterbiOnGpu = fs.viterbiOnGpu;
        t.plpOnGpu = fs.viterbiOnGpu;   // (the field the T2 status shows for its LDPC: "which device decodes")
        t.gpuAvailable = GpuViterbi::instance().available(); t.computeMode = computeMode.load();
        t.blocksOk = fs.rsClean + fs.rsCorrected; t.blocksBad = fs.rsFailed;
        t.plpValid = tpsOk; t.plpFrames = fs.packets;
        t.plpMerDb = snrDb;
        std::lock_guard<std::mutex> lk(mu);
        if (!fresh) {   // the plots did not change since the last publish: keep the ones in the telemetry (moved, not copied)
            t.eqData = std::move(tel.eqData); t.rawCells = std::move(tel.rawCells); t.eqCells = std::move(tel.eqCells); t.p1Const = std::move(tel.p1Const);
            t.chMagDb = std::move(tel.chMagDb); t.chPhase = std::move(tel.chPhase); t.irDb = std::move(tel.irDb);
        } else { pubShowSeq = showSeq; }
        t.seq = ++seq;
        tel = std::move(t);
    }
};

DvbtReceiver::DvbtReceiver() : p_(new Impl) { p_->makeFecPipe(false, false); }
DvbtReceiver::~DvbtReceiver() = default;

void DvbtReceiver::configure(double inputRateHz, double bandwidthMhz) {
    Impl& I = *p_;
    I.reset();
    I.inRate = inputRateHz; I.bwMhz = bandwidthMhz;
    I.fn = nativeRateHz(bandwidthMhz);
    I.mixer.set(0, inputRateHz);
    I.locator.configure(inputRateHz, carriersK(dvbt::k8K) * I.fn / 8192.0);
    I.rateOk = I.resampler.configure(inputRateHz, I.fn, nullptr, carriersK(dvbt::k8K) * I.fn / 8192.0) &&   // the signal fills K carriers: only what folds into them has to be stopped (a kernel half as long at 10 Msps)
                  inputRateHz >= 7.9e6 * (bandwidthMhz / 8.0);
    I.decimate = I.rateOk && !I.resampler.passthrough();
    I.clk.merged = false;
    if (I.decimate) { I.clk.setMerged(inputRateHz, I.fn); I.clk.reset(); }
}

void DvbtReceiver::setLowPriority(bool lp) { p_->lpReq = lp; }
void DvbtReceiver::setComputeMode(int m) { p_->computeMode = m; }   // applied with the next symbol, on the thread that decodes

void DvbtReceiver::setPipelined(bool on, bool blocking) {
    Impl& I = *p_;
    if (I.fecPipe->threaded() == (on && threadingEnabled()) && I.fecBlocking == blocking) return;
    I.fecPipe->flush();
    I.makeFecPipe(on, blocking);
}

void DvbtReceiver::flush() { p_->fecPipe->flush(); p_->publish(); }   // (the telemetry then has the decoder's final counts)

void DvbtReceiver::reset() { Impl& I = *p_; const double r = I.inRate, b = I.bwMhz; configure(r, b); }

void DvbtReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->cb = std::move(cb); }
int DvbtReceiver::detectLevel() const { return p_->detect.load(); }

void DvbtReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rateOk || !n) return;
    DECT2_STAGE_VAR(inp, "dvbt.input");
    // DC removal (the radio's centre spike, a HackRF's is several times a carrier): the mean of the block moves a slow estimate. Left in,
    // it lands on the centre carrier, which is a continual pilot: that pilot's error then poisons the common phase, the noise estimate
    // and, through the pilot grid, the channel estimate of every carrier, and nothing decodes. The time constant is long (50 ms) so that
    // the centre pilot itself, which turns at the carrier offset, is not taken out with it unless that offset is within a few hertz.
    {
        float sr4[4] = {0, 0, 0, 0}, si4[4] = {0, 0, 0, 0};
        size_t k = 0;
        for (; k + 4 <= n; k += 4) for (size_t l = 0; l < 4; l++) { sr4[l] += x[k + l].real(); si4[l] += x[k + l].imag(); }
        for (; k < n; k++) { sr4[0] += x[k].real(); si4[0] += x[k].imag(); }
        float sr = (sr4[0] + sr4[1]) + (sr4[2] + sr4[3]), si = (si4[0] + si4[1]) + (si4[2] + si4[3]);
        if (!std::isfinite(sr) || !std::isfinite(si)) {
            // NaN or infinite samples (a broken file or driver): one of them would stick in the DC estimate and the filters for good
            I.dcOut.assign(x, x + n);
            sr = si = 0;
            for (auto& v : I.dcOut) { if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0); sr += v.real(); si += v.imag(); }
            x = I.dcOut.data();
        }
        const float w = (float)std::min(1.0, (double)n / (0.05 * I.inRate));
        I.dc += (cf32(sr, si) / (float)n - I.dc) * w;
        I.dcOut.resize(n);
        const float dr = I.dc.real(), di = I.dc.imag();
        for (size_t q = 0; q < n; q++) I.dcOut[q] = cf32(x[q].real() - dr, x[q].imag() - di);
        x = I.dcOut.data();
    }
    // A channel recorded beside the centre (gqrx, SDR#) or a radio tuned far off: until the TPS is found, the spectrum says where the channel
    // is; when that is further from the current guess than the carrier search covers, mix it to the centre and start the acquisition again.
    if (!I.tpsOk) {
        double hz; bool found;
        if (I.locator.feed(I.dcOut.data(), n, hz, found) && found && std::fabs(hz - I.mixer.hz) > 40e3) {
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] channel found %.1f kHz from the centre\n", hz / 1e3);
            I.restartAt(hz);
        }
    }
    I.mixer.apply(I.dcOut.data(), n);
    if (I.mirror) for (size_t q = 0; q < n; q++) I.dcOut[q] = std::conj(I.dcOut[q]);
    inp.stop();
    {
        DECT2_STAGE("dvbt.resample");
        I.stage.clear();
        if (I.decimate && I.clk.merged) I.clk.rs.process(x, n, I.stage);   // resampling and clock correction in one filter
        else if (I.decimate) I.resampler.process(x, n, I.stage);
        else I.stage.assign(x, x + n);
        I.clk.process(I.stage);
        I.buf.insert(I.buf.end(), I.stage.begin(), I.stage.end());
    }
    // work through the buffer
    for (int guard = 0; guard < 100000; guard++) {
        if (I.state == 0) {
            const size_t need = (size_t)(6.0 * (8192 + 2048) + 8192 + 2048);
            if (I.buf.size() < need) break;
            // acquisition every ~2 symbols of the longest mode; keep a sliding window
            if (!I.acquire()) {
                // After two seconds without a cyclic prefix (no signal, an empty channel) the search looks at every sample once instead
                // of twice: the windows still overlap by an eighth (a symbol of the longest mode), the CPU of the search halves.
                const bool slow = I.searchSamples > (uint64_t)(2 * I.fn);
                const size_t drop = I.buf.size() - (slow ? need / 8 : need / 2);
                I.searchSamples += drop;
                I.buf.erase(I.buf.begin(), I.buf.begin() + drop);
                I.base += (int64_t)drop;
                break;
            }
        } else {
            I.searchSamples = 0;
            if (!I.step()) break;
            // free what is no longer needed
            const int64_t keep = (int64_t)std::llround(I.symStart) - 2 * (I.N + I.G) - 64;
            if (keep - I.base > (int64_t)(1 << 20)) {
                const size_t drop = (size_t)(keep - I.base);
                I.buf.erase(I.buf.begin(), I.buf.begin() + drop);
                I.base += (int64_t)drop;
            }
        }
    }
    I.publish();
}

bool DvbtReceiver::telemetry(RxTelemetry& out, uint64_t lastSeq) {
    Impl& I = *p_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.tel.seq <= lastSeq) return false;
    out = I.tel;
    return true;
}

} // namespace dect2
