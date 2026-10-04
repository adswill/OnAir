// P1 preamble detection and guard-interval detection of the DVB-T2 receiver.
#include "t2rx_impl.h"

namespace dect2 {

void T2Receiver::Impl::p1Metric(int64_t lo, int64_t len, int64_t d0, int64_t d1) {
    const int64_t n = d1 + kP1Len - d0;   // samples covered: relative indices [d0, d1 + kP1Len)
    pq1.resize(n + 1); pq2.resize(n + 1); pe.resize(n + 1);
    pq1[0] = 0; pq2[0] = 0; pe[0] = 0;
    // every index read is inside the buffer: lo >= base, lo + len == end(), and the offsets are guarded by the conditions below
    const cf32* xs = buf.data() + (lo - base);
    for (int64_t j = 0; j < n; j++) {
        const int64_t i = d0 + j;
        const cf32 x = xs[i];
        cd xc(x.real(), x.imag());
        cd ph = phiTab[(lo + i) & 1023];
        cd q1 = 0, q2 = 0;
        if (i + kP1CLen < len) { cf32 y = xs[i + kP1CLen]; q1 = xc * std::conj(cd(y.real(), y.imag())) * ph; }
        if (i >= kP1BLen) { cf32 y = xs[i - kP1BLen]; q2 = xc * std::conj(cd(y.real(), y.imag())) * ph; }
        pq1[j + 1] = pq1[j] + q1;
        pq2[j + 1] = pq2[j] + q2;
        pe[j + 1] = pe[j] + (double)x.real() * x.real() + (double)x.imag() * x.imag();
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

void T2Receiver::Impl::scanP1() {
    const int64_t W = kPeakHalfWidth;
    int64_t e = end();
    int64_t lo = std::max(scanPos, base);
    if (e - lo < kP1Len + 4 * W + 16) return;
    int64_t len = e - lo;
    int64_t lastD = len - kP1Len; // last valid d index (relative)
    int64_t cLo = scanFirst ? 0 : W;
    int64_t cHi = lastD - W;
    m.assign(lastD + 1, 0.f);
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
            trackMiss = 3;
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
            if (p1Detect(lo, lastD, dA, dB)) { from = dB + 1; continue; }   // (trackExpect is updated on acceptance)
            if (wHi <= cHi) {   // the whole window was searched without finding a P1
                if (getenv("DECT2_DEBUG")) { float mx = 0; for (int64_t k = std::max<int64_t>(0, wLo); k <= std::min(lastD, wHi); k++) mx = std::max(mx, m[k]); fprintf(stderr, "  [dbg] no P1 in the window around %.0f (+-%d), best metric %.2f: searching everything again\n", trackExpect, kTrackWindow, mx); }
                rewind();
                return;
            }
            break;   // the window goes on in the next chunk
        }
    }
    if (!tracked && fullFrom <= cHi) {
        p1Metric(lo, len, std::max<int64_t>(0, fullFrom - W), std::min(lastD, cHi + W));
        p1Detect(lo, lastD, fullFrom, cHi);
    }
    // decimated trace of the region that becomes final in this pass (zero where the search skipped it)
    for (int64_t d = cLo; d + kTraceDecim <= cHi + 1; d += kTraceDecim) {
        float mx = 0;
        for (int k = 0; k < kTraceDecim; k++) mx = std::max(mx, m[d + k]);
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
    for (int k = -1; k <= 1; k++) {
        double cfo = cfoC + k * hypStep;
        fr.assign(1024, 0.f); fi.assign(1024, 0.f);
        for (int t = 0; t < 1024; t++) {
            cf32 x = at(d + kP1CLen + t);
            double a = -kTwoPi * cfo * t / fn;
            float c = (float)std::cos(a), s = (float)std::sin(a);
            fr[t] = x.real() * c - x.imag() * s;
            fi[t] = x.real() * s + x.imag() * c;
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
            // sub-sample timing from the phase slope of the (now known) differential bits
            double bestMag = -1, bestDelta = 0;
            for (double delta = -24; delta <= 24; delta += 0.1) {
                cd acc = 0;
                for (int i = 1; i < 384; i++) {
                    int sg = 1;
                    if (i < 64) sg = 1 - 2 * bit(kS1Patterns[i1], i);
                    else if (i < 320) sg = 1 - 2 * bit(kS2Patterns[i2], i - 64);
                    else sg = 1 - 2 * bit(kS1Patterns[i1], i - 320);
                    double df = kP1ActiveCarriers[i] - kP1ActiveCarriers[i - 1];
                    cd w(z[i].real() * sg, z[i].imag() * sg);
                    acc += w * std::polar(1.0, -kTwoPi * delta * df / 1024.0);
                }
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
    bool changed = fm->n != fftN || info.s1 != curS1 || info.s2field1 != fftCode;
    if (changed) {
        fftN = fm->n;
        fftCode = info.s2field1;
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
