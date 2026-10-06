#include "dect2/dvbt_rx.h"
#include "dect2/dvbt_fec.h"
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
    std::vector<cf32> rsOut;
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
    double accPhi = 0, accSlope = 0;   // running common phase and phase ramp relative to the continual-pilot reference
    double epsFrac = 0;           // fractional carrier-frequency offset in subcarrier spacings (tracked)
    int intShift = 0;
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
        state = 0; mode = gi = -1; N = G = K = 0; symStart = 0; epsFrac = 0; intShift = 0; intLocked = false;
        intScore.clear(); intSymbols = 0; absSym = 0; fft.reset(); agreeCount = 0; agreeMode = agreeGi = -1; acqLastEnd = 0;
        tpsBitsSeen.clear(); tpsBase = 0; prevTps.clear(); prevValid = false; tpsOk = false; tpsFailures = 0; secSinceTps = 0;
        for (int i = 0; i < 4; i++) { hypVotes[i] = 0; hypScore[i] = 0; }
        grid.clear(); gridAge.clear(); cpRef.clear(); cpRefValid = false; gridFilled = 0;
        fec.reset(); streamSecs = 0; symbols = 0; packetsOut = 0; detect = 0;
        buf.clear(); base = 0; resampler.reset();
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
        timingAcc = 0; accPhi = accSlope = 0;
        epsFrac = -std::arg(best.corr) / (2 * M_PI);   // c = sum y[n] conj(y[n+N]) has phase -2 pi eps
        back = std::min(G / 4, 24);
        fft = std::make_unique<Fft>(N);
        fbuf.assign(N, cf32(0, 0));
        intScore.assign(41, 0.0); intSymbols = 0; intLocked = false; intShift = 0;
        tpsBitsSeen.clear(); tpsBase = 0; prevValid = false; tpsOk = false; absSym = 0;
        grid.assign((size_t)(K + 2) / 3 + 1, cf32(0, 0)); gridAge.assign(grid.size(), 255);
        cpRef.assign(continualPilots(mode).size(), cf32(1, 0)); cpRefValid = false; gridFilled = 0;
        for (int i = 0; i < 4; i++) { hypVotes[i] = 0; hypScore[i] = 0; }
        fec.reset();
        state = 1;
        return true;
    }

    // ------------------------------------------------------------------ one symbol
    // Transforms the symbol whose CP starts at `s` (integer) into carriers Y[0..K-1]
    void transform(int64_t s) {
        const int64_t w = s + G - back;
        const double ph0 = -2.0 * M_PI * epsFrac * (double)(w % 1048576) / N;
        cd cur(std::cos(ph0), std::sin(ph0));
        const cd rot(std::cos(-2.0 * M_PI * epsFrac / N), std::sin(-2.0 * M_PI * epsFrac / N));
        for (int i = 0; i < N; i++) {
            const cf32 v = at(w + i);
            const cd o = cd(v.real(), v.imag()) * cur;
            fbuf[i] = cf32((float)o.real(), (float)o.imag());
            cur *= rot;
            if ((i & 255) == 255) cur /= std::abs(cur);
        }
        fft->forward(fbuf.data());
        Y.resize(K);
        for (int k = 0; k < K; k++) Y[k] = fbuf[(((k + intShift - kc) % N) + N) % N];
    }

    // CP correlation around the expected position: timing error and fractional CFO
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

    // integer CFO: continual pilots keep their value in every symbol, so Y_l[k+s] conj(Y_{l-1}[k+s]) has one common phase when s is right
    std::vector<cf32> prevRawFull;     // previous symbol's full FFT (for the integer search over a range of shifts)
    void integerSearch(int64_t s) {
        // transform without carrier shift into fbuf for the next symbol, keep the previous one
        if (prevRawFull.size() != (size_t)N) prevRawFull.assign(N, cf32(0, 0));
        std::vector<cf32> cur(fbuf.begin(), fbuf.end());
        (void)s;
        if (intSymbols > 0) {
            const auto& cp = continualPilots(mode);
            for (int sh = -20; sh <= 20; sh++) {
                cd acc = 0; double mag = 0;
                for (int k : cp) {
                    const int idx = (((k + sh - kc) % N) + N) % N;
                    const cd z = cd(cur[idx].real(), cur[idx].imag()) * std::conj(cd(prevRawFull[idx].real(), prevRawFull[idx].imag()));
                    acc += z; mag += std::abs(z);
                }
                intScore[sh + 20] += mag > 0 ? std::abs(acc) / mag : 0;
            }
        }
        prevRawFull = cur;
        intSymbols++;
        if (intSymbols >= 12) {
            int best = 0; double bm = -1, second = -1;
            for (int i = 0; i < 41; i++) if (intScore[i] > bm) { bm = intScore[i]; best = i; }
            for (int i = 0; i < 41; i++) if (i != best && intScore[i] > second) second = intScore[i];
            if (bm > 0 && bm > 1.5 * second) { intShift = best - 20; intLocked = true; }
            else { std::fill(intScore.begin(), intScore.end(), 0.0); intSymbols = 0; }
        }
    }

    // process the next symbol; returns false if the data is not yet available
    bool step() {
        const int P = N + G;
        int64_t s = (int64_t)std::llround(symStart);
        if (s + P + N + 32 > end()) return false;
        if (s - 16 < base) { symStart += P; return true; } // fell behind the buffer: skip
        double terr, eraw, mag;
        cpTrack(s, terr, eraw, mag);
        // timing loop (the CP correlation peak sits at the symbol start)
        timingAcc += 0.08 * terr;
        if (std::fabs(timingAcc) >= 1.0) { const double stp = std::round(timingAcc); symStart += stp; timingAcc -= stp; }
        // fractional CFO loop
        const double d = wrap(eraw - wrapFrac(epsFrac));
        epsFrac += 0.15 * d;
        if (epsFrac > 0.5 || epsFrac < -0.5) {
            // roll the integer part into the carrier shift
            const double step = epsFrac > 0 ? 1.0 : -1.0;
            epsFrac -= step;
            if (intLocked) intShift += (int)step;
        }
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
        // ---- TPS bit for this symbol: differential BPSK against the previous symbol
        if (prevValid && prevTps.size() == tpsK.size()) {
            double acc = 0;
            for (size_t i = 0; i < tpsK.size(); i++) acc += (Y[tpsK[i]] * std::conj(prevTps[i])).real();
            tpsBitsSeen.push_back(acc < 0 ? 1 : 0);
        } else tpsBitsSeen.push_back(-1);
        if (prevValid && prevTps.size() == tpsK.size() && symbols % 3 == 0) {
            tpsShow.clear();
            for (size_t i = 0; i < tpsK.size(); i++) { const cf32 z = Y[tpsK[i]] * std::conj(prevTps[i]); const float m = std::abs(z); if (m > 1e-9f) tpsShow.push_back(z / m); }
        }
        prevTps.resize(tpsK.size());
        for (size_t i = 0; i < tpsK.size(); i++) prevTps[i] = Y[tpsK[i]];
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
        estimateAndDecode(symIdx);
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
        if (tpsDecode(b, q, fi, odd) && q.mode == mode && q.guard == gi) {
            tpsFailures = 0;
            secSinceTps = 0;
            frameIdx = fi;
            if (!(q == prm)) { prm = q; fec.configure(prm); }
        } else if (++tpsFailures >= 3) {
            // lost: start over with a fresh acquisition
            tpsOk = false; state = 0; detect = 1; agreeCount = 0;
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
        if (gridFilled < 4) return;
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
    if (I.decimate) { I.rsOut.clear(); I.resampler.process(x, n, I.rsOut); I.buf.insert(I.buf.end(), I.rsOut.begin(), I.rsOut.end()); }
    else I.buf.insert(I.buf.end(), x, x + n);
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
