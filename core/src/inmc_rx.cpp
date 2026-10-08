// Inmarsat-C NCS channel receiver: 1200 symbol/s BPSK, 8.64 s frames.
//
// Chain: mix the channel to 0 Hz, 3-stage CIC decimator to 96 to 190 ksps (r1), carrier search by squaring and an FFT (the squared BPSK signal has
// a line at twice the carrier offset, so +-10 kHz or more is found without a pilot), carrier mixer driven by a Costas loop, root raised cosine
// matched filter that also decimates to about 10 ksps (r2), Gardner timing recovery with a cubic interpolator, soft symbols. Frames are found by
// their 128 unique word symbols (spread over the whole frame, every 162 symbols), which also gives the polarity; then deinterleave, soft Viterbi,
// descramble, packets (inmc_pkt.cpp). Everything runs in feed(): at 2 Msps the cost is a few multiplications per input sample.
#include "dect2/inmc_rx.h"
#include "dect2/fftutil.h"
#include "dect2/inmc_code.h"
#include "dect2/inmc_pkt.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>

namespace dect2 {

namespace {

constexpr int kAcqN = 32768;         // FFT size of the carrier search at r1
constexpr int kUwTolerance = 30;     // wrong unique word symbols (of 128) allowed, as in the reference decoder
constexpr int kRing = 16384;         // symbol history, a power of two above the frame length plus the search window
constexpr double kCarrierBnHz = 15;  // carrier loop noise bandwidth
constexpr double kMaxCfoHz = 20000;  // search range (a radio 10 ppm off at 1.54 GHz is 15 kHz away)

inline int64_t unixNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

namespace {

// One channel: everything from the samples to the messages. The receiver runs one for the channel tuned to and up to three more for the
// channels its search finds in the band.
struct Chan {
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    double ageSec = 0, sinceGood = 0;
    InmcTelemetry tel;
    inmc::FrameParser parser;

    // ---- front end
    int D1 = 1, M = 5;
    double r1 = 0, r2 = 0, spsR1 = 0, spsR2 = 0;
    std::complex<double> nco{1, 0}, ncoStep{1, 0};
    uint64_t ci[3][2] = {};          // CIC integrators (re, im), wrap-around arithmetic
    uint64_t cz[3][2] = {};          // comb delays
    int cicCount = 0;
    float cicScale = 0;
    std::vector<float> taps;         // matched filter at r1
    std::vector<cf32> hist;          // r1 samples after the carrier mixer, circular
    int hpos = 0, mcount = 0;

    // ---- carrier
    bool acquired = false, carrierLock = false;
    double phase = 0;                // mixer phase, rad
    double wSym = 0;                 // carrier frequency in rad per symbol
    double lockEma = 0;
    double sinceLock = 0;            // seconds without lock while acquired
    std::vector<cf32> acq;           // r1 samples for the carrier search
    int acqN = 0;
    // two searches on the same samples: long blocks (sensitive, for weak signals) and short ones (a drifting carrier stays within a bin)
    struct Search {
        int n;
        Fft fft;
        std::vector<float> win, pw;
        int blocks = 0;
        explicit Search(int size) : n(size), fft(size), win((size_t)size), pw((size_t)size, 0.f) {
            for (int i = 0; i < size; i++) win[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * (float)i / (float)size);
        }
        void clear() { std::fill(pw.begin(), pw.end(), 0.f); blocks = 0; }
    };
    Search longSearch{kAcqN}, shortSearch{kAcqN / 4};

    // ---- timing
    cf32 ybuf[32];
    int64_t n2 = 0;                  // r2 samples so far
    double nextPos = 0, ts = 0, tsNom = 0;
    cf32 prevSym{0, 0};
    float agcPow = 1;

    // ---- symbols
    float soft[kRing];
    uint8_t hard[kRing];
    int64_t sc = 0;                  // symbols so far
    double m2 = 1, m4 = 1;

    // ---- frames
    bool tracking = false;
    int64_t lastEnd = 0;
    int misses = 0;
    int bestErr = 999;
    int64_t bestEnd = 0;
    bool bestRev = false;
    int bestSlip = 64;
    double uwAvg = 0;
    double esn0 = 0;
    bool haveSnr = false;
    bool announcedFrameLock = false;
    double cfoHist[8] = {};
    double sinceCfo = 0;
    int cfoN = 0;
    size_t lastMsgCount = 0;
    int consecBad = 0;
    int lockHeld = 0;

    Chan() : acq(kAcqN) {
        clearState();
    }

    void say(const std::string& s) { if (log) log(s); }

    void clearState() {
        ageSec = 0; sinceGood = 0;
        nco = {1, 0};
        std::memset(ci, 0, sizeof ci); std::memset(cz, 0, sizeof cz);
        cicCount = 0;
        hist.assign(taps.size() ? taps.size() : 1, cf32(0, 0));
        hpos = 0; mcount = 0;
        acquired = carrierLock = false;
        phase = 0; wSym = 0; lockEma = 0; sinceLock = 0;
        acqN = 0; longSearch.clear(); shortSearch.clear();
        std::memset(ybuf, 0, sizeof ybuf);
        n2 = 0; ts = tsNom; nextPos = 3 * tsNom; prevSym = {0, 0}; agcPow = 1;
        std::memset(soft, 0, sizeof soft); std::memset(hard, 0, sizeof hard);
        sc = 0; m2 = 1; m4 = 1;
        tracking = false; lastEnd = 0; misses = 0; bestErr = 999;
        consecBad = 0; uwAvg = 0; esn0 = 0; haveSnr = false; announcedFrameLock = false;
        std::memset(cfoHist, 0, sizeof cfoHist); sinceCfo = 0; cfoN = 0;
        parser.reset();
        lastMsgCount = 0;
        const uint64_t s = tel.seq;
        tel = InmcTelemetry();
        tel.seq = s;
    }

    void design() {
        D1 = std::max(1, (int)std::floor(rate / 96000.0));
        r1 = rate / D1;
        M = std::max(1, (int)std::floor(r1 / 9600.0));
        r2 = r1 / M;
        spsR1 = r1 / inmc::kSymbolRate;
        spsR2 = r2 / inmc::kSymbolRate;
        tsNom = spsR2;
        cicScale = (float)(1.0 / (std::pow((double)D1, 3.0) * 8192.0));
        // root raised cosine, roll-off 1, +-3 symbols, designed at r1; unit gain at DC
        const int half = (int)std::ceil(3.0 * spsR1);
        taps.assign((size_t)(2 * half + 1), 0.f);
        double sum = 0;
        for (int i = -half; i <= half; i++) {
            const double t = i / spsR1, a = 1.0;
            double v;
            if (std::fabs(t) < 1e-9) v = 1.0 - a + 4.0 * a / M_PI;
            else if (std::fabs(std::fabs(4.0 * a * t) - 1.0) < 1e-6) v = a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
            else v = (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - (4 * a * t) * (4 * a * t)));
            // a Hann taper keeps the truncated tails small
            v *= 0.5 + 0.5 * std::cos(M_PI * i / (half + 1));
            taps[(size_t)(i + half)] = (float)v;
            sum += v;
        }
        for (auto& x : taps) x = (float)(x / sum);
        setOffset();
    }

    void setOffset() { ncoStep = std::polar(1.0, -2.0 * M_PI * offsetHz / rate); }

    // ---------------------------------------------------------------- input
    void feed(const cf32* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            const size_t blk = std::min<size_t>(n - i, 4096);
            for (size_t k = 0; k < blk; k++) {
                const cf32 s = x[i + k];
                const std::complex<double> m = std::complex<double>(s.real(), s.imag()) * nco;
                nco *= ncoStep;
                // clip and scale to integers for the CIC
                const float fr = std::max(-200000.f, std::min(200000.f, (float)m.real() * 8192.f));
                const float fi = std::max(-200000.f, std::min(200000.f, (float)m.imag() * 8192.f));
                uint64_t v[2] = {(uint64_t)(int64_t)std::lrintf(fr), (uint64_t)(int64_t)std::lrintf(fi)};
                for (int c = 0; c < 2; c++) {
                    ci[0][c] += v[c];
                    ci[1][c] += ci[0][c];
                    ci[2][c] += ci[1][c];
                }
                if (++cicCount == D1) {
                    cicCount = 0;
                    int64_t o[2];
                    for (int c = 0; c < 2; c++) {
                        uint64_t y = ci[2][c];
                        uint64_t d1 = y - cz[0][c]; cz[0][c] = y;
                        uint64_t d2 = d1 - cz[1][c]; cz[1][c] = d1;
                        uint64_t d3 = d2 - cz[2][c]; cz[2][c] = d2;
                        o[c] = (int64_t)d3;
                    }
                    onR1(cf32((float)o[0] * cicScale, (float)o[1] * cicScale));
                }
            }
            const double mag = std::abs(nco);
            nco /= mag;
            i += blk;
        }
    }

    // ---------------------------------------------------------------- r1: carrier search, mixer, matched filter
    void onR1(cf32 x) {
        if (!acquired) {
            acq[(size_t)acqN++] = x;
            if (acqN == kAcqN) { acquire(); acqN = 0; }
        }
        const float c = (float)std::cos(phase), s = (float)std::sin(phase);
        const cf32 y = x * cf32(c, -s);
        phase += wSym / spsR1;
        if (phase > M_PI) phase -= 2 * M_PI; else if (phase < -M_PI) phase += 2 * M_PI;
        hist[(size_t)hpos] = y;
        hpos = hpos + 1 == (int)hist.size() ? 0 : hpos + 1;
        if (++mcount == M) {
            mcount = 0;
            const int L = (int)taps.size();
            float re = 0, im = 0;
            int p = hpos;       // oldest sample
            for (int k = 0; k < L; k++) {
                const cf32 h = hist[(size_t)p];
                re += taps[(size_t)k] * h.real();
                im += taps[(size_t)k] * h.imag();
                p = p + 1 == L ? 0 : p + 1;
            }
            onR2(cf32(re, im));
        }
    }

    // Square, window, FFT, accumulate power; a BPSK carrier shows as a line at twice its offset. The statistic is the sum of three
    // neighbouring bins (a drifting or off-bin line spreads), compared with the same sum in the bins around it: the squared noise has a triangular
    // spectrum (the noise is low-pass filtered by the CIC stage), so a mean over all bins would give false lines near 0 Hz.
    // The thresholds are above the largest ratio seen in 150 to 500 runs of white noise through the same code (see the numbers at each search).
    bool detect(Search& sr, int blocksNow, const int* need, const double* thr, int levels, double& fOut) {
        const int N = sr.n;
        int lvl = -1;
        for (int k = 0; k < levels; k++) if (blocksNow == need[k]) lvl = k;
        if (lvl < 0) return false;
        const double binHz = r1 / N;
        std::vector<float> p3((size_t)N);
        for (int i = 0; i < N; i++) p3[(size_t)i] = sr.pw[(size_t)((i + N - 1) % N)] + sr.pw[(size_t)i] + sr.pw[(size_t)((i + 1) % N)];
        int best = -1;
        float bv = 0;
        for (int i = 0; i < N; i++) {
            const int k = i < N / 2 ? i : i - N;
            if (std::fabs(k * binHz) > 2 * kMaxCfoHz) continue;
            if (p3[(size_t)i] > bv) { bv = p3[(size_t)i]; best = i; }
        }
        if (best < 0) return false;
        double fs = 0;
        int fn = 0;
        for (int d = 12; d <= 400; d++) { fs += p3[(size_t)((best + d) % N)] + p3[(size_t)((best - d + N) % N)]; fn += 2; }
        if (bv < thr[lvl] * (fs / fn)) return false;
        // parabolic interpolation on the log power of the single bins
        const float a = std::log(sr.pw[(size_t)((best + N - 1) % N)] + 1e-20f), b = std::log(sr.pw[(size_t)best] + 1e-20f), c = std::log(sr.pw[(size_t)((best + 1) % N)] + 1e-20f);
        double d = 0;
        const double den = a - 2 * b + c;
        if (den < -1e-9) d = 0.5 * (a - c) / den;
        d = std::max(-1.0, std::min(1.0, d));
        const int k = best < N / 2 ? best : best - N;
        fOut = (k + d) * binHz / 2.0;
        return true;
    }

    void accumulate(Search& sr, const cf32* x, int count) {
        std::vector<cf32> z((size_t)sr.n);
        for (int blk = 0; blk < count; blk++) {
            for (int i = 0; i < sr.n; i++) { const cf32 v = x[blk * sr.n + i]; z[(size_t)i] = v * v * sr.win[(size_t)i]; }
            sr.fft.forward(z.data());
            for (int i = 0; i < sr.n; i++) sr.pw[(size_t)i] += std::norm(z[(size_t)i]);
            sr.blocks++;
        }
    }

    void acquire() {
        accumulate(longSearch, acq.data(), 1);
        accumulate(shortSearch, acq.data(), 4);
        // peak / floor above pure noise: long blocks after 2, 4, 8, 16, 32 blocks 6.9, 4.1, 3.2, 2.5, 1.8; short blocks after 4, 8, 16, 32 blocks 4.0, 3.3, 2.3, 1.9
        static const int needL[] = {2, 4, 8, 16, 32};
        static const double thrL[] = {9.0, 5.3, 3.8, 2.9, 2.2};
        static const int needS[] = {8, 16, 32, 64};
        static const double thrS[] = {4.2, 3.5, 2.6, 2.2};
        double f = 0;
        bool found = detect(longSearch, longSearch.blocks, needL, thrL, 5, f);
        if (!found) found = detect(shortSearch, shortSearch.blocks, needS, thrS, 4, f);
        if (longSearch.blocks >= 48) longSearch.clear();
        if (shortSearch.blocks >= 96) shortSearch.clear();
        if (!found) return;
        wSym = 2.0 * M_PI * f / inmc::kSymbolRate;
        acquired = true; carrierLock = false; lockEma = 0; sinceLock = 0;
        longSearch.clear(); shortSearch.clear();
        char b2[96];
        snprintf(b2, sizeof b2, "Inmarsat-C: carrier found at %+.0f Hz", f);
        say(b2);
    }

    // ---------------------------------------------------------------- r2: timing recovery
    inline cf32 interp(double pos) const {
        const int64_t i0 = (int64_t)std::floor(pos);
        const float mu = (float)(pos - (double)i0);
        const cf32 y0 = ybuf[(i0 - 1) & 31], y1 = ybuf[i0 & 31], y2 = ybuf[(i0 + 1) & 31], y3 = ybuf[(i0 + 2) & 31];
        const float c0 = -mu * (mu - 1) * (mu - 2) / 6, c1 = (mu + 1) * (mu - 1) * (mu - 2) / 2,
                    c2 = -(mu + 1) * mu * (mu - 2) / 2, c3 = (mu + 1) * mu * (mu - 1) / 6;
        return y0 * c0 + y1 * c1 + y2 * c2 + y3 * c3;
    }

    void onR2(cf32 y) {
        // slow gain control to unit mean power
        agcPow += 0.0005f * (std::norm(y) - agcPow);
        const float g = 1.f / std::sqrt(agcPow + 1e-12f);
        ybuf[n2 & 31] = y * g;
        n2++;
        while ((double)(n2 - 1) >= std::floor(nextPos) + 2) {
            const cf32 cur = interp(nextPos);
            const cf32 mid = interp(nextPos - 0.5 * ts);
            const float e = std::real(std::conj(mid) * (cur - prevSym));
            prevSym = cur;
            // the detector needs the carrier removed: until the carrier is found the clock runs free
            const double ee = acquired ? std::max(-2.0, std::min(2.0, (double)e)) : 0.0;
            nextPos += ts - kTimingKp * ee;
            ts -= kTimingKi * ee;
            ts = std::max(tsNom * 0.997, std::min(tsNom * 1.003, ts));
            onSymbol(cur);
        }
    }
    static constexpr double kTimingKp = 0.012, kTimingKi = 0.00002;

    // ---------------------------------------------------------------- symbols: carrier loop, statistics, frame sync
    void onSymbol(cf32 z) {
        const float I = z.real(), Q = z.imag();
        if (acquired) {
            const double bnT = kCarrierBnHz / inmc::kSymbolRate, zeta = 0.707;
            const double th = bnT / (zeta + 1.0 / (4.0 * zeta));
            const double den = 1.0 + 2.0 * zeta * th + th * th;
            const double kp = 4.0 * zeta * th / den, ki = 4.0 * th * th / den;
            const double e = (double)I * Q;
            phase += kp * e;
            wSym += ki * e;
            const double lim = 2.0 * M_PI * (kMaxCfoHz + 500) / inmc::kSymbolRate;
            wSym = std::max(-lim, std::min(lim, wSym));
            const double nrm = (double)I * I + (double)Q * Q + 1e-9;
            lockEma += 0.01 * ((double)(I * I - Q * Q) / nrm - lockEma);
            if (!carrierLock && lockEma > 0.25) {
                carrierLock = true; sinceLock = 0;
                m2 = nrm; m4 = nrm * nrm;
                lockHeld = 0;
            } else if (carrierLock && lockEma < 0.10) {
                carrierLock = false;
                if (lockHeld > 3 * 1200) say("Inmarsat-C: carrier lost");
            }
            if (carrierLock) lockHeld++;
        }
        soft[sc & (kRing - 1)] = I;
        hard[sc & (kRing - 1)] = I > 0 ? 1 : 0;
        sc++;
        const double p2 = (double)I * I + (double)Q * Q;
        m2 += (p2 - m2) / 1024.0;
        m4 += (p2 * p2 - m4) / 1024.0;
        if (carrierLock) {
            const double a = 2 * m2 * m2 - m4;
            const double sig2 = 0.5 * (m2 - std::sqrt(std::max(a, 0.0)));
            const double amp2 = m2 - 2 * sig2;
            if (sig2 > 1e-6 && amp2 > 0) { esn0 = amp2 / (2 * sig2); haveSnr = true; }
        }
        frameSync();
    }

    // wrong unique word symbols (of 128) of the frame ending at end, per row against the normal polarity
    void uwRows(int64_t end, uint8_t d[inmc::kRows]) const {
        const int64_t start = end - (inmc::kFrameSyms - 1);
        for (int r = 0; r < inmc::kRows; r++) {
            const int64_t p = start + (int64_t)r * inmc::kCols;
            const uint8_t u = inmc::kUw[r];
            d[r] = (uint8_t)((hard[p & (kRing - 1)] != u) + (hard[(p + 1) & (kRing - 1)] != u));
        }
    }
    // slipOk: also look for one polarity flip inside the frame. Only while the frame position is known: with a free search the extra
    // freedom would let noise pass as a frame.
    int uwCount(int64_t end, bool& rev, int& slipRow, bool slipOk) const {
        uint8_t d[inmc::kRows];
        uwRows(end, d);
        int e = 0;
        for (int r = 0; r < inmc::kRows; r++) e += d[r];
        slipRow = inmc::kRows;
        if (slipOk) return inmc::uwFit(d, rev, slipRow);
        rev = e > 64;
        return rev ? 128 - e : e;
    }

    void frameSync() {
        if (sc < inmc::kFrameSyms) return;
        const int64_t end = sc - 1;
        int slip = inmc::kRows;
        if (!tracking) {
            if (!carrierLock) return;
            bool rev;
            const int e = uwCount(end, rev, slip, false);
            if (e <= kUwTolerance) {
                tracking = true; misses = 0;
                decodeAt(end, rev, slip, e);
                lastEnd = end;
                bestErr = 999;
            }
            return;
        }
        const int64_t expect = lastEnd + inmc::kFrameSyms;
        if (end < expect - 4) return;
        if (end <= expect + 4) {
            bool rev;
            const int e = uwCount(end, rev, slip, true);
            if (e < bestErr) { bestErr = e; bestEnd = end; bestRev = rev; bestSlip = slip; }
            if (end < expect + 4) return;
        }
        // the window is complete
        if (bestErr <= kUwTolerance) {
            decodeAt(bestEnd, bestRev, bestSlip, bestErr);
            lastEnd = bestEnd; misses = 0;
        } else {
            lastEnd = expect;
            if (++misses >= 3) {
                tracking = false;
                if (announcedFrameLock) say("Inmarsat-C: frame sync lost");
                announcedFrameLock = false;
            }
        }
        bestErr = 999;
    }

    void decodeAt(int64_t end, bool rev, int slipRow, int uwErr) {
        static thread_local float buf[inmc::kFrameSyms];
        const int64_t start = end - (inmc::kFrameSyms - 1);
        for (int i = 0; i < inmc::kFrameSyms; i++) {
            const bool flip = rev != (i / inmc::kCols >= slipRow);
            buf[i] = (flip ? -1.f : 1.f) * soft[(start + i) & (kRing - 1)];
        }
        if (slipRow < inmc::kRows) { tel.polaritySlips++; say("Inmarsat-C: the carrier loop slipped inside a frame, repaired with the unique word"); }
        inmc::FrameDecode d;
        inmc::decodeFrame(buf, d);
        tel.framesFound++;
        tel.uwErrors = uwErr;
        uwAvg = tel.framesFound == 1 ? uwErr : uwAvg + 0.2 * (uwErr - uwAvg);
        tel.symbolErrorRate = (float)d.symbolErrors / inmc::kCodedSyms;
        const bool ok = parser.parseFrame(d.bytes, unixNow());
        if (ok) {
            tel.blocksOk++;
            consecBad = 0;
            tel.frameNumber = parser.lastFrameNo();
            if (!announcedFrameLock) {
                announcedFrameLock = true;
                char b[96];
                snprintf(b, sizeof b, "Inmarsat-C: frame sync, frame %u, %s", tel.frameNumber, rev ? "inverted polarity" : "normal polarity");
                say(b);
            }
        } else {
            tel.blocksBad++;
            if (++consecBad >= 3 && announcedFrameLock) { announcedFrameLock = false; say("Inmarsat-C: frames do not decode"); }
        }
    }

    // ---------------------------------------------------------------- report
    void report(double dt) {
        ageSec += dt;
        if (acquired && !carrierLock) {
            sinceLock += dt;
            if (sinceLock > 3.0) {       // never locked, or lost for good: search again
                acquired = false; wSym = 0; phase = 0; acqN = 0;
                longSearch.clear(); shortSearch.clear();
                tracking = false; announcedFrameLock = false;
            }
        }
        tel.seq++;
        const double cfo = wSym * inmc::kSymbolRate / (2.0 * M_PI);
        tel.cfoHz = acquired ? cfo : 0;
        tel.carrierLock = carrierLock;
        tel.frameLock = tracking && announcedFrameLock;
        tel.uwErrorsAvg = (float)uwAvg;
        tel.snrDb = haveSnr && carrierLock ? (float)(10.0 * std::log10(std::max(esn0, 1e-3))) : 0.f;
        tel.esn0Db = tel.snrDb;
        tel.ebn0Db = tel.snrDb + 3.01f;
        // carrier drift from the frequency one second samples
        sinceCfo += dt;
        if (sinceCfo >= 1.0) {
            sinceCfo -= 1.0;
            for (int k = 7; k > 0; k--) cfoHist[k] = cfoHist[k - 1];
            cfoHist[0] = cfo;
            if (cfoN < 8) cfoN++;
            tel.driftHzS = (carrierLock && cfoN >= 5) ? (cfoHist[0] - cfoHist[4]) / 4.0 : 0.0;
        }
        parser.fill(tel);
        if (tel.messages.size() && tel.messageCount != lastMsgCount) {
            for (const auto& m : tel.messages) {
                if (m.complete && m.seen == 1 && lastMsgCount < tel.messageCount) {
                    char b[160];
                    snprintf(b, sizeof b, "Inmarsat-C: message %u, %s, %d packets", m.id, m.serviceText.c_str(), m.packets);
                    say(b);
                    break;
                }
            }
            lastMsgCount = tel.messageCount;
        }
        const bool fresh = tracking && announcedFrameLock;
        tel.state = fresh ? 2 : (carrierLock ? 1 : 0);
        if (tel.state == 2) sinceGood = 0; else sinceGood += dt;
    }
};

} // namespace


// ---------------------------------------------------------------------------------------------------------------- channel search
namespace {

// Averaged power spectrum of the whole capture, to find narrow signals of the width of an Inmarsat-C channel (2.4 kHz) away from the channel
// the user tuned to. About 100 blocks of 16384 samples a second are taken whatever the sample rate.
struct WideSearch {
    static constexpr int N = 16384;
    static constexpr int kBlocks = 300;
    Fft fft{N};
    std::vector<float> win, pw;
    std::vector<cf32> buf;
    int fill = 0, blocks = 0;
    int64_t skip = 0, gap = 0;
    double rate = 0;
    std::vector<double> prev;          // candidates of the last analysis

    WideSearch() : win(N), pw(N, 0.f), buf(N) {
        for (int i = 0; i < N; i++) win[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * (float)i / (float)N);
    }
    void config(double r) { rate = r; gap = std::max<int64_t>(0, (int64_t)(r / 100.0) - N); clear(); prev.clear(); }
    void clear() { std::fill(pw.begin(), pw.end(), 0.f); blocks = 0; fill = 0; skip = 0; }
    void feed(const cf32* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (skip > 0) { const size_t t = (size_t)std::min<int64_t>((int64_t)(n - i), skip); skip -= (int64_t)t; i += t; continue; }
            const size_t t = std::min<size_t>(n - i, (size_t)(N - fill));
            std::copy(x + i, x + i + t, buf.begin() + fill);
            fill += (int)t; i += t;
            if (fill == N) {
                for (int k = 0; k < N; k++) buf[(size_t)k] *= win[(size_t)k];
                fft.forward(buf.data());
                for (int k = 0; k < N; k++) pw[(size_t)k] += std::norm(buf[(size_t)k]);
                blocks++; fill = 0; skip = gap;
            }
        }
    }
    bool ready() const { return blocks >= kBlocks; }

    // frequencies (Hz from the centre) of narrow lumps at least 2 dB above the floor around them, strongest first; the DC spike of the radio is skipped
    std::vector<double> candidates(double minContrast = 1.6) const {
        std::vector<double> out;
        const double binHz = rate / N;
        const int W = std::max(1, (int)std::lround(2400.0 / binHz));
        std::vector<double> sm((size_t)N);
        // circular moving average over W bins (prefix sums over a tripled array)
        std::vector<double> dbl((size_t)(3 * N + 1), 0.0);
        for (int i = 0; i < 3 * N; i++) dbl[(size_t)i + 1] = dbl[(size_t)i] + pw[(size_t)(i % N)];
        for (int i = 0; i < N; i++) {
            const int lo = i - W / 2 + N, hi = lo + W;
            sm[(size_t)i] = (dbl[(size_t)hi] - dbl[(size_t)lo]) / W;
        }
        struct Cand { double f, c; };
        std::vector<Cand> cs;
        const int Hb = std::min(N / 4, (int)(60000.0 / binHz));
        const int step = std::max(1, W / 2);
        std::vector<double> loc;
        for (int i = 0; i < N; i++) {
            const int k = i < N / 2 ? i : i - N;
            const double f = k * binHz;
            if (std::fabs(f) < 10000.0 || std::fabs(f) > 0.45 * rate) continue;
            bool peak = true;
            for (int d = 1; d <= W && peak; d++)
                if (sm[(size_t)((i + d) % N)] > sm[(size_t)i] || sm[(size_t)((i - d + N) % N)] >= sm[(size_t)i]) peak = false;
            if (!peak) continue;
            loc.clear();
            for (int d = 2 * W; d <= Hb; d += step) { loc.push_back(sm[(size_t)((i + d) % N)]); loc.push_back(sm[(size_t)((i - d + N) % N)]); }
            if (loc.size() < 8) continue;
            std::nth_element(loc.begin(), loc.begin() + (long)loc.size() / 2, loc.end());
            const double med = loc[loc.size() / 2];
            if (med > 0 && sm[(size_t)i] / med > minContrast) cs.push_back({f, sm[(size_t)i] / med});
        }
        std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.c > b.c; });
        for (size_t i = 0; i < cs.size() && i < 6; i++) out.push_back(cs[i].f);
        return out;
    }
};

} // namespace

struct InmcReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    double sinceReport = 0, now = 0;      // seconds of input since the start
    uint64_t seq = 0;
    InmcTelemetry tel;
    std::vector<std::unique_ptr<Chan>> ch;     // ch[0]: the channel tuned to
    std::vector<int> ids;                      // number of each channel, for the log
    int nextId = 1;
    bool searchOn = true;
    WideSearch ws;
    struct Black { double f, until; };
    std::vector<Black> black;

    static constexpr int kMaxExtra = 3;

    void design() {
        ch.clear(); ids.clear(); nextId = 1;
        black.clear();
        ws.config(rate);
        ch.push_back(makeChan(offsetHz, 0));
        ids.push_back(0);
        now = 0; sinceReport = 0;
        tel = InmcTelemetry();
    }

    std::unique_ptr<Chan> makeChan(double off, int id) {
        auto c = std::make_unique<Chan>();
        c->rate = rate;
        c->offsetHz = off;
        c->design();
        c->clearState();
        c->log = [this, id](const std::string& s) {
            if (!log) return;
            if (id == 0) log(s);
            else log("[channel " + std::to_string(id) + "] " + s);
        };
        return c;
    }

    double carrierOf(const Chan& c) const { return c.offsetHz + (c.acquired ? c.wSym * inmc::kSymbolRate / (2.0 * M_PI) : 0.0); }

    void feed(const cf32* x, size_t n) {
        for (auto& c : ch) c->feed(x, n);
        if (searchOn) ws.feed(x, n);
        sinceReport += (double)n;
        const double per = 0.25 * rate;
        while (sinceReport >= per) {
            sinceReport -= per;
            now += 0.25;
            reportAll(0.25);
        }
    }

    void reportAll(double dt) {
        for (auto& c : ch) c->report(dt);
        housekeeping();
        compose();
        if (searchOn && ws.ready()) { search(); ws.clear(); }
    }

    // drop channels that never produced a frame, or that went quiet, and doubles of a channel already decoded
    void housekeeping() {
        for (size_t k = ch.size(); k-- > 1;) {
            Chan& c = *ch[k];
            bool drop = false;
            if (c.ageSec > 70 && c.tel.blocksOk == 0) drop = true;
            else if (c.tel.blocksOk > 0 && c.sinceGood > 100) drop = true;
            if (!drop && c.acquired)
                for (size_t j = 0; j < k && !drop; j++)
                    if (ch[j]->acquired && std::fabs(carrierOf(c) - carrierOf(*ch[j])) < 4000) drop = true;
            if (drop) {
                black.push_back({carrierOf(c), now + 300});
                ch.erase(ch.begin() + (long)k);
                ids.erase(ids.begin() + (long)k);
            }
        }
        black.erase(std::remove_if(black.begin(), black.end(), [this](const Black& b) { return b.until < now; }), black.end());
    }

    void search() {
        const std::vector<double> cands = ws.candidates();
        std::vector<double> keep;
        for (double f : cands) {
            keep.push_back(f);
            bool persistent = false;
            for (double p : ws.prev) if (std::fabs(p - f) < 3000) persistent = true;
            if (!persistent || (int)ch.size() > kMaxExtra) continue;
            if (std::fabs(f - carrierOf(*ch[0])) < 25000.0) continue;
            bool near = false;
            for (size_t k = 1; k < ch.size(); k++) if (std::fabs(f - carrierOf(*ch[k])) < 12000.0 || std::fabs(f - ch[k]->offsetHz) < 12000.0) near = true;
            for (const auto& b : black) if (std::fabs(f - b.f) < 10000.0) near = true;
            if (near) continue;
            const int id = nextId++;
            ch.push_back(makeChan(f, id));
            ids.push_back(id);
            char b[96];
            snprintf(b, sizeof b, "Inmarsat-C: another signal at %+.1f kHz from the centre, trying it as channel %d", f / 1000.0, id);
            if (log) log(b);
        }
        ws.prev = keep;
    }

    void compose() {
        tel = ch[0]->tel;
        tel.seq = ++seq;
        tel.channels.clear();
        std::vector<InmcMessage> all = tel.messages;
        uint32_t count = tel.messageCount;
        for (size_t k = 0; k < ch.size(); k++) {
            const Chan& c = *ch[k];
            if (k > 0 && !(c.tel.state == 2 || c.tel.blocksOk > 0)) continue;
            InmcChannelInfo ci;
            ci.index = ids[k];
            ci.offsetHz = c.offsetHz;
            ci.carrierHz = carrierOf(c);
            ci.state = c.tel.state;
            ci.ebn0Db = c.tel.ebn0Db;
            ci.uwErrorsAvg = c.tel.uwErrorsAvg;
            ci.frameNumber = c.tel.frameNumber;
            ci.framesOk = c.tel.blocksOk; ci.framesBad = c.tel.blocksBad;
            ci.messages = c.tel.messageCount;
            if (c.tel.ncs.valid) {
                ci.sat = c.tel.ncs.sat; ci.lesId = c.tel.ncs.lesId; ci.channelType = c.tel.ncs.channelType;
                ci.region = c.tel.ncs.region; ci.lesName = c.tel.ncs.lesName; ci.channelTypeName = c.tel.ncs.channelTypeName;
            }
            tel.channels.push_back(ci);
            if (k > 0) {
                for (auto m : c.tel.messages) { m.channel = ids[k]; all.push_back(std::move(m)); }
                count += c.tel.messageCount;
            }
        }
        if (ch.size() > 1) {
            inmc::trimMessages(all, 150, 70000);
            tel.messages = std::move(all);
            tel.messageCount = count;
        }
    }
};

InmcReceiver::InmcReceiver() : p_(std::make_unique<Impl>()) {}
InmcReceiver::~InmcReceiver() = default;

void InmcReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    if (inputRateHz >= inmcTuning().minSampleRate - 1) p_->design();
    else { p_->ch.clear(); p_->ids.clear(); }
}
void InmcReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offsetHz = hz;
    if (!p_->ch.empty()) { p_->ch[0]->offsetHz = hz; p_->ch[0]->setOffset(); }
}
void InmcReceiver::setChannelSearch(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->searchOn = on;
    if (!on && p_->ch.size() > 1) { p_->ch.resize(1); p_->ids.resize(1); }
    p_->ws.clear();
}
bool InmcReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= inmcTuning().minSampleRate - 1;
}
void InmcReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->ch.empty()) return;
    const uint64_t s = p_->seq;
    p_->design();
    p_->seq = s;
    p_->tel.seq = s;
}
void InmcReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->ch.empty()) return;
    p_->feed(x, n);
}
bool InmcReceiver::telemetry(InmcTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void InmcReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning inmcTuning() {
    ModeTuning t;
    t.stdMode = 19; t.id = "inmc"; t.name = "Inmarsat-C";
    t.minMhz = 1525; t.maxMhz = 1559; t.defMhz = 1537.1;   // IOR NCS channel (sigidwiki, not checked here)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.005;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;
    return t;
}

} // namespace dect2
