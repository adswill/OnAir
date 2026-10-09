#include "dect2/isdbt_rx.h"
#include "dect2/isdbt_resample.h"
#include "dect2/fftutil.h"
#include "dect2/isdbt.h"
#include "dect2/isdbt_demod.h"
#include "dect2/t2ofdm.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <mutex>
#include <complex>

namespace dect2 {

using namespace isdbt;
using cd = std::complex<double>;

namespace {
std::vector<int> tmccCarriers(int mode, int pos, bool diff) { return tmccCarrierList(mode, pos, diff); }
}

struct IsdbtReceiver::Impl {
    // ---- input
    double inRate = 0;
    TrackingResampler resampler;
    bool decimate = false, rateOk = true;
    std::vector<cf32> rsOut, buf;
    int64_t base = 0;
    int64_t end() const { return base + (int64_t)buf.size(); }
    const cf32& at(int64_t i) const {
        static const cf32 kZero(0.f, 0.f);
        const int64_t k = i - base;
        return (k >= 0 && k < (int64_t)buf.size()) ? buf[(size_t)k] : kZero;
    }

    // ---- where the channel sits in the input band. The receiver first assumes the centre; the power spectrum of the first samples gives a
    // second guess (a recording made beside the channel), and the receiver goes through the guesses whenever the TMCC cannot be found. Each
    // guess is also tried with I and Q swapped (a mirrored spectrum, from some radios and file formats).
    struct Guess { double hz; bool mirror; };
    std::vector<Guess> mixCand{{0.0, false}, {0.0, true}};
    bool mirror = false;
    size_t candIdx = 0;
    double mixHz = 0;
    cd mixRot{1, 0}, mixStep{1, 0};
    uint64_t mixN = 0;
    bool estDone = false;
    std::vector<cf32> estBuf;
    std::vector<double> estPow;
    int estBlocks = 0, estTries = 0;
    int64_t searchedSince = 0;                 // input samples since the current guess was taken (without a lock)
    std::vector<cf32> pre, clean;

    // ---- state
    int state = 0;                 // 0 searching, 1 tracking and looking for the TMCC, 2 locked
    int modeIdx = -1, gi = -1, mode = 0, N = 0, G = 0, K = 0, kc = 0;   // mode: 1, 2, 3
    double symStart = 0, timingAcc = 0, epsFrac = 0;
    int64_t cumStep = 0;                // whole samples the symbol position was moved by the timing loop
    uint64_t clockMarkSym = 0; double clockMarkTau = 0, sroPpm = 0;
    double cfoPhase = 0; int64_t cfoPhaseW = 0; bool cfoPhaseValid = false;
    int intShift = 0;
    int back = 0;
    std::unique_ptr<Fft> fft;
    std::vector<cf32> fbuf;
    std::atomic<int> detect{0};
    int agreeCount = 0, agreeMode = -1, agreeGi = -1;
    uint64_t absSym = 0, symbols = 0;

    // ---- TMCC search and lock
    int R = 12;                                    // carriers searched on both sides
    std::vector<std::vector<cf32>> spec;           // spectra of consecutive symbols, window of K + 2R bins
    uint64_t specBase = 0;
    int huntFails = 0, sinceHunt = 0;
    bool tmccOk = false;
    Params prm;
    Tmcc tmcc;
    int tmccSeg = -1; bool tmccDiff = false; std::vector<int> tmccK;
    uint64_t frameStartAbs = 0;
    std::vector<cf32> prevTmcc;                    // TMCC carriers of the previous symbol
    std::vector<float> tmccSoft;                   // soft bit values of the symbols of the current frame
    std::vector<cf32> tmccShow;                    // differentially detected TMCC carriers of a recent symbol, for the display
    int tmccFailures = 0;
    double secSinceTmcc = 0;
    Demod demod;
    std::vector<cf32> Y;
    std::vector<uint8_t> pktBuf;
    double streamSecs = 0;
    uint64_t packetsOut = 0;
    std::function<void(const uint8_t*, size_t, double)> cb;

    // ---- telemetry
    std::mutex mu;
    RxTelemetry tel;
    uint64_t seq = 0;

    void reset() {
        state = 0; modeIdx = -1; gi = -1; N = G = K = kc = mode = 0; symStart = 0; timingAcc = 0; epsFrac = 0; intShift = 0;
        fft.reset(); agreeCount = 0; agreeMode = agreeGi = -1; absSym = 0; symbols = 0;
        spec.clear(); specBase = 0; huntFails = 0; sinceHunt = 0; tmccOk = false; tmccSeg = -1; prevTmcc.clear(); tmccSoft.clear(); tmccFailures = 0; secSinceTmcc = 0;
        streamSecs = 0; packetsOut = 0; detect = 0; sroPpm = 0;
        buf.clear(); base = 0; resampler.reset();
        mixCand = {{0.0, false}, {0.0, true}}; candIdx = 0; mirror = false; setMix(0); estDone = false; estBuf.clear(); estPow.clear(); estBlocks = 0; estTries = 0; searchedSince = 0;
    }

    // ------------------------------------------------------------------ the channel's place in the input band
    void setMix(double hz) {
        mixHz = hz; mixN = 0; mixRot = cd(1, 0);
        const double w = -2.0 * M_PI * hz / std::max(1.0, inRate);
        mixStep = cd(std::cos(w), std::sin(w));
    }
    // a new guess: everything received so far was mixed with the old one
    void takeCandidate(size_t i) {
        candIdx = i % mixCand.size();
        setMix(mixCand[candIdx].hz); mirror = mixCand[candIdx].mirror;
        state = 0; agreeCount = 0; detect = 0; tmccOk = false; symStart = 0;
        buf.clear(); base = 0; resampler.reset(); spec.clear(); searchedSince = 0;
        if (getenv("ISDBT_DEBUG")) fprintf(stderr, "[isdbt] channel guess %zu: %.1f kHz%s\n", candIdx, mixHz / 1e3, mirror ? ", mirrored" : "");
    }
    void nextCandidate() { if (mixCand.size() > 1) takeCandidate(candIdx + 1); else searchedSince = 0; }

    // The ISDB-T signal is a flat block of 13 segments (5.57 MHz): the box of that width with the most power in the averaged spectrum, if
    // its edges stand clearly above what lies outside.
    void estimateCentre(const cf32* x, size_t n) {
        constexpr int F = 1024, kBlocks = 48;
        estBuf.insert(estBuf.end(), x, x + std::min(n, (size_t)F * kBlocks));
        if (estPow.empty()) estPow.assign(F, 0.0);
        Fft f(F);
        size_t used = 0;
        while (estBuf.size() - used >= (size_t)F && estBlocks < kBlocks) {
            std::vector<cf32> b(estBuf.begin() + (long)used, estBuf.begin() + (long)used + F);
            for (int i = 0; i < F; i++) {
                if (!std::isfinite(b[(size_t)i].real()) || !std::isfinite(b[(size_t)i].imag())) b[(size_t)i] = cf32(0, 0);
                b[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / F));
            }
            f.forward(b.data());
            for (int i = 0; i < F; i++) estPow[(size_t)((i + F / 2) % F)] += std::norm(b[(size_t)i]);   // centred: bin F / 2 is 0 Hz
            used += F; estBlocks++;
        }
        estBuf.erase(estBuf.begin(), estBuf.begin() + (long)used);
        if (estBlocks < kBlocks) return;
        estDone = true; estBuf.clear(); estBuf.shrink_to_fit();
        const int W = (int)std::lround(5.572e6 / inRate * F);
        if (W < 16 || W > F - 2) return;
        std::vector<double> cum(F + 1, 0.0);
        for (int i = 0; i < F; i++) cum[(size_t)i + 1] = cum[(size_t)i] + estPow[(size_t)i];
        int best = -1; double bs = -1;
        for (int a = 0; a + W <= F; a++) { const double v = cum[(size_t)(a + W)] - cum[(size_t)a]; if (v > bs) { bs = v; best = a; } }
        if (best < 0 || bs <= 0) return;
        const double inside = bs / W;
        const int e = std::max(2, W / 25);
        auto edgeOk = [&](int a0, int a1) {          // the band beside an edge: much weaker, or outside the input band
            a0 = std::max(0, a0); a1 = std::min(F, a1);
            if (a1 - a0 < 2) return true;
            return (cum[(size_t)a1] - cum[(size_t)a0]) / (a1 - a0) < 0.25 * inside;
        };
        if (!edgeOk(best - e - 2, best - 2) || !edgeOk(best + W + 2, best + W + e + 2)) {
            // no clear channel yet (silence or noise at the start of a recording): look again at later samples, a few times
            if (++estTries < 16) { estDone = false; estPow.assign(F, 0.0); estBlocks = 0; }
            return;
        }
        const double hz = ((double)best + W / 2.0 - F / 2.0) * inRate / F;
        if (getenv("ISDBT_DEBUG")) fprintf(stderr, "[isdbt] spectrum: channel centre %.1f kHz\n", hz / 1e3);
        if (std::fabs(hz) < 40e3) return;            // within the receiver's own frequency search
        // with I and Q swapped the channel lies on the other side
        mixCand = {{hz, false}, {0.0, false}, {-hz, true}, {0.0, true}};
        if (!tmccOk) takeCandidate(0); else candIdx = mirror ? 3 : 1;   // still searching: the spectrum's guess first
    }

    // input conditioning: the guess of the channel's place, then the resampler
    void condition(const cf32* x, size_t n) {
        const cf32* src = x;
        // NaN or infinite samples (a broken file or driver) would poison the filters and the loops for good: they become silence
        bool bad = false;
        for (size_t i = 0; i < n && !bad; i++) bad = !std::isfinite(x[i].real()) || !std::isfinite(x[i].imag());
        if (bad) {
            clean.assign(x, x + n);
            for (auto& v : clean) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            x = src = clean.data();
        }
        if (mirror) {
            pre.resize(n);
            for (size_t i = 0; i < n; i++) pre[i] = cf32(x[i].imag(), x[i].real());
            x = src = pre.data();
        }
        if (mixHz != 0) {
            pre.resize(n);
            for (size_t i = 0; i < n; i++) {
                const cd v = cd(x[i].real(), x[i].imag()) * mixRot;
                pre[i] = cf32((float)v.real(), (float)v.imag());
                mixRot *= mixStep;
                if ((++mixN & 1023) == 0) mixRot /= std::abs(mixRot);
            }
            src = pre.data();
        }
        rsOut.clear(); resampler.process(src, n, rsOut); buf.insert(buf.end(), rsOut.begin(), rsOut.end());
    }

    // ------------------------------------------------------------------ acquisition: mode and guard interval from the cyclic prefix
    bool acquire() {
        struct Cand { double score; int m, g; int64_t pos; cd corr; };
        Cand best{0, -1, -1, 0, cd(0, 0)};
        const int64_t b0 = base;
        for (int m = 1; m <= 3; m++) {
            const int n = isdbt::fftN(m);
            const int64_t avail = (int64_t)buf.size();
            if (avail < (int64_t)(6.0 * (n + n / 4) + n + n / 4)) continue;
            const int64_t B = std::min<int64_t>(avail, (int64_t)(8 * (n + n / 4) + n));
            const int64_t Dn = B - n;
            std::vector<cd> cum((size_t)Dn + 1);
            std::vector<double> en((size_t)Dn + 1);
            cd acc = 0; double ea = 0;
            cum[0] = 0; en[0] = 0;
            for (int64_t i = 0; i < Dn; i++) {
                const cf32 a = buf[buf.size() - (size_t)B + (size_t)i], b = buf[buf.size() - (size_t)B + (size_t)i + (size_t)n];
                acc += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
                ea += 0.5 * (std::norm(a) + std::norm(b));
                cum[(size_t)i + 1] = acc; en[(size_t)i + 1] = ea;
            }
            for (int g = 0; g < 4; g++) {
                const int G_ = isdbt::guardSamples(m, g), P = n + G_;
                const int64_t D = Dn - G_;
                if (D < (int64_t)P * 3) continue;
                std::vector<double> F((size_t)P, 0.0);
                std::vector<cd> Fc((size_t)P, cd(0, 0));
                std::vector<int> cnt((size_t)P, 0);
                for (int64_t d = 0; d < D; d++) {
                    const cd c = cum[(size_t)(d + G_)] - cum[(size_t)d];
                    const double e = en[(size_t)(d + G_)] - en[(size_t)d];
                    const double r = e > 1e-12 ? std::abs(c) / e : 0.0;
                    F[(size_t)(d % P)] += r; Fc[(size_t)(d % P)] += c; cnt[(size_t)(d % P)]++;
                }
                double mx = 0, mean = 0; int arg = 0;
                for (int i = 0; i < P; i++) { if (cnt[(size_t)i]) F[(size_t)i] /= cnt[(size_t)i]; mean += F[(size_t)i]; if (F[(size_t)i] > mx) { mx = F[(size_t)i]; arg = i; } }
                mean /= P;
                const double ratio = mean > 0 ? mx / mean : 0;
                const double score = mx * std::min(ratio, 12.0) / 12.0;
                if (mx > 0.12 && ratio > 3.0 && score > best.score) {
                    best.score = score; best.m = m; best.g = g;
                    const int64_t first = b0 + (int64_t)buf.size() - B;
                    best.pos = first + arg;
                    best.corr = Fc[(size_t)arg] / (double)std::max(1, cnt[(size_t)arg]);
                }
            }
        }
        if (best.m < 0) { agreeCount = 0; return false; }
        detect = std::max(detect.load(), 1);
        if (best.m == agreeMode && best.g == agreeGi) agreeCount++; else { agreeMode = best.m; agreeGi = best.g; agreeCount = 1; }
        if (agreeCount < 2) return false;
        mode = best.m; gi = best.g; N = isdbt::fftN(mode); G = isdbt::guardSamples(mode, gi); K = totalCarriers(mode); kc = centerCarrier(mode);
        const int P = N + G;
        int64_t s = best.pos;
        while (s < base + (int64_t)buf.size() - (int64_t)(6 * P)) s += P;
        while (s - P >= base + 16) s -= P;
        symStart = (double)s;
        timingAcc = 0; cumStep = 0; clockMarkSym = 0; cfoPhase = 0; cfoPhaseValid = false;
        epsFrac = -std::arg(best.corr) / (2 * M_PI);
        intShift = 0;
        back = std::min(G / 4, 24);
        fft = std::make_unique<Fft>(N);
        fbuf.assign((size_t)N, cf32(0, 0));
        const double spacing = kSampleRate / N;
        R = std::max(8, std::min(80, (int)std::ceil(60e3 / spacing)));
        spec.clear(); specBase = 0; huntFails = 0; sinceHunt = 0; tmccOk = false; prevTmcc.clear(); tmccSoft.clear(); tmccFailures = 0;
        absSym = 0;
        state = 1;
        return true;
    }

    // ------------------------------------------------------------------ one symbol
    void transform(int64_t s) {
        const int64_t w = s + G - back;
        // once the whole-carrier part of the offset is known it is removed here as well: it makes the carriers turn by 2 pi * shift * G / N
        // from one symbol to the next, which the differential detection must not see
        const double eps = epsFrac + (state == 2 ? (double)intShift : 0.0);
        // the phase of the derotation is accumulated from symbol to symbol: a change of the estimate then only changes the rate, it does not
        // make the phase of everything that follows jump by 2 pi * change * (time since the start)
        if (cfoPhaseValid) cfoPhase -= 2.0 * M_PI * eps * (double)(w - cfoPhaseW) / N;
        cfoPhaseW = w; cfoPhaseValid = true;
        cfoPhase = std::fmod(cfoPhase, 2.0 * M_PI);
        const double ph0 = cfoPhase;
        cd cur(std::cos(ph0), std::sin(ph0));
        const cd rot(std::cos(-2.0 * M_PI * eps / N), std::sin(-2.0 * M_PI * eps / N));
        for (int i = 0; i < N; i++) {
            const cf32 v = at(w + i);
            const cd o = cd(v.real(), v.imag()) * cur;
            fbuf[(size_t)i] = cf32((float)o.real(), (float)o.imag());
            cur *= rot;
            if ((i & 255) == 255) cur /= std::abs(cur);
        }
        fft->forward(fbuf.data());
        // Every whole-sample step of the timing loop moved the window and gave the carriers a phase ramp of 2 pi k / N; take the steps back out so
        // that the carriers stay comparable from one symbol to the next (the slope that remains is the clock drift, which the demodulator follows)
        if (cumStep != 0 && !getenv("ISDBT_NOCOMP")) {
            const double stepPh = 2.0 * M_PI * (double)(cumStep % N) / N;
            const cd rot2(std::cos(stepPh), std::sin(stepPh));
            cd c2(1, 0);
            for (int i = 0; i < N / 2; i++) { fbuf[(size_t)i] *= cf32((float)c2.real(), (float)c2.imag()); c2 *= rot2; if ((i & 255) == 255) c2 /= std::abs(c2); }
            cd c3 = std::polar(1.0, -stepPh * (N / 2));
            for (int i = N / 2; i < N; i++) { fbuf[(size_t)i] *= cf32((float)c3.real(), (float)c3.imag()); c3 *= rot2; if ((i & 255) == 255) c3 /= std::abs(c3); }
        }
    }

    void cpTrack(int64_t s, double& err, double& epsRaw, double& mag) {
        double bestM = -1; int bestD = 0; cd bestC = 0;
        double ms[13];
        for (int d = -6; d <= 6; d++) {
            cd c = 0;
            for (int i = 0; i < G; i++) { const cf32 a = at(s + d + i), b = at(s + d + i + N); c += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag())); }
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
    }
    static double wrap(double v) { while (v > 0.5) v -= 1; while (v < -0.5) v += 1; return v; }

    // The radio's sample clock is a few ppm off: the symbols then drift against the window. The drift of the timing loop over a few hundred
    // symbols gives the error, and the resampler's ratio follows it (otherwise the carriers turn into each other at the band edges).
    void trackClock() {
        if (decimate == false && !rateOk) return;
        const double tau = (double)cumStep + timingAcc;
        if (clockMarkSym == 0 || symbols < clockMarkSym) { clockMarkSym = symbols + 1; clockMarkTau = tau; return; }
        const uint64_t span = symbols - (clockMarkSym - 1);
        if (span < 200) return;
        // the timing loop wanders a few samples on its own (echoes, noise): only a drift well beyond that counts, however long it takes
        if (std::fabs(tau - clockMarkTau) < 8.0) return;
        const double eps = (tau - clockMarkTau) / ((double)span * (double)(N + G));   // window moved later by eps of the symbol length per symbol
        const double adj = std::max(-2e-4, std::min(2e-4, eps)) * 0.8;
        resampler.scaleStep(1.0 + adj);
        sroPpm += adj * 1e6;
        if (getenv("ISDBT_DEBUG")) fprintf(stderr, "[isdbt] clock: drift %.1f ppm, resampler adjusted, total %.1f ppm\n", eps * 1e6, sroPpm);
        clockMarkSym = symbols + 1; clockMarkTau = tau;
    }

    bool step() {
        const int P = N + G;
        int64_t s = (int64_t)std::llround(symStart);
        if (s + P + N + 32 > end()) return false;
        if (s - 16 < base) { symStart += P; return true; }
        double terr, eraw, mag;
        cpTrack(s, terr, eraw, mag);
        timingAcc += 0.08 * terr;
        if (std::fabs(timingAcc) >= 1.0) { const double stp = std::round(timingAcc); symStart += stp; timingAcc -= stp; cumStep += (int64_t)stp; }
        trackClock();
        const double d = wrap(eraw - epsFrac);
        epsFrac += 0.15 * d;
        if (epsFrac > 0.5 || epsFrac < -0.5) {
            const double stp = epsFrac > 0 ? 1.0 : -1.0;
            epsFrac -= stp;
            if (state == 2) intShift += (int)stp;
        }
        transform(s);
        symStart += P;
        if (state == 1) { huntStore(); absSym++; }
        else { processLocked(); absSym++; }
        symbols++;
        return true;
    }

    // ------------------------------------------------------------------ TMCC search
    void huntStore() {
        std::vector<cf32> w((size_t)(K + 2 * R));
        // window bin j holds carrier k with the shift s: j = k + s + R, the FFT bin is k + s - kc
        for (int j = 0; j < K + 2 * R; j++) w[(size_t)j] = fbuf[(size_t)((((j - R - kc) % N) + N) % N)];
        if (spec.empty()) specBase = absSym;
        spec.push_back(std::move(w));
        sinceHunt++;
        if ((int)spec.size() >= 2 * kSymbolsPerFrame + 16 && sinceHunt >= 100) {
            sinceHunt = 0;
            if (hunt()) { state = 2; spec.clear(); spec.shrink_to_fit(); }
            else {
                if (++huntFails >= 8) { state = 0; detect = 1; agreeCount = 0; spec.clear(); nextCandidate(); return; }
                const size_t drop = 100;
                spec.erase(spec.begin(), spec.begin() + (long)drop);
                specBase += drop;
            }
        }
    }

    bool hunt() {
        const int H = (int)spec.size();
        const int cps = carriersPerSegment(mode);
        (void)cps;
        std::vector<float> b((size_t)H);
        for (int s = -R; s <= R; s++) {
            // a shift of s carriers turns every carrier by 2 pi s G / N between symbols: take it out of the products
            const double th = 2.0 * M_PI * (double)s * (double)G / (double)N;
            const cf32 unturn((float)std::cos(th), (float)-std::sin(th));
            for (int pos = 0; pos < kSegments; pos++) {
                for (int type = 0; type < 2; type++) {
                    const bool diff = type == 0;
                    const auto ks = tmccCarriers(mode, pos, diff);
                    // soft bit values: a flip of the carrier's sign between consecutive symbols is a 1
                    b[0] = 0;
                    for (int n = 1; n < H; n++) {
                        double num = 0, den = 0;
                        for (int k : ks) {
                            const cf32 a = spec[(size_t)n][(size_t)(k + s + R)], c = spec[(size_t)(n - 1)][(size_t)(k + s + R)];
                            num += (a * std::conj(c) * unturn).real();
                            den += std::abs(a) * std::abs(c);
                        }
                        b[(size_t)n] = den > 0 ? (float)(-num / den) : 0.f;
                    }
                    if (tryDecode(b, H, s, pos, diff)) return true;
                }
            }
        }
        return false;
    }

    bool allSegmentsAgree(int s, int f, const uint8_t* info, bool even, const Params& q) {
        SegmentInfo seg[kSegments];
        if (!segmentLayout(q, seg)) return false;
        const double th = 2.0 * M_PI * (double)s * (double)G / (double)N;
        const cf32 unturn((float)std::cos(th), (float)-std::sin(th));
        uint8_t exp[2][kSymbolsPerFrame];
        tmccFrameBits(info, even, true, exp[0]);
        tmccFrameBits(info, even, false, exp[1]);
        // one pooled measure over the TMCC carriers of all segments: a carrier in a deep fade adds nothing and costs little, where a vote per segment
        // would lose the segments whose few carriers all fade together
        int used = 0;
        double c = 0, mag = 0;
        for (int pos = 0; pos < kSegments; pos++) {
            const int sn = kSegmentAtPosition[pos];
            if (seg[sn].layer < 0) continue;
            used++;
            const auto ks = tmccCarriers(mode, pos, seg[sn].diff);
            const uint8_t* e = exp[seg[sn].diff ? 0 : 1];
            for (int i = 1; i < kSymbolsPerFrame; i++) {
                const int n = f + i;
                if (n >= (int)spec.size()) break;
                for (int k : ks) {
                    const cf32 a = spec[(size_t)n][(size_t)(k + s + R)], cc = spec[(size_t)(n - 1)][(size_t)(k + s + R)];
                    const double num = (a * std::conj(cc) * unturn).real();
                    c += -num * (e[i] ? 1.0 : -1.0);
                    mag += std::abs(a) * std::abs(cc);
                }
            }
        }
        return used > 0 && mag > 0 && c / mag > 0.3;
    }

    bool tryDecode(const std::vector<float>& b, int H, int s, int pos, bool diff) {
        for (int f = 0; f + kSymbolsPerFrame <= H; f++) {
            if (f >= kSymbolsPerFrame) break;
            // synchronising word and segment type
            double c = 0, mag = 0;
            for (int i = 0; i < 16; i++) { c += b[(size_t)(f + 1 + i)] * (kTmccSync0[i] ? 1.0 : -1.0); mag += std::fabs(b[(size_t)(f + 1 + i)]); }
            if (mag < 1e-6 || std::fabs(c) / mag < 0.55) continue;
            const bool even = c > 0;
            double ty = 0;
            for (int i = 17; i <= 19; i++) ty += b[(size_t)(f + i)];
            if ((ty > 0) != diff) continue;
            uint8_t w[184]; float rel[184];
            for (int i = 0; i < 184; i++) { const float v = b[(size_t)(f + 20 + i)]; w[i] = v > 0 ? 1 : 0; rel[i] = std::fabs(v); }
            if (!tmccCorrect(w, rel)) continue;
            Tmcc t;
            tmccUnpack(w, t);
            Params q; q.mode = mode; q.guard = gi;
            if (!paramsFromTmcc(t, q)) continue;
            // every segment carries the same TMCC data: with the right shift the others agree as well (a wrong shift can land one carrier on a
            // TMCC carrier of another segment and look right on its own)
            uint8_t info[kTmccInfoBits];
            std::memcpy(info, w, kTmccInfoBits);
            if (!allSegmentsAgree(s, f, info, even, q)) continue;
            // found
            intShift = s; tmccSeg = kSegmentAtPosition[pos]; tmccDiff = diff; tmccK = tmccCarriers(mode, pos, diff);
            tmcc = t; prm = q; tmccOk = true;
            frameStartAbs = specBase + (uint64_t)f;
            tmccFailures = 0; secSinceTmcc = 0;
            demod.configure(prm);
            demod.setDelayCentre((double)back + G / 2.0);
            demod.setTmccInfo(info);
            prevTmcc.clear(); tmccSoft.clear();
            detect = 2;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ locked
    void processLocked() {
        Y.resize((size_t)K);
        for (int k = 0; k < K; k++) Y[(size_t)k] = fbuf[(size_t)((((k - kc) % N) + N) % N)];
        const int symIdx = (int)((absSym - frameStartAbs) % kSymbolsPerFrame);
        secSinceTmcc += (double)(N + G) / kSampleRate;
        demod.pushSymbol(Y.data(), symIdx);
        // TMCC bit of this symbol, from the corrected carriers
        const std::vector<cf32>& Yc = demod.corrected();
        float soft = 0;
        if (!prevTmcc.empty() && (int)Yc.size() == K) {
            double num = 0, den = 0;
            for (size_t j = 0; j < tmccK.size(); j++) { const cf32 a = Yc[(size_t)tmccK[j]], c = prevTmcc[j]; num += (a * std::conj(c)).real(); den += std::abs(a) * std::abs(c); }
            soft = den > 0 ? (float)(-num / den) : 0.f;
        }
        if (symbols % 3 == 0 && !prevTmcc.empty() && (int)Yc.size() == K) {
            tmccShow.clear();
            for (size_t j = 0; j < tmccK.size() && j < prevTmcc.size(); j++) { const cf32 z = Yc[(size_t)tmccK[j]] * std::conj(prevTmcc[j]); const float m = std::abs(z); if (m > 1e-9f) tmccShow.push_back(z / m); }
        }
        prevTmcc.resize(tmccK.size());
        if ((int)Yc.size() == K) for (size_t j = 0; j < tmccK.size(); j++) prevTmcc[j] = Yc[(size_t)tmccK[j]];
        if (symIdx == 0) tmccSoft.clear();
        tmccSoft.push_back(soft);
        streamSecs += (double)(N + G) / kSampleRate;
        pktBuf.clear();
        const size_t n = demod.takeMerged(pktBuf);
        if (n) { packetsOut += n; detect = 3; if (cb) cb(pktBuf.data(), n, streamSecs); streamSecs = 0; }
        if (symIdx == kSymbolsPerFrame - 1) verifyFrame();
    }

    void verifyFrame() {
        if ((int)tmccSoft.size() != kSymbolsPerFrame) return;
        const auto& b = tmccSoft;
        double c = 0, mag = 0;
        for (int i = 0; i < 16; i++) { c += b[(size_t)(1 + i)] * (kTmccSync0[i] ? 1.0 : -1.0); mag += std::fabs(b[(size_t)(1 + i)]); }
        bool ok = false;
        if (mag > 1e-6 && std::fabs(c) / mag > 0.4) {
            uint8_t w[184]; float rel[184];
            for (int i = 0; i < 184; i++) { const float v = b[(size_t)(20 + i)]; w[i] = v > 0 ? 1 : 0; rel[i] = std::fabs(v); }
            if (getenv("ISDBT_DEBUG2")) {
                uint8_t info[kTmccInfoBits]; tmccPack(tmcc, info);
                uint8_t ex[kSymbolsPerFrame]; tmccFrameBits(info, c > 0, tmccDiff, ex);
                int err = 0; std::string pos;
                for (int i = 0; i < 184; i++) if (w[i] != ex[20 + i]) { err++; char tmp[16]; snprintf(tmp, sizeof tmp, " %d(%.2f)", 20 + i, b[(size_t)(20 + i)]); pos += tmp; }
                fprintf(stderr, "[isdbt] raw bit errors %d:%s\n", err, pos.c_str());
            }
            if (tmccCorrect(w, rel)) {
                Tmcc t; tmccUnpack(w, t);
                Params q; q.mode = mode; q.guard = gi;
                if (paramsFromTmcc(t, q)) {
                    ok = true;
                    tmcc = t;
                    if (!(q == prm)) { prm = q; demod.configure(prm); demod.setDelayCentre((double)back + G / 2.0); }
                    // the word can change without a change of the parameters (the switching countdown, the next parameters): the demodulator
                    // uses its TMCC bits as pilots
                    uint8_t inf2[kTmccInfoBits]; std::memcpy(inf2, w, kTmccInfoBits); demod.setTmccInfo(inf2);
                }
            }
        }
        if (getenv("ISDBT_DEBUG")) fprintf(stderr, "[isdbt] frame end abs %llu: tmcc %s (sync corr %.2f) eps %.3f shift %d\n", (unsigned long long)absSym, ok ? "ok" : "FAILED", mag > 0 ? c / mag : 0.0, epsFrac, intShift);
        if (ok) { tmccFailures = 0; secSinceTmcc = 0; }
        else if (++tmccFailures >= 4) { state = 0; detect = 1; agreeCount = 0; tmccOk = false; if (getenv("ISDBT_DEBUG")) fprintf(stderr, "[isdbt] lost\n"); }
    }

    // ------------------------------------------------------------------ telemetry
    void publish() {
        RxTelemetry t;
        t.standard = 5;
        t.rateOk = rateOk;
        t.decimating = decimate;
        t.inputRate = inRate; t.nativeRate = kSampleRate;
        t.state = state == 0 ? 0 : tmccOk ? 2 : 1;
        t.fftN = N; t.guard = G; t.giIdx = gi; t.carriers = K;
        t.cfoHz = (epsFrac + intShift) * kSampleRate / std::max(1, N) + mixHz;
        t.sroPpm = sroPpm;
        t.symbolsPerFrame = kSymbolsPerFrame;
        t.frameMs = N ? (double)kSymbolsPerFrame * (N + G) / kSampleRate * 1e3 : 0;
        t.symbols = symbols;
        t.dataValid = tmccOk;
        t.dataSnrDb = (float)demod.snrDb();
        t.cpSnrDb = t.dataSnrDb;
        t.eqData = demod.eqCells();
        t.eqCells = demod.pilotCells();
        t.p1Const = tmccShow;
        if (state == 2) {
            std::vector<cf32> Hf;
            demod.channel(Hf);
            const int dec = std::max(1, K / 2048);
            t.chMagDb.clear(); t.chPhase.clear();
            bool any = false;
            for (int k = 0; k < K; k += dec) { const float m = std::abs(Hf[(size_t)k]); any |= m > 0; t.chMagDb.push_back(20.f * std::log10(std::max(1e-6f, m))); t.chPhase.push_back(std::arg(Hf[(size_t)k])); }
            t.chValid = any; t.chDecim = dec; t.chCarriers = K;
            if (any) { t.irTauMin = -N / 16; impulseResponse(Hf, N, t.irTauMin, G + std::max(8, G / 4), back + G / 8, t.irDb); }
            const std::vector<cf32>& Yc = demod.corrected();
            t.rawCells.clear();
            if ((int)Yc.size() == K) {
                const int st2 = std::max(1, K / 1500);
                for (int k = 0; k < K; k += st2) { const float m = std::abs(Hf[(size_t)k]); if (m > 1e-6f) t.rawCells.push_back(Yc[(size_t)k] / m); }
            }
        }
        t.secSinceP1 = secSinceTmcc;
        t.l1preOk = tmccOk; t.l1postOk = tmccOk;
        t.isdbt.tmccOk = tmccOk; t.isdbt.mode = mode; t.isdbt.guard = gi; t.isdbt.intShift = intShift; t.isdbt.secSinceTmcc = secSinceTmcc;
        t.isdbt.symbolIdx = tmccOk ? (int)((absSym - frameStartAbs) % kSymbolsPerFrame) : -1;
        uint64_t ok = 0, bad = 0;
        if (tmccOk) {
            t.isdbt.partial = prm.partial; t.isdbt.switching = tmcc.switching; t.isdbt.emergency = tmcc.emergency;
            for (int i = 0; i < 3; i++) {
                auto& L = t.isdbt.layer[i];
                const Layer& l = prm.layer[i];
                L.segments = l.segments; L.mod = l.mod; L.rate = l.rate; L.ti = l.ti;
                const LayerStats& st = demod.layerStats(i);
                L.packets = st.packets; L.rsClean = st.rsClean; L.rsCorrected = st.rsCorrected; L.rsFailed = st.rsFailed; L.viterbi = st.viterbiMargin; L.synced = st.synced;
                ok += st.rsClean + st.rsCorrected; bad += st.rsFailed;
            }
        }
        t.blocksOk = ok; t.blocksBad = bad;
        t.plpValid = tmccOk; t.plpFrames = ok + bad;
        t.plpMerDb = demod.snrDb();
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++seq;
        tel = std::move(t);
    }
};

IsdbtReceiver::IsdbtReceiver() : p_(new Impl) {}
IsdbtReceiver::~IsdbtReceiver() = default;

void IsdbtReceiver::configure(double inputRateHz) {
    Impl& I = *p_;
    I.reset();
    I.inRate = inputRateHz;
    // never a plain pass-through, not even at the native rate: the clock tracking works through the resampler's step
    I.rateOk = inputRateHz >= 6.0e6 && I.resampler.configure(inputRateHz, kSampleRate, false);
    I.decimate = I.rateOk && !I.resampler.passthrough();
}

void IsdbtReceiver::reset() { Impl& I = *p_; const double r = I.inRate; configure(r); }
void IsdbtReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->cb = std::move(cb); }
int IsdbtReceiver::detectLevel() const { return p_->detect.load(); }

void IsdbtReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rateOk || !n) return;
    if (!I.estDone) I.estimateCentre(x, n);
    I.condition(x, n);
    // no lock for a long time with this guess of the channel's place (no cyclic prefix found, or the TMCC search keeps failing): the next one
    if (!I.tmccOk) { I.searchedSince += (int64_t)n; if (I.searchedSince > (int64_t)(3.0 * I.inRate)) I.nextCandidate(); }
    for (int guard = 0; guard < 100000; guard++) {
        if (I.state == 0) {
            const size_t need = (size_t)(6.0 * (8192 + 2048) + 8192 + 2048);
            if (I.buf.size() < need) break;
            if (!I.acquire()) {
                const size_t drop = I.buf.size() - need / 2;
                I.buf.erase(I.buf.begin(), I.buf.begin() + (long)drop);
                I.base += (int64_t)drop;
                break;
            }
        } else {
            if (!I.step()) break;
            if (I.state == 0) continue;          // the search started again (another guess of the channel's place)
            const int64_t keep = (int64_t)std::llround(I.symStart) - 2 * (I.N + I.G) - 64;
            if (keep - I.base > (int64_t)(1 << 20)) {
                const size_t drop = (size_t)(keep - I.base);
                I.buf.erase(I.buf.begin(), I.buf.begin() + (long)drop);
                I.base += (int64_t)drop;
            }
        }
    }
    I.publish();
}

bool IsdbtReceiver::telemetry(RxTelemetry& out, uint64_t lastSeq) {
    Impl& I = *p_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.tel.seq <= lastSeq) return false;
    out = I.tel;
    return true;
}

} // namespace dect2
