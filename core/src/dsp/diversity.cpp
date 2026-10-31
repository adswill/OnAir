// Two-branch diversity combiner (selection and maximal-ratio), see dsp/diversity.h for the method.
#include "dect2/dsp/diversity.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace dect2 {

namespace {
constexpr float kPi = 3.14159265358979f;
inline float ramp01(float x, float lo, float hi) { return x <= lo ? 0.f : x >= hi ? 1.f : (x - lo) / (hi - lo); }
}

struct DiversityCombiner::Impl {
    DiversityConfig cfg;
    int N = 0, H = 0, maxLag = 0, maxShift = 0, lagLimit = 0;
    bool ok = false;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    // streams (absolute sample index = base + position)
    std::vector<cf32> A, Bd;
    int64_t base = 0, nPushed = 0, pos = 0;     // pos: absolute start of the next frame
    double th0 = 0, omega = 0, nRef = 0;        // oscillator taken off B: phase th0 at B's sample nRef, then omega (rad/sample)
    int64_t frameP0 = 0;
    void setOmega(double w) {
        const double jc = (double)(frameP0 + lag) + N / 2.0;
        th0 += omega * (jc - nRef); th0 = std::remainder(th0, 2 * 3.14159265358979);
        nRef = jc; omega = w;
    }
    int lag = 0;                                // whole samples B is read ahead of A
    // spectra
    std::vector<float> PA, PB;
    std::vector<cf32> C, Cinst, CinstPrev, af, bf, xc;
    float NA = 1, NB = 1;
    int hop = 0, hold = 0, searchHold = 0;
    int lockUp = 0, lockDown = 0;
    bool locked = false;
    int sel = 0;
    float qa = 0, qb = 0;
    std::vector<cf32> ola;
    std::vector<cf32> outQ;                     // finished samples not yet pulled
    std::vector<float> scratch;
    DiversityStatus st;
    cf32 gLast{1.f, 0.f};

    void start() {
        A.clear(); Bd.clear(); base = 0; nPushed = 0; pos = -H;
        th0 = 0; omega = 0; nRef = 0; lag = 0;
        PA.assign(N, 0.f); PB.assign(N, 0.f);
        C.assign(N, cf32()); Cinst.assign(N, cf32()); CinstPrev.assign(N, cf32());
        af.assign(N, cf32()); bf.assign(N, cf32()); xc.assign(N, cf32());
        ola.assign(N, cf32()); outQ.clear();
        if (!Q.empty()) { std::fill(Q.begin(), Q.end(), 0.f); searchFrames = 0; }
        cSeeded = false; coarseOk = false; cohS = 0; warm = 0; NA = NB = 1; hop = 0; hold = 0; searchHold = 0; lockUp = lockDown = 0; locked = false; sel = 0; qa = qb = 0;
        st = DiversityStatus();
    }

    inline cf32 aAt(int64_t i) const { return i < base || i - base >= (int64_t)A.size() ? cf32() : A[(size_t)(i - base)]; }
    inline cf32 bAt(int64_t i) const { return i < base || i - base >= (int64_t)Bd.size() ? cf32() : Bd[(size_t)(i - base)]; }

    bool reseedB = false, reseedC = false, cSeeded = false, coarseOk = false;
    float cohS = 0, phiPrev = 0;
    cf32 zNow{0, 0};
    int r1n = 0;
    bool pllOn = false;
    float pllPhi = 0, pllNu = 0;
    static constexpr int kFitFrames = 16;
    bool phiValid = false;
    int warm = 0;
    bool lagLocked = false;
    void resetCross() { reseedC = true; hold = 8; }

    static float medianOf(const std::vector<float>& v, std::vector<float>& tmp) {
        tmp = v;
        auto mid = tmp.begin() + tmp.size() / 2;
        std::nth_element(tmp.begin(), mid, tmp.end());
        return *mid;
    }


    // ---- acquisition state
    int nLag = 0, lagStep = 0, searchFrames = 0;
    std::vector<cf32> lagPh;          // [j * N + k] = exp(+j 2 pi k tau_j / N)
    std::vector<float> Q;             // [(s + maxShift) * nLag + j]: coherence summed over the frames searched
    void prepareSearch() {
        lagStep = std::max(2, N / 64);
        nLag = 2 * (maxLag / lagStep) + 1;
        lagPh.resize((size_t)nLag * N);
        for (int j = 0; j < nLag; j++) {
            const double tau = (double)(j - nLag / 2) * lagStep;
            for (int k = 0; k < N; k++) {
                const double a = 2.0 * 3.14159265358979 * (double)(((int64_t)k * (int64_t)std::llround(tau)) % N) / N;
                lagPh[(size_t)j * N + k] = cf32((float)std::cos(a), (float)std::sin(a));
            }
        }
        Q.assign((size_t)(2 * maxShift + 1) * nLag, 0.f); searchFrames = 0;
    }
    void coherentSearch(const std::vector<float>& mask, bool& pass) {
        (void)mask;
        std::vector<int> la, lb;   // bins where A / B clearly carry a signal
        std::vector<float> wa(N, 0.f), wb(N, 0.f);
        for (int k = 0; k < N; k++) {
            wa[k] = ramp01(PA[k] / NA, 1.3f, 1.8f); wb[k] = ramp01(PB[k] / NB, 1.3f, 1.8f);
            if (wa[k] > 0) la.push_back(k);
            if (wb[k] > 0) lb.push_back(k);
        }
        std::vector<int> bins;
        std::vector<float> w;
        std::vector<char> seen(N);
        for (int s = -maxShift; s <= maxShift; s++) {
            bins.clear(); w.clear();
            std::fill(seen.begin(), seen.end(), 0);
            for (int k : la) if (!seen[k]) { seen[k] = 1; bins.push_back(k); }
            for (int kb : lb) { const int k = ((kb - s) % N + N) % N; if (!seen[k]) { seen[k] = 1; bins.push_back(k); } }
            float den = 0;
            std::vector<cf32> X(bins.size());
            for (size_t i = 0; i < bins.size(); i++) {
                const int k = bins[i], kb = ((k + s) % N + N) % N;
                const float wk = std::max(wa[k], wb[kb]);
                X[i] = bf[kb] * std::conj(af[k]) * wk;
                den += wk * std::abs(bf[kb]) * std::abs(af[k]);
            }
            if (den <= 0) continue;
            for (int j = 0; j < nLag; j++) {
                cf32 z{0, 0};
                const cf32* ph = &lagPh[(size_t)j * N];
                for (size_t i = 0; i < bins.size(); i++) z += X[i] * ph[bins[i]];
                Q[(size_t)(s + maxShift) * nLag + j] += std::abs(z) / den;
            }
        }
        searchFrames++;
        if (searchFrames < 24 || (searchFrames & 7)) return;
        // decide: the strongest cell against the typical one
        std::vector<float> cells(Q);
        auto mid = cells.begin() + cells.size() / 2;
        std::nth_element(cells.begin(), mid, cells.end());
        const float med = *mid / searchFrames;
        auto q3 = cells.begin() + cells.size() * 3 / 4;
        std::nth_element(cells.begin(), q3, cells.end());
        const float sig = std::max(1e-4f, (*q3 / searchFrames - med) / 0.6745f);   // robust spread of the cells that carry no signal
        size_t bi = 0; float bv = -1;
        for (size_t i = 0; i < Q.size(); i++) if (Q[i] > bv) { bv = Q[i]; bi = i; }
        bv /= searchFrames;
        const int bs = (int)(bi / nLag) - maxShift, bj = (int)(bi % nLag);
        double shift = bs;
        if (bs > -maxShift && bs < maxShift) {
            float best_j = 0, y0 = 0, y2 = 0;
            best_j = Q[(size_t)(bs + maxShift) * nLag + bj];
            y0 = Q[(size_t)(bs - 1 + maxShift) * nLag + bj]; y2 = Q[(size_t)(bs + 1 + maxShift) * nLag + bj];
            const float dn = y0 - 2 * best_j + y2;
            if (dn < 0) shift += std::max(-0.5f, std::min(0.5f, 0.5f * (y0 - y2) / dn));
        }
        if (bv > med + 5.f * sig && bv > 1.25f * med) {
            if (std::fabs(shift) > 0.3) { setOmega(omega + 2.0 * kPi * shift / N); reseedB = true; }
            lag = std::max(-lagLimit, std::min(lagLimit, lag + (bj - nLag / 2) * lagStep));
            coarseOk = true; resetCross(); pass = true; lagLocked = true;
            std::fill(Q.begin(), Q.end(), 0.f); searchFrames = 0;
        } else if (searchFrames >= 200) {
            std::fill(Q.begin(), Q.end(), 0.f); searchFrames = 0;   // nothing stood out: start the averaging again
        }
    }

    // one STFT frame at absolute position p0
    void frame(int64_t p0) {
        frameP0 = p0;
        // B is read `lag` samples ahead and turned by e^{-j(th0 + omega (j - nRef))}, j = B's own sample index (a continuous oscillator;
        // when omega changes the phase carries on from the middle of the frame that changed it)
        {
            const int64_t j0 = p0 + lag;
            const double ph0 = th0 + omega * (double)(j0 - nRef);
            std::complex<double> ph(std::cos(ph0), -std::sin(ph0));
            const std::complex<double> stp(std::cos(omega), -std::sin(omega));
            for (int n = 0; n < N; n++) {
                af[n] = aAt(p0 + n) * win[n];
                bf[n] = bAt(j0 + n) * cf32((float)ph.real(), (float)ph.imag()) * win[n];
                ph *= stp;
                if ((n & 127) == 127) ph /= std::abs(ph);
            }
        }
        fft->forward(af.data()); fft->forward(bf.data());
        const float al = coarseOk ? cfg.smooth : std::max(cfg.smooth, 0.985f);   // longer averages while the frequency offset is searched for
        const bool first = hop == 0;
        const bool rsB = reseedB, rsC = reseedC;
        if (rsB) warm = 0; else warm++;
        reseedB = reseedC = false;
        bool pass = false;   // a correction was just made: this frame passes branch A
        for (int k = 0; k < N; k++) {
            const float pa = std::norm(af[k]), pb = std::norm(bf[k]);
            Cinst[k] = bf[k] * std::conj(af[k]);
            if (first) { PA[k] = pa; PB[k] = pb; continue; }
            PA[k] += (pa - PA[k]) * (1.f - al);
            if (rsB) PB[k] = pb; else PB[k] += (pb - PB[k]) * (1.f - al);
        }
        hop++;
        if (hold > 0) hold--;
        // noise levels: the median bin power (the wanted signal fills well under half of the bins); a little up for the median of a smoothed chi-square
        if (hop < 4 || (hop & 3) == 0) {
            NA = medianOf(PA, scratch) * 1.012f + 1e-20f;
            NB = medianOf(PB, scratch) * 1.012f + 1e-20f;
        }
        // signal bins: both branches clearly above their noise
        float mSum = 0, sA = 0, sB = 0, cohNum = 0, cohDen = 0;
        std::vector<float> mask(N);
        for (int k = 0; k < N; k++) {
            const float ra = PA[k] / NA, rb = PB[k] / NB;
            mask[k] = ramp01(std::max(ra, rb), 1.3f, 1.8f);   // the stronger branch decides where the signal is: a weak branch is still combined
            const float w = mask[k];
            if (w <= 0) continue;
            mSum += w;
            sA += w * (PA[k] - NA);   // not clamped: the sums are unbiased, a branch with no signal sums to about 0
            sB += w * (PB[k] - NB);
            cohDen += w * std::sqrt(PA[k] * PB[k]);
        }
        sA = std::max(sA, 0.02f * mSum * NA); sB = std::max(sB, 0.02f * mSum * NB);
        // the cross spectrum's common phase of this frame against the smoothed shape (all signal bins together), then the shape is updated
        // from the frame with that phase taken off: the shape keeps the per-bin phase (delay, filters), phi follows the oscillators' phase
        float phi = 0, aFit = 0, bFit = 0;
        float cvInst = 0;   // coherence by cross-validation: the phase is fitted on the even bins and judged on the odd ones and the other way round,
                            // so noise (which any fitted phase would flatter) scores 0 and a shared signal scores about its coherence
        float rFit = 0;   // how well the smoothed shape follows a straight phase line over frequency (1 = exactly)
        bool haveModel = false;
        if (first || rsB || rsC || !cSeeded) {
            for (int k = 0; k < N; k++) C[k] = Cinst[k];
            cSeeded = true;
        } else {
            // the shape of the cross spectrum is a phase line over frequency (a delay left over after the whole samples, plus a constant),
            // fitted from the smoothed shape on the signal bins: far less noisy than the bins one by one
            cf32 s1{0, 0};
            float sw = 0;
            for (int k = 0; k < N; k++) {
                const int k1 = (k + 1) % N;
                if (mask[k] > 0 && mask[k1] > 0) s1 += mask[k] * mask[k1] * (C[k1] * std::conj(C[k]));
                if (mask[k] > 0) sw += mask[k] * std::abs(C[k]);
            }
            bFit = std::abs(s1) > 0 ? std::arg(s1) : 0.f;
            cf32 s0{0, 0};
            for (int k = 0; k < N; k++) if (mask[k] > 0) {
                const float kk = (float)(k < N / 2 ? k : k - N);
                s0 += mask[k] * (C[k] * cf32(std::cos(bFit * kk), -std::sin(bFit * kk)));
            }
            aFit = std::abs(s0) > 0 ? std::arg(s0) : 0.f;
            rFit = sw > 0 ? std::min(1.f, std::abs(s0) / sw) : 0.f;
            haveModel = sw > 0;
            cf32 rho{0, 0}, par[2] = {cf32(), cf32()};
            float den[2] = {0, 0};
            for (int k = 0; k < N; k++) if (mask[k] > 0) {
                const float kk = (float)(k < N / 2 ? k : k - N);
                const cf32 shp(std::cos(aFit + bFit * kk), std::sin(aFit + bFit * kk));
                const float wk = mask[k] * std::abs(C[k]);
                const cf32 v = wk * (Cinst[k] * std::conj(shp));
                rho += v; par[k & 1] += v; den[k & 1] += wk * std::abs(Cinst[k]);
            }
            if (den[0] > 0 && den[1] > 0) {
                const cf32 e0 = par[0] / std::max(std::abs(par[0]), 1e-30f), e1 = par[1] / std::max(std::abs(par[1]), 1e-30f);
                cvInst = 0.5f * (std::real(par[1] * std::conj(e0)) / den[1] + std::real(par[0] * std::conj(e1)) / den[0]);
            }
            if (std::abs(rho) > 0) phi = std::arg(rho);
            zNow = rho;
            const cf32 rot(std::cos(phi), -std::sin(phi));
            const float alC = locked ? 1.f - (1.f - al) * 0.25f : al;   // the shape is static: average it longer once locked
            for (int k = 0; k < N; k++) C[k] += (Cinst[k] * rot - C[k]) * (1.f - alC);
        }
        for (int k = 0; k < N; k++) if (mask[k] > 0) cohNum += mask[k] * std::abs(C[k]);
        (void)cohNum; (void)cohDen;
        // second-order phase loop on the frame's common phase: averages the noise of the single-frame estimate (about 0.5 rad on weak signals)
        // while following a residual frequency offset without lag; its output is the phase used to combine
        if (haveModel && locked && !pass && !rsB && !rsC) {
            if (!pllOn) { pllPhi = phi; pllNu = 0; pllOn = true; }
            else {
                const float pred = pllPhi + pllNu;
                const float e = std::remainder(phi - pred, 2 * kPi);
                pllPhi = pred + 0.25f * e;
                pllNu += 0.03f * e;
            }
        } else pllOn = false;
        const float phiUse = pllOn ? pllPhi : phi;
        const bool enough = mSum >= 3.f;
        if (rsB || rsC) cohS = 0;
        else if (enough) cohS += (cvInst - cohS) * 0.1f;
        const float coh = cohS;
        // lock: the branches are coherent over the signal bins
        if (enough && coh > 0.10f) { lockUp++; lockDown = 0; } else if (!enough || coh < 0.04f) { lockDown++; lockUp = 0; }
        if (!locked && coarseOk && lockUp >= 6) locked = true;
        if (locked && lockDown >= 40) { locked = false; coarseOk = false; }

        // fine frequency tracking: the phase advance from frame to frame. Locked: the frame's common phase against the shape (all the signal
        // bins, weighted by the shape), slowly; before that: the phase of the product of two successive cross spectra, a little faster.
        if (coarseOk && hold == 0) {
            if (locked && phiValid) {
                // frequency from the loop below: its rate term, applied every few frames
                if (++r1n >= kFitFrames) {
                    const double d = 0.8 * pllNu;
                    setOmega(omega + d / H);
                    pllNu -= d;
                    r1n = 0;
                }
            } else if (enough && hop > 2) {
                cf32 rho{0, 0}; float den = 0;
                for (int k = 0; k < N; k++) {
                    if (mask[k] <= 0) continue;
                    rho += mask[k] * (Cinst[k] * std::conj(CinstPrev[k]));
                    den += mask[k] * std::abs(Cinst[k]) * std::abs(CinstPrev[k]);
                }
                if (den > 0 && std::abs(rho) / den > 0.15f) setOmega(omega + 0.12 * std::arg(rho) / H);
            }
        }
        phiPrev = phi; phiValid = !(first || rsB || rsC);
        if (!locked || hold > 0 || rsB || rsC) r1n = 0;
        std::copy(Cinst.begin(), Cinst.end(), CinstPrev.begin());

        // acquisition: the frequency offset (whole bins) and the delay together. For every shift s of B against A the cross spectrum over the
        // signal bins is summed with a set of delay phase ramps; the coherence of the best ramp is averaged over frames. This works when the
        // power spectra of a weak branch are too noisy to show where the signal is, because the sum is coherent over all signal bins.
        if (!coarseOk && warm >= 40 && enough) {
            coherentSearch(mask, pass);
        }

        // whole-sample delay: the cross-correlation of the two branches over the signal bins (inverse FFT of the masked cross spectrum); its
        // peak is where B lags A. A narrowband signal pins the delay down to about 1/bandwidth only, which is all that is needed: what is
        // left is absorbed by the per-bin phase. The lag moves only when the peak is clearly away from 0 (hysteresis, so it does not dither).
        if (enough && coh > 0.1f && hold == 0 && hop > 8) {
            for (int k = 0; k < N; k++) xc[k] = C[k] * mask[k];
            fft->inverse(xc.data());
            int pk = 0; float best = -1, sum = 0;
            for (int n = -maxLag; n <= maxLag; n++) {
                const float v = std::abs(xc[((n % N) + N) % N]);
                sum += v;
                if (v > best) { best = v; pk = n; }
            }
            const float mean = sum / (2 * maxLag + 1);
            double tau = pk;
            if (pk > -maxLag && pk < maxLag) {
                const float y0 = std::abs(xc[((pk - 1) % N + N) % N]), y2 = std::abs(xc[((pk + 1) % N + N) % N]);
                const float dn = y0 - 2 * best + y2;
                if (dn < 0) tau += std::max(-0.5f, std::min(0.5f, 0.5f * (y0 - y2) / dn));
            }
            const double lobe = N / std::max(mSum, 3.f);
            if (best > 3.0f * mean && std::fabs(tau) > std::max(std::max(8.0, 0.8 * lobe), N / 64.0) && std::abs(lag + (int)std::lround(tau)) <= lagLimit) {
                lag += (int)std::lround(tau);
                resetCross(); pass = true;
            }
        }

        // branch gain: level from the noise-subtracted powers, phase per bin from the cross spectrum (3 bins averaged)
        const float rho2 = sA > 0 ? sB / sA : 1.f;
        const float gmag = std::sqrt(std::max(0.0025f, std::min(400.f, rho2)));
        st.locked = locked;
        st.cfoHz = omega * cfg.rateHz / (2.0 * kPi);
        st.lagSamples = lag;
        st.bins = (int)std::lround(mSum);
        const float snrA = sA / std::max(1e-12f, mSum * NA), snrB = sB / std::max(1e-12f, mSum * NB);
        if (enough) {
            const float a = 0.2f;
            st.snrADb = st.snrADb < -90 ? 10 * std::log10(snrA + 1e-9f) : st.snrADb + a * (10 * std::log10(snrA + 1e-9f) - st.snrADb);
            st.snrBDb = st.snrBDb < -90 ? 10 * std::log10(snrB + 1e-9f) : st.snrBDb + a * (10 * std::log10(snrB + 1e-9f) - st.snrBDb);
            st.gainBDb = 20 * std::log10(gmag);
        }
        // selection state
        const bool useB = [&] {
            if (!locked) {
                // no coherent signal yet: still pass a branch that clearly carries more of it than A (A's antenna dead)
                float ea = 0, eb = 0;
                for (int k = 0; k < N; k++) { ea += std::max(PA[k] - 1.3f * NA, 0.f) / NA; eb += std::max(PB[k] - 1.3f * NB, 0.f) / NB; }
                if (cfg.mode == DiversityConfig::Selection && eb > 4.f * ea + 10.f) sel = 1;
                else if (ea >= eb) sel = 0;
                return sel == 1;
            }
            qa = snrA; qb = snrB;
            if (qb > qa * 1.26f) sel = 1; else if (qa > qb * 1.26f) sel = 0;
            return sel == 1;
        }();
        const bool mrc = cfg.mode == DiversityConfig::Mrc && locked;
        st.selected = mrc ? 2 : (useB ? 1 : 0);
        if (locked || enough) st.outSnrDb = 10 * std::log10((mrc ? snrA + snrB : std::max(snrA, snrB)) + 1e-9f);

        // combine
        const float iNA = 1.f / NA, iNB = 1.f / NB;
        for (int k = 0; k < N; k++) {
            const float w = locked && !pass ? mask[k] : 0.f;
            cf32 y = af[k];
            if (w > 0 && haveModel) {
                const float kk = (float)(k < N / 2 ? k : k - N);
                const float ph = aFit + bFit * kk + phiUse;
                const cf32 g = cf32(std::cos(ph), std::sin(ph)) * (gmag * w * rFit);   // B = g A (plus noise); less trusted where the phase is uncertain
                if (mrc) y = (af[k] * iNA + std::conj(g) * bf[k] * iNB) / (iNA + std::norm(g) * iNB);
                else if (useB) y = af[k] * (1.f - w) + (bf[k] * std::conj(g) / std::max(std::norm(g), 1e-12f)) * w;
            } else if (!locked && useB && !pass) y = bf[k];
            af[k] = y;
        }
        fft->inverse(af.data());
        const float sc = 1.f / N;
        for (int n = 0; n < N; n++) ola[n] += af[n] * (win[n] * sc);
        // the first H samples of the overlap buffer are finished
        if (p0 >= 0) outQ.insert(outQ.end(), ola.begin(), ola.begin() + H);
        std::memmove(ola.data(), ola.data() + H, (size_t)(N - H) * sizeof(cf32));
        std::fill(ola.begin() + (N - H), ola.end(), cf32());
        st.frames++;
    }

    void run() {
        for (;;) {
            const int64_t needA = pos + N, needB = pos + N + std::max(lag, 0);
            if (needA > nPushed || needB > nPushed) break;
            frame(pos);
            pos += H;
        }
        // drop what no frame can need any more
        const int64_t keep = pos + std::min(lag, 0) - maxLag - 4;
        if (keep - base > (int64_t)(8 * N)) {
            const size_t drop = (size_t)(keep - base);
            A.erase(A.begin(), A.begin() + (long)std::min(drop, A.size()));
            Bd.erase(Bd.begin(), Bd.begin() + (long)std::min(drop, Bd.size()));
            base += (int64_t)drop;
        }
    }
};


DiversityAlign diversityCoarseAlign(const cf32* a, const cf32* b, size_t n, double rateHz, double maxCfoHz, int maxSkew) {
    DiversityAlign res;
    int lg = 0; while ((size_t(1) << lg) < n) lg++;
    if (!a || !b || n < 16384 || (size_t(1) << lg) != n || !(rateHz > 8000) || maxSkew < 1) return res;
    const int P = (int)n;
    Fft fft(P);
    std::vector<cf32> A(P), B(P);
    for (int i = 0; i < P; i++) {
        const float w = 0.5f - 0.5f * std::cos(2.f * kPi * (float)(i + 0.5f) / (float)P);
        A[i] = a[i] * w; B[i] = b[i] * w;
    }
    fft.forward(A.data()); fft.forward(B.data());
    // smoothed power spectra over +-W bins, their excess over the median = where a signal is
    const int W = std::max(4, P / 8192);
    auto excess = [&](const std::vector<cf32>& X, std::vector<float>& e) {
        std::vector<float> pw(P), sm(P);
        for (int k = 0; k < P; k++) pw[k] = std::norm(X[k]);
        double acc = 0;
        for (int j = -W; j <= W; j++) acc += pw[(j + P) % P];
        for (int k = 0; k < P; k++) { sm[k] = (float)acc; acc += pw[(k + W + 1) % P] - pw[((k - W) % P + P) % P]; }
        std::vector<float> tmp = sm;
        auto mid = tmp.begin() + P / 2;
        std::nth_element(tmp.begin(), mid, tmp.end());
        const float med = std::max(*mid, 1e-30f);
        e.resize(P);
        for (int k = 0; k < P; k++) e[k] = std::max(sm[k] / med - 1.5f, 0.f);   // below 1.5x the median: noise
    };
    std::vector<float> ea, eb;
    excess(A, ea); excess(B, eb);
    double sa = 0, sb = 0; for (int k = 0; k < P; k++) { sa += ea[k]; sb += eb[k]; }
    if (sa <= 0 || sb <= 0) return res;
    // power-spectrum shift correlation for all shifts at once: r(s) = sum_k ea[k] eb[k+s]  (FFT cross-correlation)
    std::vector<cf32> FA(P), FB(P);
    for (int k = 0; k < P; k++) { FA[k] = cf32(ea[k], 0); FB[k] = cf32(eb[k], 0); }
    fft.forward(FA.data()); fft.forward(FB.data());
    for (int k = 0; k < P; k++) FA[k] = std::conj(FA[k]) * FB[k];
    fft.inverse(FA.data());
    const int maxS = std::min(P / 4, (int)std::ceil(maxCfoHz / (rateHz / P)));
    std::vector<std::pair<float, int>> cand;
    for (int s = -maxS; s <= maxS; s++) {
        const float v = FA[(s % P + P) % P].real(), vm = FA[((s - 1) % P + P) % P].real(), vp = FA[((s + 1) % P + P) % P].real();
        if (v > vm && v >= vp) cand.push_back({v, s});
    }
    std::sort(cand.begin(), cand.end(), [](auto& x, auto& y) { return x.first > y.first; });
    if (cand.size() > 3) cand.resize(3);
    if (cand.empty()) return res;
    // coherent check around each candidate: phases only, weighted by the signal bins of both; the delay is the peak of the inverse FFT
    float bestScore = 0, bestPeak = 0; int bestSkew = 0, bestShift = 0;
    std::vector<cf32> X(P);
    for (const auto& cd : cand) {
        for (int d = -(W + 2); d <= W + 2; d++) {
            const int s = cd.second + d;
            if (std::abs(s) > maxS) continue;
            std::fill(X.begin(), X.end(), cf32());
            int cnt = 0;
            for (int k = 0; k < P; k++) {
                const int kb = ((k + s) % P + P) % P;
                const float w = std::sqrt(ea[k] * eb[kb]);
                if (w <= 0) continue;
                const cf32 c = B[kb] * std::conj(A[k]);
                const float m = std::abs(c);
                if (m > 0) { X[k] = c * (w / m); cnt++; }
            }
            if (cnt < 4) continue;
            fft.inverse(X.data());
            float pk = 0, sum = 0; int pn = 0;
            for (int q = -maxSkew; q <= maxSkew; q++) {
                const float v = std::abs(X[(q % P + P) % P]);
                sum += v;
                if (v > pk) { pk = v; pn = q; }
            }
            const float mean = sum / (2 * maxSkew + 1);
            const float score = mean > 0 ? pk / mean : 0.f;
            if (score > bestScore) { bestScore = score; bestPeak = pk; bestSkew = pn; bestShift = s; }
        }
    }
    (void)bestPeak;
    res.confidence = bestScore;
    if (bestScore < 8.f) return res;
    res.ok = true; res.skewSamples = bestSkew; res.cfoHz = bestShift * rateHz / P;
    return res;
}

DiversityCombiner::DiversityCombiner() : p_(new Impl) {}
DiversityCombiner::~DiversityCombiner() = default;

bool DiversityCombiner::configure(const DiversityConfig& c) {
    Impl& d = *p_;
    d.cfg = c;
    if (!(c.rateHz > 8000)) { d.ok = false; return false; }
    int n = c.fftSize;
    if (n <= 0) { n = 256; while (n < 8192 && n * 1.414 < c.rateHz / 500.0) n <<= 1; }
    if (n < 64 || (n & (n - 1))) { d.ok = false; return false; }
    d.N = n; d.H = n / 2;
    d.maxLag = c.maxLag > 0 ? c.maxLag : n / 4;
    d.lagLimit = c.lagLimit > 0 ? c.lagLimit : (int)(0.25 * c.rateHz);
    d.maxShift = std::max(2, std::min(n / 4, (int)std::ceil(c.maxCfoHz / (c.rateHz / n))));
    d.fft = std::make_unique<Fft>(n);
    d.win.resize(n);
    for (int i = 0; i < n; i++) d.win[i] = std::sin(kPi * (float)i / (float)n);   // periodic sqrt-Hann: w^2 sums to 1 at 50 % overlap
    d.ok = true;
    d.start();
    d.prepareSearch();
    return true;
}
void DiversityCombiner::setMode(DiversityConfig::Mode m) { p_->cfg.mode = m; }
void DiversityCombiner::reset() { if (p_->ok) p_->start(); }
int DiversityCombiner::latency() const { return p_->N; }
int DiversityCombiner::fftSize() const { return p_->N; }
DiversityStatus DiversityCombiner::status() const { return p_->st; }

void DiversityCombiner::push(const cf32* a, const cf32* b, size_t n) {
    Impl& d = *p_;
    if (!d.ok || n == 0) return;
    d.A.insert(d.A.end(), a, a + n);
    d.Bd.insert(d.Bd.end(), b, b + n);
    d.nPushed += (int64_t)n;
    d.run();
}

size_t DiversityCombiner::pull(std::vector<cf32>& out) {
    Impl& d = *p_;
    const size_t n = d.outQ.size();
    out.insert(out.end(), d.outQ.begin(), d.outQ.end());
    d.outQ.clear();
    return n;
}

} // namespace dect2
