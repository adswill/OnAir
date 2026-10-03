#include "dect2/t2rx.h"
#include "dect2/t2ofdm.h"
#include "dect2/resampler.h"
#include "dect2/t2pilots.h"
#include "dect2/t2l1.h"
#include "dect2/t2interleave.h"

#include "dect2/dsp_compat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace dect2 {

using cd = std::complex<double>;
static constexpr double kTwoPi = 6.283185307179586;

namespace {

constexpr double kP1Threshold = 0.30; // C-A-B correlation needed to try decoding
constexpr double kP1MinConf = 0.40;   // decoded S1/S2 sequence correlation needed to accept
constexpr int kPeakHalfWidth = 700;

struct Frame {
    int64_t anchor;      // absolute index of the first symbol after P1
    int next = 0;        // next symbol index to process
    int maxSyms = 0;
};

} // namespace

struct T2Receiver::Impl {
    // ---- configuration
    double inRate = 0, fn = 0;
    bool decimate = false, rateOk = true;
    RationalResampler resampler;
    std::vector<cf32> rsOut;

    // ---- native-rate buffer
    std::vector<cf32> buf;
    int64_t base = 0; // absolute index of buf[0]
    int64_t end() const { return base + (int64_t)buf.size(); }
    // Samples outside the buffered range read as silence: timing corrections can place a symbol slightly before the oldest retained
    // sample or after the newest one, and that must cost one symbol, not a crash.
    const cf32& at(int64_t abs) const {
        static const cf32 kZero(0.f, 0.f);
        const int64_t i = abs - base;
        if (i >= 0 && i < (int64_t)buf.size()) return buf[(size_t)i];
        return kZero;
    }

    // ---- FFT
    std::vector<float> fr, fi;
    void doFft(int log2n, bool inverse = false) { fftSplit(fr.data(), fi.data(), log2n, inverse); }

    // ---- P1 scanner
    int64_t scanPos = 0;
    bool scanFirst = true;
    std::array<cd, 1024> phiTab; // exp(-j 2 pi g / 1024)
    int64_t lastP1Abs = INT64_MIN / 2;
    double rejectedPos = 0;
    bool haveRejected = false;
    std::vector<float> trace;

    // ---- state
    int state = 0;
    P1Info p1;
    uint64_t p1Count = 0;
    double prevPos = -1;
    int64_t giAnchor = 0;
    int giIdx = -1;
    float giScore[kNumGi] = {};
    float giMargin = 0;
    int fftN = 0, guard = 0, carriers = 0, nP2 = 0;
    int fftCode = -1, curS1 = -1;
    int frameSyms = 0;
    double frameLen = 0, sro = 0, frameMsv = 0;
    std::deque<Frame> frames;
    double cfoEst = 0;
    double cpCorrAvg = 0, timingAvg = 0;
    int lowCount = 0;
    int64_t gridOff = 0, pendingGridOff = 0;
    uint64_t symbols = 0, symCounter = 0;
    std::vector<cf32> prevCells;
    int64_t prevSymAbs = -1;
    int64_t lastP1Seen = 0;
    double frameCfo = 0;   // CFO used for every symbol of the current frame (constant, so phase stays continuous)
    int64_t curAnchor = 0;

    // ---- P2 stage
    int kMax = 0;
    std::vector<std::vector<cf32>> p2cells;
    GridInterpolator interp;
    bool chValid = false, extDetected = false;
    std::vector<cf32> chH;
    int chK = 0, irMin = 0;
    std::vector<float> chMag, chPh, irDb, snrDbv;
    float p2Snr = 0;
    std::vector<cf32> eqP2;
    L1Pre l1pre;
    L1Post l1post;
    std::atomic<int> plpSelect{-1};
    bool l1preOk = false, l1postOk = false;
    bool haveGoodL1 = false;       // the last L1 that passed its CRC, kept to bridge frames whose own L1 fails
    L1Pre goodPre; L1Post goodPost;
    int64_t goodAnchor = 0;
    int l1Reuse = 0;               // consecutive frames that used the kept L1
    uint64_t l1Reused = 0;
    uint64_t l1preGood = 0, l1preBad = 0, l1postGood = 0, l1postBad = 0;
    int l1Iters = 0;
    float l1N0 = 0;
    // data stage
    std::vector<std::vector<cf32>> frameCells;
    bool dataValid = false;
    float dataSnr = 0;
    int dataDx = 0, dataDy = 0, dataPp = 0;
    std::vector<cf32> eqData;
    double chDelayCentre = 0, chDelayHalf = 0; // delay support of the channel (samples rel. to the main path)
    bool chDelayKnown = false;
    std::vector<float> dataSnrCar;
    uint64_t dataFrames = 0;
    // PLP
    PlpDecoder plpDec;
    std::function<void(const PlpResult&)> plpCb;
    std::vector<std::vector<cf32>> p2Sym, p2G2;
    uint64_t frameCounter = 0;
    bool plpValid = false;
    int plpId = 0, plpBlocks = 0, plpSkipped = 0, selectedPlpId = -1;
    PlpFec plpFec;
    uint64_t plpFrames = 0, blocksOk = 0, blocksBad = 0, headersOk = 0, plpBchCorr = 0;
    double plpMer = 0, plpPre = 0, plpIters = 0, plpMs = 0;
    bool plpGpu = false;
    std::deque<std::vector<uint8_t>> blockMaps;
    std::vector<cf32> plpConst;
    std::vector<uint8_t> plpConstErr;
    std::vector<uint16_t> plpConstTx;
    uint64_t plpConstSeq = 0;
    int hUpl = 0, hDfl = 0, hSyncd = 0;

    // ---- outputs
    std::mutex mu;
    RxTelemetry tel;
    std::vector<cf32> p1Const, diffCells, rawCells;

    Impl() {
        for (int i = 0; i < 1024; i++) phiTab[i] = std::polar(1.0, -kTwoPi * i / 1024.0);
    }

    void resetAll() {
        resampler.reset();
        buf.clear();
        base = 0;
        scanPos = 0;
        scanFirst = true;
        lastP1Abs = INT64_MIN / 2;
        trace.clear();
        state = 0;
        p1 = P1Info();
        p1Count = 0;
        prevPos = -1;
        giIdx = -1;
        std::fill(std::begin(giScore), std::end(giScore), 0.f);
        giMargin = 0;
        fftN = guard = carriers = nP2 = 0;
        fftCode = curS1 = -1;
        frameSyms = 0;
        frameLen = sro = frameMsv = 0;
        frames.clear();
        cfoEst = cpCorrAvg = timingAvg = 0;
        lowCount = 0;
        gridOff = 0;
        symbols = symCounter = 0;
        prevCells.clear();
        prevSymAbs = -1;
        p1Const.clear(); diffCells.clear(); rawCells.clear();
        p2cells.clear(); chValid = false; chH.clear(); chMag.clear(); chPh.clear(); irDb.clear(); snrDbv.clear(); eqP2.clear();
        l1pre = L1Pre(); l1post = L1Post(); l1preOk = l1postOk = false; haveGoodL1 = false; l1Reuse = 0; l1preGood = l1preBad = l1postGood = l1postBad = 0;
        frameCells.clear(); dataValid = false; eqData.clear(); dataSnrCar.clear(); dataFrames = 0;
    }

    // ------------------------------------------------------------ P1 detection
    void scanP1() {
        const int64_t W = kPeakHalfWidth;
        int64_t e = end();
        int64_t lo = std::max(scanPos, base);
        if (e - lo < kP1Len + 4 * W + 16) return;
        int64_t len = e - lo;
        int64_t lastD = len - kP1Len; // last valid d index (relative)
        std::vector<cd> pq1(len + 1), pq2(len + 1);
        std::vector<double> pe(len + 1);
        for (int64_t i = 0; i < len; i++) {
            const cf32 x = at(lo + i);
            cd xc(x.real(), x.imag());
            cd ph = phiTab[(lo + i) & 1023];
            cd q1 = 0, q2 = 0;
            if (i + kP1CLen < len) { cf32 y = at(lo + i + kP1CLen); q1 = xc * std::conj(cd(y.real(), y.imag())) * ph; }
            if (i >= kP1BLen) { cf32 y = at(lo + i - kP1BLen); q2 = xc * std::conj(cd(y.real(), y.imag())) * ph; }
            pq1[i + 1] = pq1[i] + q1;
            pq2[i + 1] = pq2[i] + q2;
            pe[i + 1] = pe[i] + (double)x.real() * x.real() + (double)x.imag() * x.imag();
        }
        std::vector<float> m(lastD + 1);
        for (int64_t d = 0; d <= lastD; d++) {
            cd sc = pq1[d + kP1CLen] - pq1[d];
            cd sb = pq2[d + kP1Len] - pq2[d + kP1CLen + kP1ALen];
            double en = 0.5 * (pe[d + kP1Len] - pe[d]);
            m[d] = en > 1e-12 ? (float)((std::abs(sc) + std::abs(sb)) / en) : 0.f;
        }
        int64_t cLo = scanFirst ? 0 : W;
        int64_t cHi = lastD - W;
        // decimated trace of the region that becomes final in this pass
        for (int64_t d = cLo; d + kTraceDecim <= cHi + 1; d += kTraceDecim) {
            float mx = 0;
            for (int k = 0; k < kTraceDecim; k++) mx = std::max(mx, m[d + k]);
            trace.push_back(mx);
        }
        if (trace.size() > 2048) trace.erase(trace.begin(), trace.begin() + (trace.size() - 2048));
        for (int64_t d = cLo; d <= cHi; d++) {
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
        scanFirst = false;
        scanPos = lo + lastD - 2 * W;
    }

    struct P1Decode {
        bool ok = false;
        int s1 = 0, s2 = 0;
        double conf = 0, cfo = 0, frac = 0;
        std::vector<cf32> z;
    };

    P1Decode decodeP1(int64_t d, double cfoC) {
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

    void handleP1Candidate(int64_t d, float metric) {
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

    void onFrameSpacing(double L) {
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

    // ------------------------------------------------------------ guard-interval detection
    bool evaluateGi() {
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

    // ------------------------------------------------------------ OFDM symbols
    void processFrames() {
        const int N = fftN, G = guard;
        while (!frames.empty()) {
            bool stalled = false;
            while (!frames.empty() && frames.front().next < frames.front().maxSyms) {
                Frame& f = frames.front();
                if (f.next == 0) { gridOff += pendingGridOff; pendingGridOff = 0; }
                int64_t s = f.anchor + gridOff + (int64_t)std::llround((double)f.next * (N + G) * (1.0 + sro));
                if (s + N + G + 40 > end()) { stalled = true; break; }
                int idx = f.next++;
                processSymbol(s, idx); // may clear `frames` (loss of lock / guard-interval correction)
            }
            if (stalled || frames.empty()) break;
            frames.pop_front();
        }
    }

    // FFT of the symbol that starts at s (guard included); returns the K central carriers, CFO-corrected.
    void fftCells(int64_t s, std::vector<cf32>& cells, int K) {
        const int N = fftN, G = guard;
        int back = std::min(G / 4, 32);
        int64_t w0 = s + G - back;
        fr.assign(N, 0.f); fi.assign(N, 0.f);
        double dph = -kTwoPi * frameCfo / fn;
        double ph0 = dph * (double)(w0 - curAnchor);
        for (int n = 0; n < N; n++) {
            cf32 x = at(w0 + n);
            double a = ph0 + dph * n;
            float cs = (float)std::cos(a), sn = (float)std::sin(a);
            fr[n] = x.real() * cs - x.imag() * sn;
            fi[n] = x.real() * sn + x.imag() * cs;
        }
        doFft((int)std::lround(std::log2((double)N)));
        cells.resize(K);
        for (int kk = 0; kk < K; kk++) {
            int f = kk - (K - 1) / 2;
            int b = (f + N) % N;
            cells[kk] = cf32(fr[b], fi[b]);
        }
    }

    // Channel estimate from the P2 pilots, equalisation, L1-pre / L1-post decoding.
    bool p2Hypothesis(bool ext, bool final) {
        const FftMode* fm = fftModeFromS2(fftCode);
        const int G = guard, N = fftN;
        PilotConfig pc; pc.fftCode = fftCode; pc.ext = ext; pc.pp = 0;
        PilotMap pm(pc);
        if (!pm.valid()) { pc.pp = 1; pm = PilotMap(pc); }
        if (!pm.valid()) return false;
        const int K = pm.carriers(), off = (kMax - K) / 2;
        const int S = fftCode == 5 ? 6 : 3;
        const int M = (K - 1) / S + 1;
        std::vector<cf32> hg(M, cf32(0, 0));
        std::vector<int> cnt(M, 0);
        std::vector<std::vector<cf32>> obs(nP2, std::vector<cf32>(M, cf32(0, 0)));
        std::vector<uint8_t> types;
        for (int l = 0; l < nP2; l++) {
            pm.symbolTypes(l, nP2 + 2, types);
            for (int n = 0; n < M; n++) {
                int kk = S * n;
                if (types[kk] != kCellP2Pilot) continue;
                cf32 v = p2cells[l][off + kk] / pm.pilot(l, kk, kCellP2Pilot).real();
                obs[l][n] = v;
                hg[n] += v;
                cnt[n]++;
            }
        }
        std::vector<char> have(M);
        for (int n = 0; n < M; n++) { have[n] = cnt[n] > 0; if (have[n]) hg[n] /= (float)cnt[n]; }
        for (int n = 0; n < M; n++) {
            if (have[n]) continue;
            int a = n - 1, b = n + 1;
            while (a >= 0 && !have[a]) a--;
            while (b < M && !have[b]) b++;
            if (a >= 0 && b < M) hg[n] = hg[a] + (hg[b] - hg[a]) * ((float)(n - a) / (float)(b - a));
            else if (a >= 0) hg[n] = hg[a];
            else if (b < M) hg[n] = hg[b];
        }
        const int back = std::min(G / 4, 32);
        std::vector<cf32> H;
        interp.run(hg, S, K, N, (double)back + G / 2.0, H);
        {
            // second pass: narrow the delay passband to where the channel actually has energy
            int tMin, tMax;
            int lim = std::min(N / 2 - 1, G * 3 / 2);
            bool spanOk = !getenv("DECT2_NONARROW") && delaySpan(H, N, -std::min(lim, std::max(G / 2, 64)), lim, back, tMin, tMax);
            if (getenv("DECT2_DEBUG") && spanOk) fprintf(stderr, "  [dbg] delay span %d .. %d samples (G=%d)\n", tMin, tMax, G);
            if (spanOk) {
                double centre = 0.5 * (tMin + tMax), half = 0.5 * (tMax - tMin) + 48;
                double cut = half / ((double)N / (2.0 * S));
                if (cut < 0.9) {
                    interp.run(hg, S, K, N, (double)back + centre, H, cut);
                    chDelayCentre = centre; chDelayHalf = half; chDelayKnown = true;
                } else { chDelayKnown = false; }
            } else chDelayKnown = false;
        }

        if (getenv("DECT2_DEBUG")) {
            std::vector<float> ir;
            impulseResponse(H, N, -N / 4, N / 2, back, ir);
            std::vector<std::pair<float,int>> pk;
            for (int i = 2; i + 2 < (int)ir.size(); i++) if (ir[i] > ir[i-1] && ir[i] >= ir[i+1] && ir[i] > -45) pk.push_back({ir[i], i - N / 4});
            std::sort(pk.rbegin(), pk.rend());
            fprintf(stderr, "  [dbg] P2 IR peaks (delay samples rel. main: dB):");
            for (size_t i = 0; i < pk.size() && i < 10; i++) fprintf(stderr, " %d:%.0f", pk[i].second, pk[i].first);
            fprintf(stderr, "\n");
        }
        // equalised P2 data cells, per symbol in carrier order, then frequency de-interleaved
        const int cP2 = pm.p2DataCells();
        std::vector<std::vector<cf32>> sym(nP2), gsym(nP2);
        std::vector<cf32> eqAll;
        for (int l = 0; l < nP2; l++) {
            pm.symbolTypes(l, nP2 + 2, types);
            std::vector<cf32> rx;
            rx.reserve(cP2);
            for (int kk = 0; kk < K; kk++)
                if (types[kk] == kCellData) rx.push_back(p2cells[l][off + kk] / H[kk]);
            std::vector<int> Hi;
            freqInterleaverSeq(fftCode, cP2, (l & 1) != 0, Hi);
            sym[l].assign(cP2, cf32(0, 0));
            gsym[l].assign(cP2, cf32(0, 0));
            {
                int jj = 0;
                for (int kk = 0; kk < K; kk++)
                    if (types[kk] == kCellData && jj < cP2) { gsym[l][Hi[jj]] = cf32(std::norm(H[kk]), 0); jj++; }
            }
            for (int j = 0; j < cP2 && j < (int)rx.size(); j++) sym[l][Hi[j]] = rx[j];
            eqAll.insert(eqAll.end(), rx.begin(), rx.end());
        }
        std::vector<cf32> stream;
        p2Gather(sym, nP2, cP2, 1840, 0, stream);
        std::vector<cf32> preCells(stream.begin(), stream.begin() + 1840);
        float n0 = 0;
        for (auto& c : preCells) n0 += c.imag() * c.imag();
        n0 = std::max(1e-4f, n0 / 1840.f);
        if (getenv("DECT2_DEBUG")) {
            double mr = 0; for (auto& c : preCells) mr += std::fabs(c.real());
            int neg = 0; for (auto& c : preCells) neg += c.real() < 0;
            fprintf(stderr, "  [dbg] P2 hyp ext=%d: n0 %.4f mean|Re| %.3f neg %d/1840 K %d\n", (int)ext, n0, mr / 1840, neg, K);
        }
        L1Pre pre;
        L1Result r = decodeL1Pre(preCells, n0, pre);
        bool preOk = r.ok;
        l1Iters = r.ldpcIters;
        L1Post post;
        bool postOk = false;
        auto tryPost = [&](const L1Pre& pp, L1Post& out) {
            std::vector<cf32> st2;
            p2Gather(sym, nP2, cP2, 1840, pp.postSize, st2);
            std::vector<cf32> postCells(st2.begin() + 1840, st2.begin() + 1840 + std::min<size_t>(pp.postSize, st2.size() - 1840));
            return decodeL1Post(postCells, n0, pp, nP2, (pp.s2 & 1) != 0, out).ok;
        };
        if (preOk) postOk = tryPost(pre, post);
        if (!preOk && !final) return false;

        // The signalling (L1) hardly ever changes, and it is protected by a CRC that a weak frame sometimes fails: one frame in eight on a
        // marginal signal. Losing the L1 loses the whole 240 ms frame, which is a visible freeze, although its data would decode fine.
        // So when a block fails its CRC, the last good one is reused for a few frames; the data blocks have their own CRCs, which would
        // flag a real change of the configuration.
        bool reusedPre = false, reusedPost = false;
        {
            const int64_t anchorNow = frames.empty() ? 0 : frames.front().anchor;
            auto sameLayout = [](const L1Pre& a, const L1Pre& b) {
                return a.s1 == b.s1 && a.s2 == b.s2 && a.guardInterval == b.guardInterval && a.pilotPattern == b.pilotPattern && a.numDataSyms == b.numDataSyms &&
                       a.postSize == b.postSize && a.postInfoSize == b.postInfoSize && a.l1Mod == b.l1Mod && a.bwtExt == b.bwtExt && a.cellId == b.cellId &&
                       a.networkId == b.networkId && a.systemId == b.systemId;
            };
            if (haveGoodL1 && l1Reuse < 8 && !getenv("DECT2_NOL1REUSE")) {
                if (!preOk) { pre = goodPre; preOk = true; reusedPre = true; postOk = tryPost(pre, post); }
                if (preOk && !postOk && sameLayout(pre, goodPre)) {
                    post = goodPost;
                    const double fl = frameLen > 0 ? frameLen : 1.0;
                    post.frameIdx = (goodPost.frameIdx + (int)std::llround((double)(anchorNow - goodAnchor) / fl)) & 0xFF;
                    post.crcOk = true; postOk = true; reusedPost = true;
                }
            }
            if (preOk && postOk && !reusedPre && !reusedPost) { goodPre = pre; goodPost = post; goodAnchor = anchorNow; haveGoodL1 = true; l1Reuse = 0; }
            else if (reusedPre || reusedPost) { l1Reuse++; l1Reused++; }
        }

        // accept this hypothesis: publish channel data
        extDetected = ext;
        chH = H; chK = K; chValid = true;
        l1preOk = preOk; l1postOk = postOk;
        if (preOk) { l1pre = pre; if (reusedPre) l1preBad++; else l1preGood++; p2Sym = sym; p2G2.assign(nP2, {}); for (int l = 0; l < nP2; l++) { p2G2[l].resize(cP2); for (int j = 0; j < cP2; j++) p2G2[l][j] = gsym[l][j].real(); } } else l1preBad++;
        if (postOk) { l1post = post; if (reusedPost || reusedPre) l1postBad++; else l1postGood++; } else if (preOk) l1postBad++;
        l1N0 = n0;
        snrDbv.clear();
        float sigAll = 0, noiseAll = 0;
        if (nP2 >= 2) {
            std::vector<float> sig(M), nz(M);
            for (int n = 0; n < M; n++) {
                float v = 0, m2 = std::norm(hg[n]);
                int c = 0;
                for (int l = 0; l < nP2; l++) if (have[n]) { v += std::norm(obs[l][n] - hg[n]); c++; }
                sig[n] = m2;
                nz[n] = c > 1 ? v / (float)(c - 1) : 0.f; // per-observation noise variance (sample variance)
            }
            const int W = 12;
            for (int n = 0; n < M; n++) {
                double sa = 0, na = 0;
                for (int j = std::max(0, n - W); j <= std::min(M - 1, n + W); j++) { sa += sig[j]; na += nz[j]; }
                snrDbv.push_back((float)(10 * std::log10(std::max(1e-9, sa) / std::max(1e-12, na))));
            }
            for (int n = 0; n < M; n++) { sigAll += sig[n]; noiseAll += nz[n]; }
            p2Snr = (float)(10 * std::log10(std::max(1e-9f, sigAll) / std::max(1e-12f, noiseAll)));
        } else {
            // single P2 symbol: use the quadrature spread of the BPSK L1-pre cells
            p2Snr = (float)(10 * std::log10(0.5 / (2.0 * n0 > 1e-9 ? 2 * n0 : 1e-9)));
            (void)fm;
        }
        chMag.clear(); chPh.clear();
        int dec = std::max(1, K / 4096);
        for (int i = 0; i < K; i += dec) {
            chMag.push_back(20 * std::log10(std::max(1e-9f, std::abs(chH[i]))));
            chPh.push_back(std::arg(chH[i]));
        }
        irMin = -std::max(8, G / 4);
        impulseResponse(chH, N, irMin, G + std::max(8, G / 4), back, irDb);
        eqP2.clear();
        size_t step = std::max<size_t>(1, eqAll.size() / 4096);
        for (size_t i = 0; i < eqAll.size(); i += step) eqP2.push_back(eqAll[i]);
        if (preOk) {
            if (pre.guardInterval != giIdx && pre.guardInterval < kNumGi) {
                // the blind guard-interval guess was wrong: adopt the signalled value and resynchronise
                giIdx = pre.guardInterval;
                guard = guardSamples(fftN, giIdx);
                frames.clear();
                frameSyms = 0;
                frameLen = 0;
                prevPos = -1;
                dataValid = false;
                return preOk;
            }
            int total = nP2 + pre.numDataSyms;
            if (total != frameSyms) { frameSyms = total; frameMsv = (kP1Len + (double)total * (fftN + guard)) / fn * 1e3; }
            if (!frames.empty()) frames.front().maxSyms = std::max(frames.front().maxSyms, frameSyms);
        }
        return preOk;
    }

    // Integer-bin frequency error: the CP correlation only resolves the offset modulo one carrier spacing, so a wrong
    // P1-based estimate can leave the whole spectrum shifted by whole carriers. Correlate the P2 pilots at shifts.
    int p2IntegerShift() {
        PilotConfig pc; pc.fftCode = fftCode; pc.ext = false; pc.pp = 0;
        PilotMap pm(pc);
        for (int q = 1; !pm.valid() && q < 8; q++) { pc.pp = q; pm = PilotMap(pc); }
        if (!pm.valid() || p2cells.empty() || (int)p2cells[0].size() != kMax) return 0;
        const int K = pm.carriers(), off = (kMax - K) / 2;
        const int S = fftCode == 5 ? 6 : 3;
        std::vector<uint8_t> types;
        pm.symbolTypes(0, nP2 + 2, types);
        double best = -1, base = 0;
        int bestS = 0;
        for (int sh = -6; sh <= 6; sh++) {
            cd acc = 0;
            double pw = 0;
            for (int kk = 0; kk + S < K; kk += S) {
                if (types[kk] != kCellP2Pilot || types[kk + S] != kCellP2Pilot) continue;
                int i0 = off + kk + sh, i1 = off + kk + S + sh;
                if (i0 < 0 || i1 >= kMax) continue;
                const cf32 a = p2cells[0][i0], b = p2cells[0][i1];
                float p0 = pm.pilot(0, kk, kCellP2Pilot).real(), p1 = pm.pilot(0, kk + S, kCellP2Pilot).real();
                cf32 d = b * std::conj(a) * (p0 * p1);
                acc += cd(d.real(), d.imag());
                pw += std::abs(d);
            }
            double sc = pw > 0 ? std::abs(acc) / pw : 0;
            if (sh == 0) base = sc;
            if (sc > best) { best = sc; bestS = sh; }
        }
        if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] P2 integer-bin search: best shift %d score %.3f (shift 0: %.3f)\n", bestS, best, base);
        return (bestS != 0 && best > 0.35 && best > 1.8 * base) ? bestS : 0;
    }

    void runP2Stage() {
        const FftMode* fm = fftModeFromS2(fftCode);
        if (!fm || p2cells.size() != (size_t)nP2) return;
        for (auto& c : p2cells) if ((int)c.size() != kMax) return;
        if (int sh = p2IntegerShift()) {
            double bin = fn / fftN;
            cfoEst += sh * bin;
            frameCfo = cfoEst;
            char b2[120]; snprintf(b2, sizeof b2, "corrected a %+d-carrier frequency offset (CFO now %+.1f Hz)", sh, cfoEst);
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "  [dbg] %s\n", b2);
            frames.clear(); frameCells.clear(); p2cells.clear();
            return;
        }
        bool extPossible = fm->kExt != 0;
        bool guess = false;
        if (extPossible) {
            int off = (kMax - fm->kNormal) / 2;
            double edge = 0, mid = 0;
            int ne = 0, nm = 0;
            for (auto& c : p2cells) {
                for (int i = 0; i < off; i++) { edge += std::norm(c[i]) + std::norm(c[kMax - 1 - i]); ne += 2; }
                for (int i = kMax / 2 - 200; i < kMax / 2 + 200; i++) { mid += std::norm(c[i]); nm++; }
            }
            guess = ne && nm && (edge / ne) > 0.25 * (mid / nm);
        }
        // try the more likely carrier mode first; L1-pre CRC decides
        if (!extPossible) { p2Hypothesis(false, true); return; }
        if (p2Hypothesis(guess, false)) return;
        p2Hypothesis(!guess, false) || p2Hypothesis(guess, true);
    }

    // Assemble the frame's cell stream, cut out the PLP and hand it to the decoder threads.
    void submitPlp(const std::vector<cf32>& dstream, const std::vector<float>& dn0, double sigma2) {
        const int cP2 = (int)p2Sym[0].size();
        const int nPre = 1840, nPost = l1pre.postSize;
        // pick the data PLP (first non-common PLP, else the first)
        int sel = -1;
        const int want = plpSelect.load();
        if (want >= 0) for (size_t i = 0; i < l1post.plps.size(); i++) if (l1post.plps[i].id == want) { sel = (int)i; break; }
        if (sel < 0) { sel = 0; for (size_t i = 0; i < l1post.plps.size(); i++) if (l1post.plps[i].type == 1) { sel = (int)i; break; } }
        if (sel < 0 || sel >= (int)l1post.plps.size()) return;
        const L1PlpConf& pc = l1post.plps[sel];
        if (sel >= (int)l1post.dyn.size()) return;
        const L1PlpDyn& dy = l1post.dyn[sel];
        selectedPlpId = pc.id;
        if (pc.timeIlType != 0 || pc.type == 2 || l1post.subSlices > 1) { plpSkipped++; return; } // inter-frame interleaving and sub-slicing are not implemented
        PlpJob job;
        job.frameNo = ++frameCounter;
        job.t2Frame = l1post.frameIdx;
        job.frameSec = frameMsv / 1e3;
        job.plpId = pc.id;
        job.fec.shortFrame = pc.fecType == 0;
        job.fec.rate = pc.cod;
        job.fec.mod = pc.mod;
        job.fec.rotation = pc.rotation != 0;
        job.numBlocks = dy.numBlocks;
        job.tiBlocks = pc.timeIlLength;
        FecDims d = fecDims(job.fec);
        if (!d.ok || job.numBlocks <= 0) { plpSkipped++; return; }
        std::vector<cf32> stream, g2s;
        p2Gather(p2Sym, nP2, cP2, nPre, nPost, stream);
        {
            std::vector<std::vector<cf32>> gs(nP2);
            for (int l = 0; l < nP2; l++) { gs[l].resize(cP2); for (int j = 0; j < cP2; j++) gs[l][j] = p2G2[l][j]; }
            p2Gather(gs, nP2, cP2, nPre, nPost, g2s);
        }
        std::vector<float> n0s(stream.size());
        for (size_t i = 0; i < stream.size(); i++) n0s[i] = (float)(sigma2 / (2.0 * std::max(1e-12f, g2s[i].real())));
        stream.insert(stream.end(), dstream.begin(), dstream.end());
        n0s.insert(n0s.end(), dn0.begin(), dn0.end());
        const size_t start = (size_t)nPre + nPost + dy.start;
        const size_t need = (size_t)job.numBlocks * d.cellsPerBlock;
        if (start + need > stream.size()) { plpSkipped++; return; }
        job.cells.assign(stream.begin() + start, stream.begin() + start + need);
        job.n0.assign(n0s.begin() + start, n0s.begin() + start + need);
        plpValid = true; plpId = pc.id; plpFec = job.fec; plpBlocks = job.numBlocks;
        plpDec.submit(std::move(job));
    }

    void pollPlp() {
        PlpResult r;
        while (plpDec.poll(r)) {
            plpFrames++;
            blocksOk += r.blocksOk; blocksBad += r.bchFailed; headersOk += r.headerOk; plpBchCorr += r.bchCorrected;
            plpMer = r.blocksOk ? r.merDb : plpMer;
            plpPre = r.preBer; plpIters = r.avgLdpcIters; plpMs = r.decodeMs; plpGpu = r.usedGpu;
            {
                std::vector<uint8_t> bm(r.frames.size(), 0);
                for (size_t i = 0; i < r.frames.size(); i++) bm[i] = r.frames[i].bits.empty() ? 0 : 1;
                blockMaps.push_back(std::move(bm));
                if (blockMaps.size() > 90) blockMaps.pop_front();
            }
            if (!r.constellation.empty()) { plpConst = r.constellation; plpConstErr = r.constErr; plpConstTx = r.constTx; plpConstSeq++; }
            for (auto& f : r.frames) if (f.header.crcOk) { hUpl = f.header.upl; hDfl = f.header.dfl; hSyncd = f.header.syncd; break; }
            if (plpCb) plpCb(r);
        }
    }

    // Scattered-pilot channel estimation for the data / frame-closing symbols of the frame just received.
    void runDataStage() {
        const FftMode* fm = fftModeFromS2(fftCode);
        if (!fm || !l1preOk) return;
        const int L = frameSyms, N = fftN, G = guard;
        for (int l = 0; l < L; l++) if ((int)frameCells[l].size() != kMax) return;
        PilotConfig pc;
        pc.fftCode = fftCode; pc.ext = l1pre.bwtExt != 0; pc.pp = l1pre.pilotPattern; pc.tr = (l1pre.papr & 2) != 0; pc.giIdx = giIdx;
        PilotMap pm(pc);
        if (!pm.valid()) { dataValid = false; return; }
        const int K = pm.carriers(), off = (kMax - K) / 2, dx = pm.dx(), dy = pm.dy();
        const int M = (K - 1) / dx + 1;
        struct Obs { int l; cf32 v; uint8_t type; };
        std::vector<std::vector<Obs>> obs(M);
        std::vector<std::vector<uint8_t>> types(L);
        for (int l = 0; l < L; l++) {
            pm.symbolTypes(l, L, types[l]);
            for (int n = 0; n < M; n++) {
                int k = dx * n;
                uint8_t t = types[l][k];
                if (t != kCellP2Pilot && t != kCellScattered && t != kCellContinual) continue;
                cf32 pv = pm.pilot(l, k, t);
                obs[n].push_back({l, frameCells[l][off + k] / pv.real(), t});
            }
        }
        if (getenv("DECT2_DEBUG")) {
            // which pilot configuration explains the received cells? coherence of consecutive symbols on that config's continual pilots
            for (int e2 = 0; e2 < 2; e2++) for (int ppc = 0; ppc < 8; ppc++) {
                PilotConfig c2 = pc; c2.pp = ppc; c2.ext = e2;
                PilotMap m2(c2);
                if (!m2.valid()) continue;
                int K2 = m2.carriers(), off2 = (kMax - K2) / 2;
                cd tot = 0; double mag = 0; int n2 = 0;
                std::vector<uint8_t> ta, tb;
                for (int l = nP2 + 1; l < std::min(L - 1, nP2 + 12); l++) {
                    m2.symbolTypes(l, L, ta); m2.symbolTypes(l - 1, L, tb);
                    for (int kk = 0; kk < K2; kk++) {
                        if (ta[kk] != kCellContinual || tb[kk] != kCellContinual) continue;
                        cf32 za = frameCells[l][off2 + kk] / m2.pilot(l, kk, kCellContinual).real();
                        cf32 zb = frameCells[l - 1][off2 + kk] / m2.pilot(l - 1, kk, kCellContinual).real();
                        cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                        tot += pr; mag += std::abs(pr); n2++;
                    }
                }
                // scattered pilots, symbols 16 apart / dy apart, same carriers: coherence over dy symbols
                cd tot2 = 0; double mag2 = 0; int n3 = 0;
                int dy2 = m2.dy();
                for (int l = nP2; l + dy2 < std::min(L - 1, nP2 + 3 * dy2); l++) {
                    m2.symbolTypes(l, L, ta); m2.symbolTypes(l + dy2, L, tb);
                    for (int kk = 0; kk < K2; kk++) {
                        if (ta[kk] != kCellScattered || tb[kk] != kCellScattered) continue;
                        cf32 za = frameCells[l][off2 + kk] / m2.pilot(l, kk, kCellScattered).real();
                        cf32 zb = frameCells[l + dy2][off2 + kk] / m2.pilot(l + dy2, kk, kCellScattered).real();
                        cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                        tot2 += pr; mag2 += std::abs(pr); n3++;
                    }
                }
                fprintf(stderr, "  [dbg] ext %d PP%d: continual coherence %.3f (%d)  scattered(dy) coherence %.3f (%d) phase %.2f\n", e2, ppc + 1, mag > 0 ? std::abs(tot) / mag : 0.0, n2, mag2 > 0 ? std::abs(tot2) / mag2 : 0.0, n3, std::arg(tot2));
            }
        }
        // Common phase error and residual timing: compare each symbol with the previous one on the continual pilots.
        // The symbol grid has integer-sample resolution, so the sub-sample timing error shows up as a phase slope
        // across the carriers (up to ~1 rad at the band edge for 32K); it is estimated and removed here.
        {
            double cumA = 0, cumB = 0;
            std::vector<int> cont;
            std::vector<cd> prod;
            std::vector<cf32> prevRaw = frameCells[nP2]; // raw (uncorrected) cells of the previous symbol
            for (int l = nP2 + 1; l < L; l++) {
                std::vector<cf32> curRaw = frameCells[l];
                cont.clear(); prod.clear();
                for (int kk = 0; kk < K; kk++) {
                    if (types[l][kk] != kCellContinual || types[l - 1][kk] != kCellContinual) continue;
                    cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, kCellContinual).real();
                    cf32 zb = prevRaw[off + kk] / pm.pilot(l - 1, kk, kCellContinual).real();
                    cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                    cont.push_back(kk);
                    prod.push_back(pr);
                }
                prevRaw = curRaw;
                if (cont.size() < 4) continue;
                // slope from phase differences of neighbouring pilots
                cd sl = 0;
                double wsum = 0, dsum = 0;
                for (size_t i = 0; i + 1 < cont.size(); i++) {
                    double w = std::min(std::abs(prod[i]), std::abs(prod[i + 1]));
                    sl += prod[i + 1] * std::conj(prod[i]) / std::max(1e-12, std::abs(prod[i]) * std::abs(prod[i + 1])) * w;
                    dsum += w * (cont[i + 1] - cont[i]);
                    wsum += w;
                }
                double beta = (wsum > 0 && dsum > 0) ? std::arg(sl) / (dsum / wsum) : 0.0;
                cd a = 0;
                for (size_t i = 0; i < cont.size(); i++) a += prod[i] * std::polar(1.0, -beta * (cont[i] - (K - 1) / 2.0));
                double alpha = std::arg(a);
                cumA += alpha;
                cumB += beta;
                if (getenv("DECT2_DEBUG") && l < nP2 + 14) fprintf(stderr, "  [dbg] sym %d: alpha %+.3f beta*10k %+.3f (cum %+.2f, %+.2f) pilots %zu\n", l, alpha, beta * 1e4, cumA, cumB * 1e4, cont.size());
                for (int kk = 0; kk < K; kk++) {
                    double ph = -(cumA + cumB * (kk - (K - 1) / 2.0));
                    frameCells[l][off + kk] *= cf32((float)std::cos(ph), (float)std::sin(ph));
                }
            }
            if (getenv("DECT2_DEBUG")) {
                std::vector<uint8_t> ta2, tb2;
                cd tot2 = 0; double mag2 = 0; int n3 = 0;
                for (int l = nP2; l + dy < std::min(L - 1, nP2 + 3 * dy); l++) {
                    for (int kk = 0; kk < K; kk++) {
                        if (types[l][kk] != kCellScattered || types[l + dy][kk] != kCellScattered) continue;
                        cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, kCellScattered).real();
                        cf32 zb = frameCells[l + dy][off + kk] / pm.pilot(l + dy, kk, kCellScattered).real();
                        cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                        tot2 += pr; mag2 += std::abs(pr); n3++;
                    }
                }
                for (int lag : {1, 4, 16}) {
                    cd tc = 0; double mc = 0; int nc = 0;
                    for (int l = nP2; l + lag < std::min(L, nP2 + 40); l++)
                        for (int kk = 0; kk < K; kk++) {
                            if (types[l][kk] != kCellContinual || types[l + lag][kk] != kCellContinual) continue;
                            cf32 za = frameCells[l][off + kk] / pm.pilot(l, kk, kCellContinual).real();
                            cf32 zb = frameCells[l + lag][off + kk] / pm.pilot(l + lag, kk, kCellContinual).real();
                            cd pr = cd(za.real(), za.imag()) * std::conj(cd(zb.real(), zb.imag()));
                            tc += pr; mc += std::abs(pr); nc++;
                        }
                    fprintf(stderr, "  [dbg] continual coherence at lag %d: %.3f (%d)\n", lag, mc > 0 ? std::abs(tc) / mc : 0.0, nc);
                }
                fprintf(stderr, "  [dbg] AFTER correction: scattered(dy) coherence %.3f (%d) phase %.2f\n", mag2 > 0 ? std::abs(tot2) / mag2 : 0.0, n3, std::arg(tot2));
            }
            // rebuild the pilot observations from the phase-corrected cells
            for (int n = 0; n < M; n++) obs[n].clear();
            for (int l = 0; l < L; l++)
                for (int n = 0; n < M; n++) {
                    int k = dx * n;
                    uint8_t t = types[l][k];
                    if (t != kCellP2Pilot && t != kCellScattered && t != kCellContinual) continue;
                    obs[n].push_back({l, frameCells[l][off + k] / pm.pilot(l, k, t).real(), t});
                }
        }
        // per-grid-point noise from scattered-pilot second differences
        std::vector<float> varN(M, 0.f), pw(M, 0.f);
        double sumSig = 0, sumNoise = 0;
        for (int n = 0; n < M; n++) {
            std::vector<const Obs*> sc;
            for (auto& o : obs[n]) if (o.type == kCellScattered) sc.push_back(&o);
            double acc = 0, p = 0;
            int c = 0;
            for (size_t j = 1; j + 1 < sc.size(); j++) {
                double w = (double)(sc[j]->l - sc[j - 1]->l) / (double)(sc[j + 1]->l - sc[j - 1]->l);
                cf32 pred = sc[j - 1]->v + (sc[j + 1]->v - sc[j - 1]->v) * (float)w;
                double e = std::norm(sc[j]->v - pred) / (1.0 + w * w + (1 - w) * (1 - w));
                acc += e;
                p += std::norm(sc[j]->v);
                c++;
            }
            if (c) { varN[n] = (float)(acc / c); pw[n] = (float)(p / c); sumSig += pw[n]; sumNoise += varN[n]; }
        }
        const float amp2 = [&] { cf32 pv = pm.pilot(nP2, 0, kCellScattered); return pv.real() * pv.real(); }();
        const float cpAmp2 = [&] { for (int kk = 0; kk < K; kk++) if (types[nP2][kk] == kCellContinual) { cf32 pv = pm.pilot(nP2, kk, kCellContinual); return pv.real() * pv.real(); } return amp2; }();
        dataSnrCar.clear();
        const int Wd = 10;
        for (int n = 0; n < M; n++) {
            double sa = 0, na = 0;
            for (int j = std::max(0, n - Wd); j <= std::min(M - 1, n + Wd); j++) { sa += pw[j]; na += varN[j]; }
            dataSnrCar.push_back((float)(10 * std::log10(std::max(1e-9, sa) / std::max(1e-12, na * amp2))));
        }
        dataSnr = (float)(10 * std::log10(std::max(1e-9, sumSig) / std::max(1e-12, sumNoise * amp2)));
        if (getenv("DECT2_DEBUG")) {
            int cntN = 0; for (int n = 0; n < M; n++) cntN += varN[n] > 0;
            double ratioMed = 0; std::vector<double> rr; for (int n = 0; n < M; n++) if (varN[n] > 0 && pw[n] > 0) rr.push_back(10 * std::log10(pw[n] / varN[n]));
            std::sort(rr.begin(), rr.end()); if (!rr.empty()) ratioMed = rr[rr.size() / 2];
            fprintf(stderr, "  [dbg] data SNR: grid %d with noise est %d, sumSig %.3g sumNoise %.3g amp2 %.2f median pilot-SNR %.1f dB => %.1f dB\n", M, cntN, sumSig, sumNoise, amp2, ratioMed, dataSnr);
        }

        // channel per data symbol: time-interpolate each grid point, then frequency-interpolate
        std::vector<size_t> ptr(M, 0);
        std::vector<cf32> grid(M), H;
        const int back = std::min(G / 4, 32);
        std::vector<cf32> all;
        const int firstData = nP2;
        // noise power of the raw FFT cells: variance of the pilot observations times the pilot power
        double sigma2 = 0;
        {
            double sn = 0; int cn = 0;
            for (int n = 0; n < M; n++) if (varN[n] > 0) { sn += varN[n]; cn++; }
            sigma2 = cn ? sn / cn * amp2 : 0;
        }
        std::vector<cf32> dstream; std::vector<float> dn0;
        const bool wantPlp = l1postOk && !l1post.plps.empty() && !p2Sym.empty() && sigma2 > 0;
        // Per grid carrier: weighted least-squares fit of H(l) = a + b (l - lbar) over every pilot observation in the frame
        // (P2 symbols excluded: they carry a different phase/timing reference). The slope is shrunk towards zero when it is
        // not significant, which averages the pilot noise over the whole frame for a quasi-static channel.
        std::vector<cf32> fa(M), fb(M);
        std::vector<float> flbar(M, 0.f);
        for (int n = 0; n < M; n++) {
            double sw = 0, sl = 0;
            cd sv = 0;
            int cnt = 0;
            for (auto& o : obs[n]) {
                if (o.type == kCellP2Pilot) continue;
                const double w = (o.type == kCellContinual ? cpAmp2 : amp2);
                sw += w; sl += w * o.l; sv += w * cd(o.v.real(), o.v.imag()); cnt++;
            }
            if (cnt == 0) { fa[n] = n ? fa[n - 1] : cf32(1, 0); fb[n] = 0; continue; }
            const double lbar = sl / sw;
            const cd a0 = sv / sw;
            double stt = 0;
            cd stv = 0;
            for (auto& o : obs[n]) {
                if (o.type == kCellP2Pilot) continue;
                const double w = (o.type == kCellContinual ? cpAmp2 : amp2), t = o.l - lbar;
                stt += w * t * t;
                stv += w * t * cd(o.v.real(), o.v.imag());
            }
            cd b0 = 0;
            if (cnt >= 3 && stt > 1e-9) {
                b0 = stv / stt;
                double snr = stt * std::norm(b0) / std::max(1e-18, (double)varN[n] * amp2 * 0 + 1.0 / 1.0 * 0 + (varN[n] > 0 ? varN[n] * amp2 : 1e-9));
                // James-Stein style shrinkage of the slope: keep it only if it stands out of the noise
                double shrink = std::max(0.0, 1.0 - 1.0 / std::max(1e-9, snr));
                b0 *= shrink;
            }
            fa[n] = cf32((float)a0.real(), (float)a0.imag());
            fb[n] = cf32((float)b0.real(), (float)b0.imag());
            flbar[n] = (float)lbar;
        }
        // Noise power of every data symbol: residual of the pilots against the fitted channel. Interference that comes and goes
        // (a few frames of raised noise, a burst) hits some symbols harder than the frame average that sigma2 describes; scaling the
        // cell noise variance per symbol keeps the LDPC input honest (over-confident wrong bits are far worse than weak ones).
        std::vector<double> symScale(L, 1.0);
        {
            std::vector<double> rp(L, 0.0); std::vector<int> rc(L, 0);
            for (int n = 0; n < M; n++)
                for (auto& o : obs[n]) {
                    if (o.type == kCellP2Pilot || o.l < firstData) continue;
                    const cf32 fit = fa[n] + fb[n] * ((float)o.l - flbar[n]);
                    rp[(size_t)o.l] += std::norm(o.v - fit); rc[(size_t)o.l]++;
                }
            double tot = 0; long cnt = 0;
            for (int l = firstData; l < L; l++) { tot += rp[(size_t)l]; cnt += rc[(size_t)l]; }
            if (cnt > 0 && tot > 0) {
                const double mean = tot / (double)cnt;
                std::vector<double> raw(L, 1.0);
                for (int l = firstData; l < L; l++) raw[(size_t)l] = rc[(size_t)l] ? (rp[(size_t)l] / rc[(size_t)l]) / mean : 1.0;
                for (int l = firstData; l < L; l++) {   // light smoothing over neighbouring symbols, then limits
                    const double a0 = raw[(size_t)std::max(firstData, l - 1)], a1 = raw[(size_t)l], a2 = raw[(size_t)std::min(L - 1, l + 1)];
                    symScale[(size_t)l] = std::min(8.0, std::max(0.25, 0.25 * a0 + 0.5 * a1 + 0.25 * a2));
                }
            }
            if (getenv("DECT2_SYMNOISE")) {
                fprintf(stderr, "SYMNOISE frame %llu:", (unsigned long long)(frameCounter + 1));
                for (int l = firstData; l < L; l++) fprintf(stderr, " %.2f", 10 * std::log10(symScale[(size_t)l]));
                fprintf(stderr, "\n");
            }
        }
        for (int l = firstData; l < L; l++) {
            for (int n = 0; n < M; n++) grid[n] = fa[n] + fb[n] * ((float)l - flbar[n]);
            {
                double tau0 = (double)back + (chDelayKnown ? chDelayCentre : G / 2.0);
                double cut = chDelayKnown ? std::min(1.0, chDelayHalf / ((double)N / (2.0 * dx))) : 1.0;
                interp.run(grid, dx, K, N, tau0, H, cut);
            }
            for (int kk = 0; kk < K; kk++)
                if (types[l][kk] == kCellData) all.push_back(frameCells[l][off + kk] / H[kk]);
            if (wantPlp) {
                int cD = 0;
                for (int kk = 0; kk < K; kk++) cD += types[l][kk] == kCellData;
                std::vector<int> Hi;
                freqInterleaverSeq(fftCode, cD, (l & 1) != 0, Hi);
                std::vector<cf32> orig(cD);
                std::vector<float> on0(cD);
                int jj = 0;
                for (int kk = 0; kk < K; kk++)
                    if (types[l][kk] == kCellData) {
                        cf32 hh = H[kk];
                        float g2 = std::max(1e-12f, std::norm(hh));
                        orig[Hi[jj]] = frameCells[l][off + kk] * std::conj(hh) / g2;
                        on0[Hi[jj]] = (float)(sigma2 * symScale[(size_t)l] / (2.0 * g2));
                        jj++;
                    }
                dstream.insert(dstream.end(), orig.begin(), orig.end());
                dn0.insert(dn0.end(), on0.begin(), on0.end());
            }
            if (l == firstData + (L - firstData) / 2) { chH = H; chK = K; } // keep a mid-frame channel snapshot for display
        }
        if (wantPlp) submitPlp(dstream, dn0, sigma2);
        eqData.clear();
        size_t step = std::max<size_t>(1, all.size() / 6000);
        for (size_t i = 0; i < all.size(); i += step) eqData.push_back(all[i]);
        dataValid = true;
        dataDx = dx; dataDy = dy; dataPp = pc.pp;
        dataFrames++;
        // refresh channel-derived displays from the data-symbol estimate
        chMag.clear(); chPh.clear();
        int dec = std::max(1, K / 4096);
        for (int i = 0; i < K; i += dec) {
            chMag.push_back(20 * std::log10(std::max(1e-9f, std::abs(chH[i]))));
            chPh.push_back(std::arg(chH[i]));
        }
        irMin = -std::max(8, G / 4);
        impulseResponse(chH, N, irMin, G + std::max(8, G / 4), back, irDb);
        snrDbv = dataSnrCar;
        p2Snr = dataSnr;
        (void)fm;
    }

    void processSymbol(int64_t s, int k) {
        const int N = fftN, G = guard;
        if (k == 0) { frameCfo = cfoEst; curAnchor = s; }
        // cyclic-prefix correlation
        auto cpCorr = [&](int64_t st, double& e) {
            cd c = 0;
            e = 0;
            for (int n = 0; n < G; n++) {
                cf32 a = at(st + n), b = at(st + n + N);
                c += cd(a.real(), a.imag()) * std::conj(cd(b.real(), b.imag()));
                e += 0.5 * (std::norm(cd(a.real(), a.imag())) + std::norm(cd(b.real(), b.imag())));
            }
            return c;
        };
        double e;
        cd c = cpCorr(s, e);
        double rho = e > 0 ? std::abs(c) / e : 0;
        cpCorrAvg += 0.1 * (rho - cpCorrAvg);
        if (rho > 0.15) {
            double epsCp = -std::arg(c) * fn / (kTwoPi * N);
            double bin = fn / N;
            double meas = epsCp + std::round((cfoEst - epsCp) / bin) * bin;
            cfoEst += 0.08 * (meas - cfoEst);
            lowCount = 0;
        } else if (++lowCount > 40) {
            state = 0; // lost the signal
            frames.clear();
            lowCount = 0;
            return;
        }
        // timing error from the CP correlation peak
        int D = std::min(8, std::max(2, G / 8));
        double mags[17];
        int best = D;
        for (int dd = -D; dd <= D; dd++) {
            double ee;
            cd cc = cpCorr(s + dd, ee);
            mags[dd + D] = ee > 0 ? std::abs(cc) / ee : 0;
            if (mags[dd + D] > mags[best]) best = dd + D;
        }
        double off = best - D;
        if (best > 0 && best < 2 * D) {
            double a = mags[best - 1], b = mags[best], cc = mags[best + 1];
            double den = a - 2 * b + cc;
            if (den < 0) off += 0.5 * (a - cc) / den;
        }
        timingAvg += 0.05 * (off - timingAvg);
        if (std::fabs(timingAvg) > 6 && rho > 0.3) { pendingGridOff += (int64_t)std::llround(timingAvg); timingAvg = 0; } // applied at the next frame start

        if (k < nP2) {
            if ((int)p2cells.size() != nP2) p2cells.assign(nP2, {});
            fftCells(s, p2cells[k], kMax);
            if (k == nP2 - 1) runP2Stage();
        }
        if (l1preOk && frameSyms > 0 && k < frameSyms && k >= 0) {
            if ((int)frameCells.size() != frameSyms) frameCells.assign(frameSyms, {});
            if (k >= nP2) fftCells(s, frameCells[k], kMax);
            else if (k < (int)p2cells.size()) frameCells[k] = p2cells[k];
            else frameCells[k].clear(); // runP2Stage() just reset the receiver (cleared the P2 cells)
            if (k == frameSyms - 1) runDataStage();
        }
        symbols++;
        symCounter++;
        // display: FFT consecutive symbol pairs, at a limited rate
        int stride = std::max(2, (int)std::lround(fn / (N + G) / 100.0));
        int phase = (int)(symCounter % stride);
        if (phase > 1) return;
        int back = std::min(G / 4, 32);
        int64_t w0 = s + G - back;
        fr.assign(N, 0.f); fi.assign(N, 0.f);
        double ph0 = -kTwoPi * cfoEst * (double)(w0 - s) / fn;
        double dph = -kTwoPi * cfoEst / fn;
        for (int n = 0; n < N; n++) {
            cf32 x = at(w0 + n);
            double a = ph0 + dph * n;
            float cs = (float)std::cos(a), sn = (float)std::sin(a);
            fr[n] = x.real() * cs - x.imag() * sn;
            fi[n] = x.real() * sn + x.imag() * cs;
        }
        doFft((int)std::lround(std::log2((double)N)));
        int K = carriers;
        std::vector<cf32> cells(K);
        double pw = 0;
        for (int kk = 0; kk < K; kk++) {
            int f = kk - (K - 1) / 2;
            int b = (f + N) % N;
            cells[kk] = cf32(fr[b], fi[b]);
            pw += std::norm(cells[kk]);
        }
        float nrm = (float)std::sqrt(std::max(1e-20, pw / K));
        for (auto& cl : cells) cl /= nrm;
        const size_t maxPts = 4096;
        size_t step = std::max<size_t>(1, K / maxPts);
        rawCells.clear();
        for (size_t i = 0; i < (size_t)K; i += step) rawCells.push_back(cells[i]);
        if (phase == 1 && prevCells.size() == (size_t)K) {
            diffCells.clear();
            for (size_t i = 0; i < (size_t)K; i += step) diffCells.push_back(cells[i] * std::conj(prevCells[i]));
        }
        prevCells = std::move(cells);
        (void)k;
    }

    // ------------------------------------------------------------ driver
    void run() {
        for (int guardLoop = 0; guardLoop < 8; guardLoop++) {
            scanP1();
            if (state == 1) evaluateGi();
            if (state == 2) processFrames();
            if (state == 2 && frames.empty() && lastP1Seen > 0 && end() - lastP1Seen > (int64_t)(std::max(frameLen, 0.35 * fn) * 3 + 2 * fn * 0.05)) {
                state = 0; // no P1 for a long while
            }
            // limit memory
            int64_t need = scanPos;
            if (state == 1) need = std::min(need, giAnchor);
            if (!frames.empty()) need = std::min(need, frames.front().anchor);
            int64_t drop = need - base - 4096;
            if (drop > (1 << 20)) {
                buf.erase(buf.begin(), buf.begin() + drop);
                base += drop;
            }
            break;
        }
    }

    void publish() {
        std::lock_guard<std::mutex> lk(mu);
        RxTelemetry& t = tel;
        t.seq++;
        t.rateOk = rateOk;
        t.decimating = decimate;
        t.inputRate = inRate;
        t.nativeRate = fn;
        t.state = state;
        t.p1 = p1;
        t.p1Count = p1Count;
        t.secSinceP1 = lastP1Seen ? (double)(end() - lastP1Seen) / fn : 1e9;
        t.frameMs = frameMsv;
        t.symbolsPerFrame = frameSyms;
        t.sroPpm = sro * 1e6;
        std::copy(giScore, giScore + kNumGi, t.giScore);
        t.giIdx = giIdx;
        t.giMargin = giMargin;
        t.cfoHz = state >= 1 ? cfoEst : p1.cfoHz;
        t.cpCorr = (float)cpCorrAvg;
        double r = std::min(0.999, std::max(0.001, cpCorrAvg));
        t.cpSnrDb = (float)(10 * std::log10(r / (1 - r)));
        t.timingErr = (float)timingAvg;
        t.symbols = symbols;
        t.fftN = fftN;
        t.guard = guard;
        t.carriers = carriers;
        t.p1Trace = trace;
        t.p1Const = p1Const;
        t.cells = diffCells;
        t.rawCells = rawCells;
        t.chValid = chValid;
        t.extCarriers = extDetected;
        t.chCarriers = chK;
        t.chMagDb = chMag;
        t.chPhase = chPh;
        t.chDecim = chK > 0 ? std::max(1, chK / 4096) : 1;
        t.irDb = irDb;
        t.irTauMin = irMin;
        t.snrDb = snrDbv;
        t.snrStep = fftCode == 5 ? 6 : 3;
        t.p2SnrDb = p2Snr;
        t.eqCells = eqP2;
        t.l1preOk = l1preOk; t.l1postOk = l1postOk;
        t.l1pre = l1pre; t.l1post = l1post;
        t.plpSelectedId = selectedPlpId;
        t.plpList.clear(); t.unsupported.clear();
        if (l1postOk) {
            for (size_t i = 0; i < l1post.plps.size(); i++) {
                const L1PlpConf& c = l1post.plps[i];
                RxTelemetry::PlpInfo pi;
                pi.id = c.id; pi.type = c.type; pi.payloadType = c.payloadType; pi.mod = c.mod; pi.cod = c.cod; pi.rotation = c.rotation;
                pi.fecType = c.fecType; pi.tiType = c.timeIlType; pi.tiLength = c.timeIlLength;
                pi.blocks = i < l1post.dyn.size() ? l1post.dyn[i].numBlocks : 0;
                pi.supported = c.timeIlType == 0 && c.type != 2 && l1post.subSlices <= 1;
                t.plpList.push_back(pi);
                char b[160];
                if (c.timeIlType != 0) { snprintf(b, sizeof b, "PLP %d uses inter-frame time interleaving (TIME_IL_TYPE 1), which is not implemented", c.id); t.unsupported.push_back(b); }
                if (c.type == 2 || l1post.subSlices > 1) { snprintf(b, sizeof b, "PLP %d is sub-sliced (type 2 / %d sub-slices per frame), which is not implemented", c.id, l1post.subSlices); t.unsupported.push_back(b); }
            }
        }
        if (l1preOk) {
            if (l1pre.s1 == 1 || l1pre.s1 == 4) t.unsupported.insert(t.unsupported.begin(), "MISO transmission (two transmitters, Alamouti coding) is not implemented");
            if (l1pre.type != 0) t.unsupported.push_back("the multiplex carries generic streams (GSE/GS), not an MPEG transport stream");
        }
        if (l1postOk && l1post.fefLength > 0) t.unsupported.push_back("the signal contains FEF (future extension) frames; they are skipped");
        t.l1preGood = l1preGood; t.l1preBad = l1preBad; t.l1postGood = l1postGood; t.l1postBad = l1postBad;
        t.l1Iters = l1Iters;
        t.dataValid = dataValid;
        t.dataPp = dataPp; t.dataDx = dataDx; t.dataDy = dataDy;
        t.dataSnrDb = dataSnr;
        t.eqData = eqData;
        t.dataSnrCarrier = dataSnrCar;
        t.dataFrames = dataFrames;
        pollPlp();
        t.plpValid = plpValid; t.plpId = plpId; t.plpFec = plpFec; t.plpBlocks = plpBlocks;
        t.plpFrames = plpFrames; t.plpFramesDropped = plpDec.dropped();
        t.blocksOk = blocksOk; t.blocksBad = blocksBad; t.headersOk = headersOk; t.plpBchCorrected = plpBchCorr;
        t.plpMerDb = plpMer; t.plpPreBer = plpPre; t.plpIters = plpIters; t.plpDecodeMs = plpMs; t.plpOnGpu = plpGpu; t.gpuAvailable = plpDec.gpuAvailable(); t.computeMode = plpDec.mode();
        t.plpConst = plpConst; t.plpConstErr = plpConstErr; t.plpConstTx = plpConstTx; t.plpConstSeq = plpConstSeq;
        t.blockMap.assign(blockMaps.begin(), blockMaps.end());
        t.plpHeaderUpl = hUpl; t.plpHeaderDfl = hDfl; t.plpHeaderSyncd = hSyncd; t.plpSkipped = plpSkipped;
        if (dataValid) t.snrStep = dataDx;
    }
};

T2Receiver::T2Receiver() : p_(new Impl) {}
T2Receiver::~T2Receiver() = default;

void T2Receiver::configure(double inputRateHz, double bandwidthMhz) {
    Impl& I = *p_;
    std::lock_guard<std::mutex> lk(I.mu);
    I.inRate = inputRateHz;
    I.fn = nativeRateHz(bandwidthMhz);
    I.rateOk = I.resampler.configure(inputRateHz, I.fn) && inputRateHz >= 7.9e6 * (bandwidthMhz / 8.0);
    I.decimate = I.rateOk && !I.resampler.passthrough();
    I.resetAll();
}

void T2Receiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetAll();
}

void T2Receiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (!I.rateOk || I.fn <= 0) return;
    if (I.decimate) { I.rsOut.clear(); I.resampler.process(x, n, I.rsOut); I.buf.insert(I.buf.end(), I.rsOut.begin(), I.rsOut.end()); }
    else I.buf.insert(I.buf.end(), x, x + n);
    I.run();
}

void T2Receiver::selectPlp(int id) { p_->plpSelect = id; }
void T2Receiver::setComputeMode(int m) { p_->plpDec.setMode(m); }
void T2Receiver::setPlpCallback(std::function<void(const PlpResult&)> cb) { p_->plpCb = std::move(cb); }

bool T2Receiver::telemetry(RxTelemetry& out, uint64_t lastSeq) {
    Impl& I = *p_;
    I.publish();
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.tel.seq <= lastSeq) return false;
    out = I.tel;
    return true;
}

} // namespace dect2
