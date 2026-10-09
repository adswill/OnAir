#include "dect2/dvbt_rx.h"
#include "dect2/dvbt_fec.h"
#include "dect2/exact_resampler.h"
#include "dect2/fftutil.h"
#include "dect2/resampler.h"
#include "dect2/t2.h"
#include "dect2/t2ofdm.h"
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
    struct ClockCorrector {
        ExactResampler rs;
        bool on = false;
        std::vector<cf32> out;
        double ppm() const { return on ? (rs.step() - 1.0) * 1e6 : 0.0; }
        void reset() { on = false; out.clear(); }
        // the samples run fast by rel (positive: more samples than nominal): take that much more input per output sample
        void adjust(double rel, double rate) {
            if (!on) { rs.configure(rate, rate * (1.0 + 1e-9)); rs.scaleStep(1.0 + 1e-9); on = true; }
            rs.scaleStep(1.0 + rel);
        }
        void process(std::vector<cf32>& io) { if (!on) return; out.clear(); rs.process(io.data(), io.size(), out); io.swap(out); }
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
    double timingAcc = 0;         // fractional timing error accumulated by the loop
    double timingDrift = 0;       // samples per symbol the window drifts (sample clock offset), learnt by the loop
    int64_t prevWin = INT64_MIN;  // start of the previous symbol's FFT window
    int winShift = 0;             // this window's start minus (previous start + one symbol)
    double accPhi = 0, accSlope = 0;   // running common phase and phase ramp relative to the continual-pilot reference
    double epsFrac = 0;           // fractional carrier-frequency offset in subcarrier spacings (tracked)
    double rhoAvg = 0;            // usual strength of the cyclic-prefix correlation (|sum| / energy), from the symbols that were fine
    int lowRun = 0;               // symbols in a row whose correlation has collapsed: the receiver has lost the symbol timing
    int intShift = 0;
    int intRange = 20;            // whole carriers the integer search covers each way: 50 ppm at the top of UHF (43 kHz), with margin
    bool intLocked = false;
    std::vector<double> intScore; // accumulated pilot-coherence metric per candidate shift
    int intSymbols = 0;
    uint64_t absSym = 0;          // symbols processed since lock
    int back = 0;
    std::unique_ptr<Fft> fft;
    std::vector<cf32> fbuf;
    std::atomic<int> detect{0};
    int agreeCount = 0, agreeMode = -1, agreeGi = -1;
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
    std::vector<uint8_t> pktBuf;
    double streamSecs = 0;
    uint64_t symbols = 0;
    uint64_t packetsOut = 0;
    std::function<void(const uint8_t*, size_t, double)> cb;

    // ---- telemetry
    std::mutex mu;
    RxTelemetry tel;
    uint64_t seq = 0;
    std::vector<cf32> eqShow, rawShow, tpsShow, pilotShow;
    std::vector<float> chMag, chPh;
    std::vector<float> irDb;
    int irTauMin = 0;
    double cfoHz = 0;
    int64_t lastPublishSym = -100;

    void reset() {
        state = 0; mode = gi = -1; N = G = K = 0; symStart = 0; epsFrac = 0; intShift = 0; intLocked = false; rhoAvg = 0; lowRun = 0; streaming = false; phaseCheck = false; postResync = 0;
        intScore.clear(); intSymbols = 0; absSym = 0; fft.reset(); agreeCount = 0; agreeMode = agreeGi = -1; acqLastEnd = 0;
        tpsBitsSeen.clear(); tpsBase = 0; prevTps.clear(); prevCp.clear(); prevValid = false; prevWin = INT64_MIN; tpsOk = false; tpsFailures = 0; secSinceTps = 0;
        for (int i = 0; i < 4; i++) { hypVotes[i] = 0; hypScore[i] = 0; }
        grid.clear(); gridAge.clear(); cpRef.clear(); cpRefValid = false; gridFilled = 0;
        fec.reset(); streamSecs = 0; symbols = 0; packetsOut = 0; detect = 0;
        buf.clear(); base = 0; resampler.reset(); dc = cf32(0, 0); clk.reset();
        eqShow.clear(); rawShow.clear(); chMag.clear(); chPh.clear(); irDb.clear();
    }

    // ------------------------------------------------------------------ acquisition: FFT size and guard interval from the cyclic prefix
    bool acquire() {
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
            // products y[i] conj(y[i+n]) and energies, as cumulative sums
            std::vector<cd> cum(Dn + 1);
            std::vector<double> rr;
            std::vector<double> en(Dn + 1);
            cd acc = 0; double ea = 0;
            cum[0] = 0; en[0] = 0;
            for (int64_t i = 0; i < Dn; i++) {
                const cf32 a = buf[buf.size() - B + i], b = buf[buf.size() - B + i + n];
                acc += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
                ea += 0.5 * (std::norm(a) + std::norm(b));
                cum[i + 1] = acc; en[i + 1] = ea;
            }
            for (int g = 0; g < 4; g++) {
                const int G_ = dvbt::guardSamples(m, g), P = n + G_;
                const int64_t D = Dn - G_;
                if (D < P * 3) continue;
                std::vector<double> F(P, 0.0);
                std::vector<cd> Fc(P, cd(0, 0));
                std::vector<int> cnt(P, 0);
                // runs on every block while searching, so keep it lean: the normalised magnitudes in one plain loop the compiler can
                // vectorise (sqrt of the norm: std::abs goes through the much slower overflow-safe hypot), then the fold by symbol phase
                // with a wrapping index instead of d % P
                rr.resize((size_t)D);
                for (int64_t d = 0; d < D; d++) {
                    const double cr = cum[d + G_].real() - cum[d].real(), ci = cum[d + G_].imag() - cum[d].imag();
                    const double e = en[d + G_] - en[d];
                    rr[d] = e > 1e-12 ? std::sqrt(cr * cr + ci * ci) / e : 0.0;
                }
                for (int64_t d = 0, ph = 0; d < D; d++) {
                    F[ph] += rr[d]; Fc[ph] += cum[d + G_] - cum[d]; cnt[ph]++;
                    if (++ph == P) ph = 0;
                }
                double mx = 0, mean = 0; int arg = 0;
                for (int i = 0; i < P; i++) { if (cnt[i]) F[i] /= cnt[i]; mean += F[i]; if (F[i] > mx) { mx = F[i]; arg = i; } }
                mean /= P;
                const double ratio = mean > 0 ? mx / mean : 0;
                // a clean cyclic prefix gives a folded peak far above the floor; wrong (N, G) hypotheses stay flat
                const double score = mx * std::min(ratio, 12.0) / 12.0;
                if (mx > 0.12 && ratio > 3.0 && score > best.score) {
                    best.score = score; best.mode = m; best.gi = g;
                    // symbol start in absolute samples: the first occurrence of the peak phase after the oldest used sample
                    const int64_t first = b0 + (int64_t)buf.size() - B;
                    best.pos = first + arg;
                    best.corr = Fc[arg] / (double)std::max(1, cnt[arg]);
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
        timingAcc = 0; timingDrift = 0; accPhi = accSlope = 0; rhoAvg = 0; lowRun = 0;
        epsFrac = -std::arg(best.corr) / (2 * M_PI);   // c = sum y[n] conj(y[n+N]) has phase -2 pi eps
        back = std::min(G / 4, 24);
        fft = std::make_unique<Fft>(N);
        fbuf.assign(N, cf32(0, 0));
        // a radio 50 ppm off at 860 MHz is 43 kHz away: 38 carriers in 8K at 8 MHz, 62 at 5 MHz. Search +-55 kHz.
        intRange = std::max(20, (int)std::ceil(55e3 / (fn / N)));
        intScore.assign((size_t)(2 * intRange + 1), 0.0); intSymbols = 0; intLocked = false; intShift = 0;
        tpsBitsSeen.clear(); tpsBase = 0; prevValid = false; prevWin = INT64_MIN; tpsOk = false; absSym = 0;
        grid.assign((size_t)(K + 2) / 3 + 1, cf32(0, 0)); gridAge.assign(grid.size(), 255);
        cpRef.assign(continualPilots(mode).size(), cf32(1, 0)); cpRefValid = false; gridFilled = 0;
        for (int i = 0; i < 4; i++) { hypVotes[i] = 0; hypScore[i] = 0; }
        fec.reset(); streaming = false; phaseCheck = false; postResync = 0;
        state = 1;
        return true;
    }

    // ------------------------------------------------------------------ one symbol
    // Transforms the symbol whose CP starts at `s` (integer) into carriers Y[0..K-1]
    void transform(int64_t s) {
        const int64_t w = s + G - back;
        const double ph0 = -2.0 * M_PI * epsFrac * (double)(w % 1048576) / N;
        const double dph = -2.0 * M_PI * epsFrac / N;
        // fractional carrier-offset correction: an exact phasor at the start of every block of 64 samples and a table for the
        // offsets inside it, so no phasor recurrence runs through the whole symbol (and the multiplications vectorise)
        constexpr int B = 64;
        cf32 tab[B];
        for (int i = 0; i < B; i++) tab[i] = cf32((float)std::cos(dph * i), (float)std::sin(dph * i));
        const cf32* src = &at(w);
        for (int i0 = 0; i0 < N; i0 += B) {
            const double a = ph0 + dph * i0;
            const cf32 c0((float)std::cos(a), (float)std::sin(a));
            for (int j = 0; j < B; j++) fbuf[i0 + j] = src[i0 + j] * (c0 * tab[j]);
        }
        fft->forward(fbuf.data());
        Y.resize(K);
        for (int k = 0; k < K; k++) Y[k] = fbuf[(((k + intShift - kc) % N) + N) % N];
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
    bool resync(int64_t s) {
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
        lowRun = 0;
        if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] resync at sym %llu: best offset %d (rho %.2f, usual %.2f)\n", (unsigned long long)absSym, bestD >= P / 2 ? bestD - P : bestD, bestRho, rhoAvg);
        if (bestRho >= std::max(0.3, 0.5 * rhoAvg)) {
            // The boundary repeats every P samples. Take the shift nearest to zero, not the first one ahead: if some samples (less than half a
            // symbol) went missing, the symbol that follows starts a little earlier than expected, and stepping back keeps the receiver's count of
            // symbols (and so the frame position and the transport stream alignment) right. A step ahead by almost a whole symbol would skip one.
            if (bestD >= P / 2) bestD -= P;
            symStart = (double)(s + bestD);
            timingAcc = 0; accPhi = accSlope = 0;
            prevValid = false; prevWin = INT64_MIN; winShift = 0;
            cpRefValid = false; std::fill(gridAge.begin(), gridAge.end(), (uint8_t)255); gridFilled = 0;
            resyncs++;
            phaseCheck = true; postResync = 2;
            return true;
        }
        tpsOk = false; state = 0; detect = 1; agreeCount = 0;
        return true;
    }
    uint64_t resyncs = 0;
    bool streaming = false;       // the error correction has been fed symbols without a break since the last acquisition
    bool phaseCheck = false;      // after a resync: check once which of the four scattered-pilot positions the next good symbol has
    int postResync = 0;           // frame checks still to come after a resync: the first one is expected to fail (that frame holds the gap)

    static double wrap(double v) { while (v > 0.5) v -= 1; while (v < -0.5) v += 1; return v; }

    // integer CFO: continual pilots keep their value in every symbol, so Y_l[k+s] conj(Y_{l-1}[k+s]) has one common phase when s is right
    std::vector<cf32> prevRawFull;     // previous symbol's full FFT (for the integer search over a range of shifts)
    void integerSearch(int64_t s) {
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
            }
        }
        prevRawFull = cur;
        intSymbols++;
        if (intSymbols >= 12) {
            int best = 0; double bm = -1, second = -1;
            const int nS = 2 * intRange + 1;
            for (int i = 0; i < nS; i++) if (intScore[i] > bm) { bm = intScore[i]; best = i; }
            for (int i = 0; i < nS; i++) if (i != best && intScore[i] > second) second = intScore[i];
            if (bm > 0 && bm > 1.5 * second) { intShift = best - intRange; intLocked = true; }
            else { std::fill(intScore.begin(), intScore.end(), 0.0); intSymbols = 0; }
        }
    }

    // process the next symbol; returns false if the data is not yet available
    bool step() {
        const int P = N + G;
        int64_t s = (int64_t)std::llround(symStart);
        if (s + P + N + 32 > end()) return false;
        if (s - 16 < base) { symStart += P; return true; } // fell behind the buffer: skip
        double terr, eraw, mag, rho;
        cpTrack(s, terr, eraw, mag, rho);
        // how reliable is this symbol's correlation? Against its usual strength, from the symbols that were fine
        const bool collapsed = rhoAvg > 0 && absSym > 40 && rho < 0.3 * rhoAvg;
        if (!collapsed) rhoAvg = rhoAvg == 0 ? rho : rhoAvg + (absSym < 200 ? 0.05 : 0.005) * (rho - rhoAvg);
        lowRun = collapsed ? lowRun + 1 : 0;
        if (lowRun >= 12) {
            if (!resync(s)) return false;
            return true;
        }
        // timing loop (the CP correlation peak sits at the symbol start). Proportional plus integral: the integral learns the drift
        // of the sample clock, so the proportional part can stay small. With echoes the CP correlation peak is broad and noisy, and a
        // fast loop dithered the window by a sample tens of times a second, each step disturbing the channel estimate. It runs fast
        // for the first symbols after acquisition (to learn a clock offset of tens of ppm) and then slow.
        // A symbol whose correlation has collapsed says nothing about timing or frequency: noise would walk both loops away (a swing of half a
        // subcarrier was seen on a real recording), so they hold still until the correlation is back or the receiver has resynchronised.
        const bool pullIn = absSym < 500;
        if (!collapsed) {
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
        if (std::abs(winShift) > 16) { winShift = 0; prevValid = false; }   // a skipped symbol: no valid previous one
        prevWin = s;
        transform(s);
        if (!intLocked) {
            // search needs the unshifted transform: the shifted Y is only a re-indexing of fbuf, which is intact after transform()
            integerSearch(s);
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
            }
        } else tpsBitsSeen.push_back(-1);
        prevTps.resize(tpsK.size());
        for (size_t i = 0; i < tpsK.size(); i++) prevTps[i] = Y[tpsK[i]];
        prevCp.resize(cpK.size());
        for (size_t i = 0; i < cpK.size(); i++) prevCp[i] = Y[cpK[i]];
        prevValid = true;
        if (tpsBitsSeen.size() > 4000) { tpsBitsSeen.erase(tpsBitsSeen.begin(), tpsBitsSeen.begin() + 2000); tpsBase += 2000; }

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
        if (!tpsOk) {
            // channel estimate not meaningful yet: only accumulate the pilot-phase votes and wait for TPS
            return;
        }
        if (phaseCheck && lowRun == 0) {
            // the first good symbol after a resync: whole symbols may have gone missing with the lost samples. Move the frame position on by that
            // many and give the error correction the same number of erased symbols, so that its bit stream stays aligned.
            phaseCheck = false;
            const int delta = scatteredPhaseDelta(symIdx);
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] scattered pilot check at sym %llu: the real stream is %d symbol(s) ahead of the count\n", (unsigned long long)absSym, delta);
            if (delta > 0) {
                if (streaming) for (int i = 0; i < delta; i++) pushErasure((symIdx + i) % 68);
                frameStartAbs -= (uint64_t)delta;
                symIdx = (symIdx + delta) % 68;
            }
        }
        if (lowRun > 0) { if (streaming) pushErasure(symIdx); }   // this symbol's correlation has collapsed: nothing to decode, nothing to learn from
        else estimateAndDecode(symIdx);
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
                tpsOk = true;
                secSinceTps = 0;
                tpsFailures = 0;
                fec.configure(prm);
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
            if (!(q == prm)) { prm = q; fec.configure(prm); }
        } else if (postResync > 0) {
            // After a resync the first frame that comes round holds the gap and is expected to fail. If the second one fails too, whole symbols went
            // missing (more than the scattered-pilot check can tell, which is only the position modulo four): look for the TPS block again at once,
            // keeping the symbol timing, the carrier offset and the channel, instead of waiting for three failures and starting from scratch.
            if (--postResync == 0) { tpsOk = false; tpsFailures = 0; }
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
        const int Nd = dataCarriers(mode);
        std::vector<cf32> eq((size_t)Nd, cf32(0, 0));
        std::vector<float> n0((size_t)Nd, 1e6f);
        fec.pushSymbol(eq.data(), n0.data(), symIdx);
        streamSecs += (double)(N + G) / fn;
        fec.takePackets(pktBuf);
        if (!pktBuf.empty()) {
            packetsOut += pktBuf.size() / 188;
            detect = 3;
            if (cb) cb(pktBuf.data(), pktBuf.size() / 188, streamSecs);
            streamSecs = 0;
            pktBuf.clear();
        }
    }

    void estimateAndDecode(int symIdx) {
        const auto& cp = continualPilots(mode);
        const auto& tpsK = tpsCarriers(mode);
        // ---- per-symbol common phase and timing slope from the continual pilots, relative to their running reference
        std::vector<cf32> c(cp.size());
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
            std::vector<cd> z(cp.size());
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
        // derotate the whole symbol by (phi + slope*(k-kc))
        std::vector<cf32> Yc(K);
        for (int k = 0; k < K; k++) { const double a = -(phi + slope * (k - kc)); Yc[k] = Y[k] * cf32((float)std::cos(a), (float)std::sin(a)); }
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
        // ---- scattered pilots of this symbol refresh one quarter of the grid (carriers 3 * (4j + l mod 4))
        const int l4 = symIdx % 4;
        for (int k = 3 * l4; k < K; k += 12) {
            const int n = k / 3;
            grid[n] = Yc[k] / pilotValue(k);
            gridAge[n] = 0;
        }
        // grid points that coincide with continual pilots get the better (smoothed) estimate
        for (size_t i = 0; i < cp.size(); i++) if (cp[i] % 3 == 0) { grid[cp[i] / 3] = cpRef[i]; gridAge[cp[i] / 3] = 0; }
        for (auto& a : gridAge) if (a < 250) a++;
        gridFilled++;
        if (gridFilled < 4) { if (streaming) pushErasure(symIdx); return; }
        const double tau0 = (double)back + (double)G / 2.0;
        std::vector<cf32> g((size_t)(K + 2) / 3);
        for (size_t n = 0; n < g.size(); n++) g[n] = grid[n];
        interp.run(g, 3, K, N, tau0, H, 1.0);
        // ---- equalise the data carriers
        std::vector<uint8_t> roles;
        carrierRoles(mode, symIdx, roles);
        const int Nd = dataCarriers(mode);
        std::vector<cf32> eq(Nd);
        std::vector<float> n0(Nd);
        int di = 0;
        for (int k = 0; k < K && di < Nd; k++) {
            if (roles[k] != 0) continue;
            const float g2 = std::max(1e-9f, std::norm(H[k]));
            eq[di] = Yc[k] * std::conj(H[k]) / g2;
            n0[di] = (float)(sigma2 / (2.0 * g2));
            di++;
        }
        if (di != Nd) return;
        fec.pushSymbol(eq.data(), n0.data(), symIdx);
        streaming = true;
        streamSecs += (double)(N + G) / fn;
        const FecStats& fs = fec.stats();
        fec.takePackets(pktBuf);
        if (!pktBuf.empty()) {
            packetsOut += pktBuf.size() / 188;
            detect = 3;
            if (cb) cb(pktBuf.data(), pktBuf.size() / 188, streamSecs);
            streamSecs = 0;
            pktBuf.clear();
        } else if (detect.load() < 2) detect = 2;
        (void)fs;
        // ---- display data
        if (symbols % 6 == 0) {
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
            impulseResponse(H, N, irTauMin, G + std::max(8, G / 4), back + back0, irDb);
            double sn = 0;
            for (int k = 0; k < K; k += 7) sn += std::norm(H[k]);
            sn /= (double)((K + 6) / 7);
            snrDb = 10 * std::log10(std::max(1e-9, sn / sigma2));
        }
        lastTpsK = tpsK.size();
    }
    int chDecimV = 1;
    size_t lastTpsK = 0;

    // ------------------------------------------------------------------ telemetry
    void publish() {
        RxTelemetry t;
        t.standard = 1;
        t.rateOk = rateOk;
        t.decimating = decimate;
        t.inputRate = inRate; t.nativeRate = fn;
        t.state = state == 0 ? 0 : tpsOk ? 2 : 1;
        t.fftN = N; t.guard = G; t.giIdx = gi; t.carriers = K;
        t.cfoHz = (epsFrac + intShift) * fn / std::max(1, N);
        t.symbolsPerFrame = 68;
        t.frameMs = N ? 68.0 * (N + G) / fn * 1e3 : 0;
        t.symbols = symbols;
        t.dataValid = tpsOk && gridFilled >= 4;
        t.dataSnrDb = (float)snrDb;
        t.cpSnrDb = (float)snrDb;
        t.eqData = eqShow;
        t.rawCells = rawShow;
        t.eqCells = pilotShow;
        t.p1Const = tpsShow;
        t.chValid = !chMag.empty();
        t.chMagDb = chMag; t.chPhase = chPh; t.chDecim = chDecimV; t.chCarriers = K;
        t.irDb = irDb; t.irTauMin = irTauMin;
        t.secSinceP1 = secSinceTps;
        t.l1preOk = tpsOk; t.l1postOk = tpsOk;
        t.dvbt.tpsOk = tpsOk; t.dvbt.mode = mode; t.dvbt.guard = gi;
        if (tpsOk) { t.dvbt.mod = prm.mod; t.dvbt.hier = prm.hier; t.dvbt.crHp = prm.crHp; t.dvbt.crLp = prm.crLp; t.dvbt.cellId = prm.cellId; t.dvbt.frameIdx = frameIdx; }
        const FecStats& fs = fec.stats();
        t.dvbt.fecSync = fs.syncLocked; t.dvbt.packets = fs.packets; t.dvbt.rsClean = fs.rsClean; t.dvbt.rsCorrected = fs.rsCorrected; t.dvbt.rsFailed = fs.rsFailed;
        t.dvbt.viterbiMargin = fs.viterbiMargin; t.dvbt.punctPhase = fs.punctPhase; t.dvbt.secSinceTps = secSinceTps;
        t.dvbt.symbolIdx = tpsOk ? (int)((absSym - frameStartAbs) % 68) : -1;
        t.blocksOk = fs.rsClean + fs.rsCorrected; t.blocksBad = fs.rsFailed;
        t.plpValid = tpsOk; t.plpFrames = fs.packets;
        t.plpMerDb = snrDb;
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++seq;
        tel = std::move(t);
    }
};

DvbtReceiver::DvbtReceiver() : p_(new Impl) {}
DvbtReceiver::~DvbtReceiver() = default;

void DvbtReceiver::configure(double inputRateHz, double bandwidthMhz) {
    Impl& I = *p_;
    I.reset();
    I.inRate = inputRateHz; I.bwMhz = bandwidthMhz;
    I.fn = nativeRateHz(bandwidthMhz);
    I.rateOk = I.resampler.configure(inputRateHz, I.fn) && inputRateHz >= 7.9e6 * (bandwidthMhz / 8.0);
    I.decimate = I.rateOk && !I.resampler.passthrough();
}

void DvbtReceiver::reset() { Impl& I = *p_; const double r = I.inRate, b = I.bwMhz; configure(r, b); }

void DvbtReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->cb = std::move(cb); }
int DvbtReceiver::detectLevel() const { return p_->detect.load(); }

void DvbtReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rateOk || !n) return;
    // DC removal (the radio's centre spike, a HackRF's is several times a carrier): the mean of the block moves a slow estimate. Left in,
    // it lands on the centre carrier, which is a continual pilot: that pilot's error then poisons the common phase, the noise estimate
    // and, through the pilot grid, the channel estimate of every carrier, and nothing decodes. The time constant is long (50 ms) so that
    // the centre pilot itself, which turns at the carrier offset, is not taken out with it unless that offset is within a few hertz.
    {
        float sr4[4] = {0, 0, 0, 0}, si4[4] = {0, 0, 0, 0};
        size_t k = 0;
        for (; k + 4 <= n; k += 4) for (size_t l = 0; l < 4; l++) { sr4[l] += x[k + l].real(); si4[l] += x[k + l].imag(); }
        for (; k < n; k++) { sr4[0] += x[k].real(); si4[0] += x[k].imag(); }
        const float sr = (sr4[0] + sr4[1]) + (sr4[2] + sr4[3]), si = (si4[0] + si4[1]) + (si4[2] + si4[3]);
        const float w = (float)std::min(1.0, (double)n / (0.05 * I.inRate));
        I.dc += (cf32(sr, si) / (float)n - I.dc) * w;
        I.dcOut.resize(n);
        const float dr = I.dc.real(), di = I.dc.imag();
        for (size_t q = 0; q < n; q++) I.dcOut[q] = cf32(x[q].real() - dr, x[q].imag() - di);
        x = I.dcOut.data();
    }
    I.stage.clear();
    if (I.decimate) I.resampler.process(x, n, I.stage);
    else I.stage.assign(x, x + n);
    I.clk.process(I.stage);
    I.buf.insert(I.buf.end(), I.stage.begin(), I.stage.end());
    // work through the buffer
    for (int guard = 0; guard < 100000; guard++) {
        if (I.state == 0) {
            const size_t need = (size_t)(6.0 * (8192 + 2048) + 8192 + 2048);
            if (I.buf.size() < need) break;
            // acquisition every ~2 symbols of the longest mode; keep a sliding window
            if (!I.acquire()) {
                const size_t drop = I.buf.size() - need / 2;
                I.buf.erase(I.buf.begin(), I.buf.begin() + drop);
                I.base += (int64_t)drop;
                break;
            }
        } else {
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
