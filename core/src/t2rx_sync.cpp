// P1 preamble detection and guard-interval detection of the DVB-T2 receiver.
#include "t2rx_impl.h"
#if (defined(__ARM_NEON) || defined(__aarch64__)) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#define T2P1_NEON 1
#elif defined(__SSE2__) && !defined(DECT2_NO_SIMD)
#include <emmintrin.h>
#define T2P1_SSE2 1
#endif

namespace dect2 {

void T2Receiver::Impl::p1Metric(int64_t lo, int64_t len, int64_t d0, int64_t d1) {
    const int64_t n = d1 + kP1Len - d0;   // samples covered: relative indices [d0, d1 + kP1Len)
    p1Evaluated += (uint64_t)(d1 - d0 + 1);
    pq1.resize(n + 1); pq2.resize(n + 1); pe.resize(n + 1);
    pq1[0] = 0; pq2[0] = 0; pe[0] = 0;
    // every index read is inside the buffer: lo >= base, lo + len == end(), and the offsets are guarded by the conditions below
    const cf32* xs = buf.data() + (lo - base);
    // plain real arithmetic: std::complex<double> multiplies go through a slow NaN-checking library call
    double a1r = 0, a1i = 0, a2r = 0, a2i = 0, ae = 0;
    for (int64_t j = 0; j < n; j++) {
        const int64_t i = d0 + j;
        const double xr = xs[i].real(), xi = xs[i].imag();
        const cd& ph = phiTab[(lo + i) & 1023];
        if (i + kP1CLen < len) {   // x * conj(y) * ph
            const double yr = xs[i + kP1CLen].real(), yi = xs[i + kP1CLen].imag();
            const double tr = xr * yr + xi * yi, ti = xi * yr - xr * yi;
            a1r += tr * ph.real() - ti * ph.imag(); a1i += tr * ph.imag() + ti * ph.real();
        }
        if (i >= kP1BLen) {
            const double yr = xs[i - kP1BLen].real(), yi = xs[i - kP1BLen].imag();
            const double tr = xr * yr + xi * yi, ti = xi * yr - xr * yi;
            a2r += tr * ph.real() - ti * ph.imag(); a2i += tr * ph.imag() + ti * ph.real();
        }
        ae += xr * xr + xi * xi;
        pq1[j + 1] = cd(a1r, a1i);
        pq2[j + 1] = cd(a2r, a2i);
        pe[j + 1] = ae;
    }
    for (int64_t d = d0; d <= d1; d++) {
        const int64_t jd = d - d0;
        cd sc = pq1[jd + kP1CLen] - pq1[jd];
        cd sb = pq2[jd + kP1Len] - pq2[jd + kP1CLen + kP1ALen];
        double en = 0.5 * (pe[jd + kP1Len] - pe[jd]);
        m[d] = en > 1e-12 ? (float)((std::abs(sc) + std::abs(sb)) / en) : 0.f;
    }
}

bool T2Receiver::Impl::p1Detect(int64_t lo, int64_t lastD, int64_t dA, int64_t dB) {
    const int64_t W = kPeakHalfWidth;
    const double before = prevPos;
    for (int64_t d = dA; d <= dB; d++) {
        if (m[d] < kP1Threshold) continue;
        bool peak = true;
        for (int64_t k = std::max<int64_t>(0, d - W); k <= std::min(lastD, d + W); k++)
            if (m[k] > m[d] || (m[k] == m[d] && k < d)) { peak = false; break; }
        if (!peak) continue;
        int64_t dAbs = lo + d;
        if (dAbs - lastP1Abs < 2 * kP1Len) continue;
        handleP1Candidate(dAbs, m[d]);
        d += W;
    }
    return prevPos != before;
}

// Sums over one block of kCoarseBlk samples from x on: the energy, and x * conj(y) with y 542 samples later (c, when wanted) and 482 earlier
// (b). NEON or SSE2 where available (two partial sums per term, so the additions do not wait on each other), plain loops elsewhere.
static void blockSums(const cf32* x, bool wantC, bool wantB, float& e, cd& c, cd& b) {
    const float* xf = reinterpret_cast<const float*>(x);
    const float* yc = xf + 2 * kP1CLen;
    const float* yb = xf - 2 * kP1BLen;
#if defined(T2P1_NEON)
    const float32x4_t z = vdupq_n_f32(0);
    float32x4_t e0 = z, e1 = z, c0 = z, c1 = z, c2 = z, c3 = z, b0 = z, b1 = z, b2 = z, b3 = z;
    for (int n = 0; n < kCoarseBlk; n += 4) {
        const float32x4x2_t v = vld2q_f32(xf + 2 * n);   // re, im of four samples
        e0 = vfmaq_f32(e0, v.val[0], v.val[0]); e1 = vfmaq_f32(e1, v.val[1], v.val[1]);
        if (wantC) {
            const float32x4x2_t y = vld2q_f32(yc + 2 * n);
            c0 = vfmaq_f32(c0, v.val[0], y.val[0]); c1 = vfmaq_f32(c1, v.val[1], y.val[1]);
            c2 = vfmaq_f32(c2, v.val[1], y.val[0]); c3 = vfmaq_f32(c3, v.val[0], y.val[1]);
        }
        if (wantB) {
            const float32x4x2_t y = vld2q_f32(yb + 2 * n);
            b0 = vfmaq_f32(b0, v.val[0], y.val[0]); b1 = vfmaq_f32(b1, v.val[1], y.val[1]);
            b2 = vfmaq_f32(b2, v.val[1], y.val[0]); b3 = vfmaq_f32(b3, v.val[0], y.val[1]);
        }
    }
    e = vaddvq_f32(vaddq_f32(e0, e1));
    c = cd(vaddvq_f32(vaddq_f32(c0, c1)), vaddvq_f32(vsubq_f32(c2, c3)));
    b = cd(vaddvq_f32(vaddq_f32(b0, b1)), vaddvq_f32(vsubq_f32(b2, b3)));
#elif defined(T2P1_SSE2)
    auto load = [](const float* p, __m128& re, __m128& im) {   // four samples, split into re and im
        const __m128 lo = _mm_loadu_ps(p), hi = _mm_loadu_ps(p + 4);
        re = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0)); im = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
    };
    auto hsum = [](__m128 v) { float f[4]; _mm_storeu_ps(f, v); return (f[0] + f[2]) + (f[1] + f[3]); };
    const __m128 z = _mm_setzero_ps();
    __m128 e0 = z, e1 = z, cr = z, ci = z, br = z, bi = z;
    for (int n = 0; n < kCoarseBlk; n += 4) {
        __m128 xr, xi, yr, yi;
        load(xf + 2 * n, xr, xi);
        e0 = _mm_add_ps(e0, _mm_mul_ps(xr, xr)); e1 = _mm_add_ps(e1, _mm_mul_ps(xi, xi));
        if (wantC) {
            load(yc + 2 * n, yr, yi);
            cr = _mm_add_ps(cr, _mm_add_ps(_mm_mul_ps(xr, yr), _mm_mul_ps(xi, yi)));
            ci = _mm_add_ps(ci, _mm_sub_ps(_mm_mul_ps(xi, yr), _mm_mul_ps(xr, yi)));
        }
        if (wantB) {
            load(yb + 2 * n, yr, yi);
            br = _mm_add_ps(br, _mm_add_ps(_mm_mul_ps(xr, yr), _mm_mul_ps(xi, yi)));
            bi = _mm_add_ps(bi, _mm_sub_ps(_mm_mul_ps(xi, yr), _mm_mul_ps(xr, yi)));
        }
    }
    e = hsum(_mm_add_ps(e0, e1));
    c = cd(hsum(cr), hsum(ci));
    b = cd(hsum(br), hsum(bi));
#else
    float se = 0, cr = 0, ci = 0, br = 0, bi = 0;
    for (int n = 0; n < 2 * kCoarseBlk; n += 2) {
        se += xf[n] * xf[n] + xf[n + 1] * xf[n + 1];
        if (wantC) { cr += xf[n] * yc[n] + xf[n + 1] * yc[n + 1]; ci += xf[n + 1] * yc[n] - xf[n] * yc[n + 1]; }
        if (wantB) { br += xf[n] * yb[n] + xf[n + 1] * yb[n + 1]; bi += xf[n + 1] * yb[n] - xf[n] * yb[n + 1]; }
    }
    e = se; c = cd(cr, ci); b = cd(br, bi);
#endif
}

// Every start position costs the exact metric two turned double-precision products, three prefix sums and two magnitudes, which is most of
// the receiver's work while it searches. This stage sums the products over blocks of kCoarseBlk samples in single precision without turning
// them (the frequency shift moves the phase by 1/32 of a turn over a block), turns each block's sum by the phase at its centre, and evaluates
// the metric once per block, with the C window rounded to 17 blocks (544 samples) and the B window to 15 (480). It reads within a few per
// cent of the exact metric at the same position, and the exact metric changes by about 0.03 at most over the 16 samples to the nearest grid
// point, so a P1 that reaches kP1Threshold in the exact metric shows at least about 0.28 here.
void T2Receiver::Impl::p1Coarse(int64_t lo, int64_t dA, int64_t dB) {
    constexpr int B = kCoarseBlk;
    constexpr int nC = (kP1CLen + B - 1) / B;             // blocks of the C window
    constexpr int b0 = (kP1CLen + kP1ALen + B - 1) / B;   // first block of the B window
    constexpr int nT = kP1Len / B;                        // blocks of the whole P1
    const int64_t K = (dB - dA) / B + 2;                  // grid points: the last one is at or past dB (and its P1 still ends inside the buffer)
    const int64_t nb = K + nT - 1;
    cbC.resize(nb + 1); cbB.resize(nb + 1); cbE.resize(nb + 1);
    cbC[0] = 0; cbB[0] = 0; cbE[0] = 0;
    const cf32* xs = buf.data() + (lo - base);
    for (int64_t j = 0; j < nb; j++) {
        const int64_t i0 = dA + j * B;
        float e; cd c, b;
        blockSums(xs + i0, j < K - 1 + nC, j >= b0, e, c, b);   // the C products for the C windows' blocks, the B products for the B windows'
        const cd ph = phiBlk[(lo + i0) & 1023];
        cbC[j + 1] = cbC[j] + c * ph; cbB[j + 1] = cbB[j] + b * ph; cbE[j + 1] = cbE[j] + e;
    }
    coarse.resize(K);
    coarseFrom = dA;
    for (int64_t k = 0; k < K; k++) {
        const cd sc = cbC[k + nC] - cbC[k], sb = cbB[k + nT] - cbB[k + b0];
        const double en = 0.5 * (cbE[k + nT] - cbE[k]);
        coarse[k] = en > 1e-12 ? (float)((std::abs(sc) + std::abs(sb)) / en) : 0.f;
    }
}

void T2Receiver::Impl::p1Search(int64_t lo, int64_t len, int64_t lastD, int64_t dA, int64_t dB) {
    const int64_t W = kPeakHalfWidth;
    auto exact = [&](int64_t a, int64_t b) {
        p1Metric(lo, len, std::max<int64_t>(0, a - W), std::min(lastD, b + W));
        done.emplace_back(a, b);
        p1Detect(lo, lastD, a, b);
    };
    if (gateOff) { exact(dA, dB); return; }
    // Every position within half a grid step of a grid point that fires is searched exactly (a whole step either side, for margin), together
    // with the metric over the peak test's reach around it, so the candidates, their order and their peak tests are the ones a search of
    // the whole stretch would have had. Stretches closer than the peak test's reach are joined (their metric spans would overlap anyway).
    p1Coarse(lo, dA, dB);
    int64_t sA = -1, sB = -1;
    for (size_t k = 0; k < coarse.size(); k++) {
        if (coarse[k] < kP1PreThreshold) continue;
        const int64_t g = dA + (int64_t)k * kCoarseBlk;
        const int64_t a = std::max(dA, g - kCoarseBlk), b = std::min(dB, g + kCoarseBlk);
        if (a > b) continue;
        if (sA >= 0 && a <= sB + 2 * W) { sB = std::max(sB, b); continue; }
        if (sA >= 0) exact(sA, sB);
        sA = a; sB = b;
    }
    if (sA >= 0) exact(sA, sB);
}

void T2Receiver::Impl::scanP1() {
    const int64_t W = kPeakHalfWidth;
    int64_t e = end();
    int64_t lo = std::max(scanPos, base);
    if (e - lo < kP1Len + 4 * W + 16) return;
    int64_t len = e - lo;
    int64_t lastD = len - kP1Len; // last valid d index (relative)
    int64_t cLo = scanFirst ? 0 : W;
    int64_t cHi = lastD - W;
    if ((int64_t)m.size() != lastD + 1) m.resize(lastD + 1);   // only the searched stretches are written (and recorded in `done`); the rest is never read
    done.clear(); coarse.clear();
    // Once locked, the next P1 is expected one frame after the last one, so only a window around that position is searched (a P1 is
    // 2048 samples in a frame of two million). A window that comes up empty is widened, after three misses the whole range is searched
    // again until a P1 is accepted, and every 20 frames one frame's worth is searched in full to catch a change at the transmitter.
    const bool noTrack = trackDisabled();
    bool tracked = !noTrack && state == 2 && frameLen > 0 && prevPos >= 0 && trackMiss < 3;
    if (tracked && trackFrames >= 20) { trackFrames = 0; trackFullUntil = lo + cHi + (int64_t)frameLen; }
    if (tracked && lo + cLo < trackFullUntil) tracked = false;
    int64_t fullFrom = cLo;   // where a search of the whole range takes over
    if (tracked) {
        // A window that comes up empty means a P1 was lost or the stream jumped (samples dropped): go back to just after the last
        // accepted P1 - those samples are still buffered - and search everything from there, as the plain scanner would have.
        auto rewind = [&]() {
            trackMiss = 3; p1Rescans++;
            scanPos = std::max<int64_t>(trackKeep, base);
            scanFirst = true;
        };
        int64_t from = cLo;
        while (from <= cHi) {
            if (state != 2 || frameLen <= 0 || prevPos < 0) { tracked = false; fullFrom = from; break; }
            if (trackExpect <= 0) trackExpect = prevPos + frameLen;
            const int64_t wLo = (int64_t)std::floor(trackExpect) - kTrackWindow - lo, wHi = (int64_t)std::ceil(trackExpect) + kTrackWindow - lo;
            if (wHi < from) {   // this expected P1 lies behind the scan front: it was not seen
                if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] tracked P1 behind the scan front (expected %.0f): searching everything again\n", trackExpect);
                rewind();
                return;
            }
            if (wLo > cHi) break;   // not reached yet
            const int64_t dA = std::max(from, wLo), dB = std::min(cHi, wHi);
            p1Metric(lo, len, std::max<int64_t>(0, dA - W), std::min(lastD, dB + W));
            done.emplace_back(dA, dB);
            if (p1Detect(lo, lastD, dA, dB)) { from = dB + 1; continue; }   // (trackExpect is updated on acceptance)
            if (wHi <= cHi) {   // the whole window was searched without finding a P1
                if (getenv("DECT2_DEBUG")) { float mx = 0; for (int64_t k = std::max<int64_t>(0, wLo); k <= std::min(lastD, wHi); k++) mx = std::max(mx, m[k]); fprintf(stderr, "  [dbg] no P1 in the window around %.0f (+-%d), best metric %.2f: searching everything again\n", trackExpect, kTrackWindow, mx); }
                rewind();
                return;
            }
            break;   // the window goes on in the next chunk
        }
    }
    if (!tracked && fullFrom <= cHi) p1Search(lo, len, lastD, fullFrom, cHi);
    // decimated trace of the region that becomes final in this pass (the cheap stage's metric where only that ran, zero where the search skipped it)
    for (int64_t d = cLo; d + kTraceDecim <= cHi + 1; d += kTraceDecim) {
        float mx = 0;
        for (const auto& r : done)
            if (r.first <= d + kTraceDecim - 1 && r.second >= d)
                for (int k = 0; k < kTraceDecim; k++) if (d + k >= r.first && d + k <= r.second) mx = std::max(mx, m[d + k]);
        if (!coarse.empty() && d + kTraceDecim > coarseFrom)
            for (int64_t k = std::max<int64_t>(0, (d - coarseFrom + kCoarseBlk - 1) / kCoarseBlk); k < (int64_t)coarse.size() && coarseFrom + k * kCoarseBlk < d + kTraceDecim; k++)
                mx = std::max(mx, coarse[(size_t)k]);
        trace.push_back(mx);
    }
    if (trace.size() > 2048) trace.erase(trace.begin(), trace.begin() + (trace.size() - 2048));
    scanFirst = false;
    scanPos = lo + lastD - 2 * W;
}

T2Receiver::Impl::P1Decode T2Receiver::Impl::decodeP1(int64_t d, double cfoC) {
    P1Decode best;
    const double binHz = fn / 1024.0;
    double hypStep = fn / kP1CLen; // 1/(542 T)
    // The C-A-B correlation gives the offset only modulo 1/(542 T) (16.9 kHz at 8 MHz), so the offsets cfoC + k/(542 T) are tried and the
    // one whose carriers carry the S1/S2 sequences wins. Three of them (+-25 kHz) were enough for a radio tuned to the channel, but not for
    // a recording made with the channel off centre (a Hungarian mux recorded 280 kHz off never showed a P1): the search now covers
    // kP1CfoBins P1 carriers either side, which keeps the 6.9 MHz wide P1 inside the band.
    const int kHyp = (int)std::ceil(kP1CfoBins * (double)kP1CLen / 1024.0);
    for (int k = -kHyp; k <= kHyp; k++) {
        double cfo = cfoC + k * hypStep;
        fr.assign(1024, 0.f); fi.assign(1024, 0.f);
        const cd step = std::polar(1.0, -kTwoPi * cfo / fn);
        cd w(1, 0);
        for (int t = 0; t < 1024; t++) {
            cf32 x = at(d + kP1CLen + t);
            const float c = (float)w.real(), s = (float)w.imag();
            fr[t] = x.real() * c - x.imag() * s;
            fi[t] = x.real() * s + x.imag() * c;
            w *= step;
        }
        doFft(10);
        // differential decode across the active carriers
        std::vector<cf32> y(384), z(384);
        for (int i = 0; i < 384; i++) {
            int f = kP1ActiveCarriers[i] + 86 - 512;
            int b = (f + 1024) % 1024;
            y[i] = cf32(fr[b], fi[b]);
            // undo the transmit randomiser is part of the table sign; see rnd below
        }
        static int rnd[384];
        static bool init = false;
        if (!init) {
            int sr = 0x4e46;
            for (int i = 0; i < 384; i++) {
                int b = (sr ^ (sr >> 1)) & 1;
                rnd[i] = b ? -1 : 1;
                sr >>= 1;
                if (b) sr |= 0x4000;
            }
            init = true;
        }
        for (int i = 0; i < 384; i++) y[i] *= (float)rnd[i];
        z[0] = 0;
        double tot = 0;
        for (int i = 1; i < 384; i++) {
            z[i] = y[i] * std::conj(y[i - 1]);
            tot += std::abs(z[i]);
        }
        if (tot <= 0) continue;
        auto bit = [](const uint8_t* bytes, int i) { return (bytes[i >> 3] >> (7 - (i & 7))) & 1; };
        double bs1 = -1e30, bs2 = -1e30;
        int i1 = 0, i2 = 0;
        for (int s = 0; s < 8; s++) {
            double sc = 0;
            for (int i = 1; i < 64; i++) sc += (1 - 2 * bit(kS1Patterns[s], i)) * z[i].real();
            for (int i = 0; i < 64; i++) sc += (1 - 2 * bit(kS1Patterns[s], i)) * z[320 + i].real();
            if (sc > bs1) { bs1 = sc; i1 = s; }
        }
        for (int s = 0; s < 16; s++) {
            double sc = 0;
            for (int i = 0; i < 256; i++) sc += (1 - 2 * bit(kS2Patterns[s], i)) * z[64 + i].real();
            if (sc > bs2) { bs2 = sc; i2 = s; }
        }
        double conf = (bs1 + bs2) / tot;
        if (conf > best.conf) {
            best.conf = conf;
            best.s1 = i1;
            best.s2 = i2;
            best.cfo = cfo;
            best.z.assign(z.begin(), z.end());
            // sub-sample timing from the phase slope of the (now known) differential bits; carriers with the same spacing to
            // their neighbour share a phasor, so the signed differentials are summed per spacing first
            cd perDf[16] = {};
            for (int i = 1; i < 384; i++) {
                int sg = 1;
                if (i < 64) sg = 1 - 2 * bit(kS1Patterns[i1], i);
                else if (i < 320) sg = 1 - 2 * bit(kS2Patterns[i2], i - 64);
                else sg = 1 - 2 * bit(kS1Patterns[i1], i - 320);
                int df = kP1ActiveCarriers[i] - kP1ActiveCarriers[i - 1];
                if (df < 1 || df >= 16) df = 0;   // (not expected: slot 0 is unused)
                perDf[df] += cd(z[i].real() * sg, z[i].imag() * sg);
            }
            double bestMag = -1, bestDelta = 0;
            for (double delta = -24; delta <= 24; delta += 0.1) {
                cd acc = 0;
                for (int df = 1; df < 16; df++)
                    if (perDf[df] != cd(0, 0)) acc += perDf[df] * std::polar(1.0, -kTwoPi * delta * df / 1024.0);
                if (std::abs(acc) > bestMag) { bestMag = std::abs(acc); bestDelta = delta; }
            }
            best.frac = bestDelta;
        }
    }
    (void)binHz;
    best.ok = best.conf >= kP1MinConf && best.s1 < 5;
    return best;
}

void T2Receiver::Impl::handleP1Candidate(int64_t d, float metric) {
    // CFO from the correlation phases
    cd sc = 0, sb = 0;
    for (int i = 0; i < kP1CLen; i++) {
        cf32 a = at(d + i), b = at(d + i + kP1CLen);
        sc += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag())) * phiTab[(d + i) & 1023];
    }
    for (int i = 0; i < kP1BLen; i++) {
        int64_t g = d + kP1CLen + kP1ALen + i;
        cf32 a = at(g), b = at(g - kP1BLen);
        sb += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag())) * phiTab[g & 1023];
    }
    // arg(sc * exp(+j phi d)) = -2 pi eps 542 T
    cd rot = std::polar(1.0, kTwoPi * (double)(d & 1023) / 1024.0);
    double epsC = -std::arg(sc * rot) * fn / (kTwoPi * kP1CLen);
    P1Decode r = decodeP1(d, epsC);
    if (!r.ok) return;

    // Once the frame length is known, a P1 that does not land where the previous frame predicts is suspicious (a false
    // detection would make a whole garbage frame). Ignore it - unless the next P1 agrees with it, which means the
    // timeline really did jump (e.g. samples were lost), so resynchronise on it.
    if (state == 2 && frameLen > 0 && prevPos >= 0) {
        auto plausible = [&](double from, double at) {
            double dist = at - from, k = std::round(dist / frameLen), err = dist - k * frameLen;
            return k >= 1 && std::fabs(err) <= std::max(300.0, frameLen * 150e-6);
        };
        if (!plausible(prevPos, (double)d)) {
            bool confirmsRejected = haveRejected && plausible(rejectedPos, (double)d);
            if (!confirmsRejected) {
                if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] ignored implausible P1 (%.0f samples from the expected position)\n", (double)d - prevPos - frameLen);
                rejectedPos = (double)d; haveRejected = true;
                lastP1Abs = d;
                return;
            }
            // the timeline jumped: drop what is in flight and start over from this P1
            frames.clear(); frameCells.clear(); p2cells.clear();
            prevPos = -1;
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] timeline jump confirmed, resynchronising\n");
        }
        haveRejected = false;
    }
    lastP1Abs = d;
    P1Info info;
    info.valid = true;
    info.s1 = r.s1;
    info.s2field1 = r.s2 >> 1;
    info.mixed = r.s2 & 1;
    const FftMode* fm = fftModeFromS2(info.s2field1);
    info.fftN = fm ? fm->n : 0;
    info.cfoHz = r.cfo;
    info.conf = (float)r.conf;
    info.metric = metric;
    info.timingFrac = -r.frac;
    info.pos = (uint64_t)d;
    double pos = (double)d - r.frac; // refined start of P1
    p1 = info;
    p1Count++;
    lastP1Seen = end();
    p1Const.assign(r.z.begin() + 1, r.z.end());
    float nrm = 0;
    for (auto& c : p1Const) nrm += std::abs(c);
    nrm = std::max(1e-9f, nrm / p1Const.size());
    for (auto& c : p1Const) c /= nrm;

    if (!fm || info.s1 >= 5 || info.s1 == 2) { // non-T2 or reserved: nothing to demodulate
        return;
    }
    // S2 field 1 has two codes for 8K and for 32K: 1 and 5 with the guard intervals 1/32 - 1/4, 6 and 7 with 1/128, 19/256 and 19/128
    // (EN 302 755 table 18). The pilots, interleaver and tables depend only on the FFT size, so 6 and 7 are taken as 1 and 5: with the raw 7
    // the 6-entry tables were read past their end and a 32K 19/128 or 19/256 mux (UK, Hungary) never got to L1-pre.
    const int code = info.s2field1 == 7 ? 5 : info.s2field1 == 6 ? 1 : info.s2field1;
    bool changed = fm->n != fftN || info.s1 != curS1 || code != fftCode;
    if (changed) {
        fftN = fm->n;
        fftCode = code;
        curS1 = info.s1;
        nP2 = fm->nP2;
        giIdx = -1;
        guard = 0;
        frameLen = 0; frameSyms = 0; sro = 0;
        frames.clear();
        prevPos = -1;
        cfoEst = info.cfoHz;
        prevCells.clear();
        p2cells.clear(); chValid = false;
        kMax = fm->kExt ? fm->kExt : fm->kNormal;
        state = 1;
        giAnchor = (int64_t)std::llround(pos) + kP1Len;
        carriers = fm->kNormal;
    }
    if (getenv("DECT2_DEBUG") && prevPos >= 0 && frameLen > 0) fprintf(stderr, "  [dbg] P1 spacing error %+.1f samples (metric %.2f conf %.2f cfo %+.0f)\n", (pos - prevPos) - frameLen, info.metric, info.conf, info.cfoHz);
    if (state == 0 && !changed && giIdx >= 0 && guard > 0) {
        // lost lock earlier but the parameters are unchanged: resume without re-detecting the guard interval
        state = 2;
        frames.clear(); frameCells.clear(); p2cells.clear();
        prevPos = -1;
        cfoEst = info.cfoHz;
    }
    if (prevPos >= 0) onFrameSpacing(pos - prevPos);
    prevPos = pos;
    trackMiss = 0; trackExpect = pos + frameLen; trackFrames++; trackKeep = (int64_t)pos + 2 * kP1Len;
    if (state == 2 && frameSyms > 0) {
        Frame f;
        f.anchor = (int64_t)std::llround(pos) + kP1Len;
        f.maxSyms = frameSyms;
        frames.push_back(f);
    } else if (state == 2) {
        Frame f;
        f.anchor = (int64_t)std::llround(pos) + kP1Len;
        f.maxSyms = std::max(6, nP2);
        frames.push_back(f);
    }
    if (cfoEst == 0) cfoEst = info.cfoHz;
    // between frames P1 gives a coarse CFO; keep CP-derived value if we already have one
    if (state != 2) cfoEst = info.cfoHz;
}

void T2Receiver::Impl::onFrameSpacing(double L) {
    if (guard <= 0) return;
    int sym = fftN + guard;
    double nSyms = (L - kP1Len) / sym;
    double r = std::round(nSyms);
    double tol = std::max(40.0, L * 80e-6) / sym; // allow +-80 ppm of sample-clock error
    if (r >= 1 && std::fabs(nSyms - r) <= tol) {
        frameSyms = (int)r;
        double nominal = kP1Len + r * sym;
        double meas = (L - nominal) / nominal;
        sro = sro == 0 ? meas : sro + 0.3 * (meas - sro);
        frameLen = nominal;
        frameMsv = nominal / fn * 1e3;
    }
}

bool T2Receiver::Impl::evaluateGi() {
    const int N = fftN;
    const int K = 3;
    int maxG = guardSamples(N, 3);
    if (end() < giAnchor + (int64_t)K * (N + maxG) + N + 16) return false;
    float score[kNumGi];
    for (int g = 0; g < kNumGi; g++) {
        int G = guardSamples(N, g);
        double acc = 0;
        for (int k = 0; k < K; k++) {
            int64_t s = giAnchor + (int64_t)k * (N + G);
            cd c = 0;
            double e = 0;
            for (int n = 0; n < G; n++) {
                cf32 a = at(s + n), b = at(s + n + N);
                c += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
                e += 0.5 * (std::norm(cd(a.real(), a.imag())) + std::norm(cd(b.real(), b.imag())));
            }
            acc += e > 0 ? std::abs(c) / e : 0;
        }
        score[g] = (float)(acc / K);
    }
    int best = 0;
    for (int g = 1; g < kNumGi; g++) if (score[g] > score[best]) best = g;
    float second = 0;
    for (int g = 0; g < kNumGi; g++) if (g != best) second = std::max(second, score[g]);
    std::copy(score, score + kNumGi, giScore);
    giMargin = second > 1e-6f ? score[best] / second : 99.f;
    if (score[best] < 0.25f) { // nothing usable - drop back to searching
        state = 0;
        fftN = 0;
        return true;
    }
    giIdx = best;
    guard = guardSamples(N, giIdx);
    state = 2;
    // process the symbols that follow this first P1 so something is displayed immediately
    Frame f;
    f.anchor = giAnchor;
    f.maxSyms = std::max(6, nP2);
    frames.push_back(f);
    return true;
}

} // namespace dect2
