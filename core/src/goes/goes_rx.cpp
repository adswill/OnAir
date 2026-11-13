// GOES HRIT receiver (goes_rx.h).
#include "dect2/common/bits.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/dsp/rrc.h"
#include "dect2/results/results_io.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/goes/goes_proto.h"
#include "dect2/goes/goes_tel.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/hrpt/hrpt_proto.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kRolloff = 0.5;
constexpr double kGoesSymbolRate = 927000.0;
constexpr int kFrameSyms = 2 * 8 * meteor::kCaduBytes;    // 16384 code symbols a frame
constexpr int kSyncFrames = 3;                             // frames the sync search looks over
constexpr int kAcqFft = 4096, kAcqBlocks = 8, kAcqMaxBlocks = 64;

using dect2::dsp::rrc;

// complex FIR with the history kept twice over, so every output is one contiguous dot product
struct Fir {
    std::vector<float> h;
    std::vector<cf32> buf;
    size_t pos = 0;
    void set(std::vector<float> taps) { h = std::move(taps); buf.assign(2 * h.size(), cf32(0, 0)); pos = 0; }
    void push(cf32 x) { buf[pos] = x; buf[pos + h.size()] = x; if (++pos == h.size()) pos = 0; }   // no modulo per sample (4.1)
    cf32 out() const {
        const cf32* b = buf.data() + pos;   // oldest first
        float re = 0, im = 0;
        const size_t n = h.size();
        for (size_t i = 0; i < n; i++) { re += h[n - 1 - i] * b[i].real(); im += h[n - 1 - i] * b[i].imag(); }
        return cf32(re, im);
    }
    void clear() { std::fill(buf.begin(), buf.end(), cf32(0, 0)); pos = 0; }
};

inline cf32 cubic(const cf32* y, double t) {
    const int i = (int)std::floor(t);
    const float f = (float)(t - i);
    const cf32 p0 = y[i - 1], p1 = y[i], p2 = y[i + 1], p3 = y[i + 2];
    // Catmull-Rom
    const cf32 a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const cf32 b = p0 - 2.5f * p1 + 2.f * p2 - 0.5f * p3;
    const cf32 c = -0.5f * p0 + 0.5f * p2;
    return ((a * f + b) * f + c) * f + p1;
}

void loopGains(double bnT, double zeta, double& alpha, double& beta) {
    const double th = bnT / (zeta + 1 / (4 * zeta)), d = 1 + 2 * zeta * th + th * th;
    alpha = 4 * zeta * th / d;
    beta = 4 * th * th / d;
}
} // namespace

struct GoesReceiver::Impl {
    std::mutex mu;
    GoesTelemetry pub;
    std::function<void(const std::string&)> logCb;
    std::function<void(const std::vector<uint8_t>&, int, int)> fileCb;
    std::function<void(const uint8_t*)> caduCb;     // every good frame as a 1024 byte CADU (mu)
    std::deque<std::vector<uint8_t>> caduIn;        // CADU files waiting to be decoded by the feed thread (mu)
    bool rsDual = true;                             // the Reed-Solomon symbol basis that last decoded (CCSDS dual basis, or conventional)
    uint64_t rsSwitches = 0;
    std::atomic<bool> resetReq{false}, clearReq{false};
    bool loadReq = false;                // loaded products (mu): the feed thread hands them to the assembler
    std::vector<std::shared_ptr<goes::Product>> loadIn;
    std::atomic<double> offset{0};
    double rateSet = 0;
    double symbolRate = kGoesSymbolRate;

    // front end
    double rate = 0, fs2 = 0, sps = 0;
    int D = 1, decPhase = 0;
    Fir lpf, mf;
    double appliedOffset = 0;
    double cfo = 0;                       // coarse carrier offset (Hz) the mixer takes out
    std::complex<double> nco{1, 0}, rot{1, 0};
    int ncoCount = 0;
    // timing
    std::vector<cf32> y;
    double pos = 0, spsEst = 0;
    cf32 yprev{0, 0};
    float agc = 1;
    double tAlpha = 0, tBeta = 0;
    // carrier
    double cPhase = 0, cFreq = 0, cAlpha = 0, cBeta = 0;
    float lockM = 0, meanAbs = 0.5f, meanIm2 = 0.1f;
    bool acq = true, carrierLock = false;
    std::vector<cf32> acqBuf;
    std::vector<float> acqSpec;
    int acqBlocks = 0;
    int64_t sinceAcq = 0, lowLock = 0;
    uint64_t okAtAcq = 0;
    std::vector<double> rejected;         // carrier offsets tried that did not lock (a DC spike, a symbol rate line): skipped next time
    std::unique_ptr<Fft> fft;
    // symbols -> code
    int64_t symIdx = 0;
    std::vector<float> syncBuf;           // soft symbols (> 0 = code bit 1) for the sync search
    int64_t syncBase = 0;                 // symbol index of syncBuf[0]
    int64_t nextSearch = 0;
    bool symSync = false, g2inv = true;
    int alignParity = 0;
    float firstOfPair = 0;
    hrpt::Viterbi vit;                    // fast 16 bit butterflies: 3 Msym/s QPSK (GK-2A HRIT) needs it
    bool oqpsk = false;                   // Elektro HRIT 1.15 Msym/s: the Q rail starts half a symbol later (SatDump pipeline elektro_hrit); Q is taken half a symbol after I, after the carrier loop (timing())
    bool qpsk = false;                    // GK-2A HRIT: QPSK, G1 on I and G2 on Q; the receiver turns the serial order and polarity up in the sync search
    bool swapIQ = false;
    std::vector<float> syncBuf2;          // the same symbols with I and Q swapped (QPSK: a quarter turn swaps them)
    float meanErr2 = 0.1f;
    std::string tag = "GOES HRIT";
    int dialect = 0;
    // coarse location of the signal in a capture much wider than it (decimation > 1): an averaged power spectrum of the raw samples
    bool coarseDone = false, useCoarse = false;
    int nWide = 0, wideBlocks = 0;
    std::unique_ptr<Fft> fftWide;
    std::vector<cf32> wideBuf;
    std::vector<float> widePsd;
    std::vector<double> coarseRej;        // absolute offsets (Hz) the coarse search sent the fine search to and that did not lock
    double coarseAbs = 0;
    uint64_t asmPattern = 0, asmPatternNrz = 0;   // the sync word through the convolutional encoder, as sent plainly / NRZ-M coded
    bool nrzm = false;                    // the frame bits are differentially coded (GOES-19 HRIT): the decoded bit is the change of the code bit
    uint8_t nrzPrev = 0;
    // bits -> frames
    int fstate = 0;                       // 0 search, 1 collect, 2 check the sync word
    uint32_t reg = 0;
    int regBits = 0, collected = 0, misses = 0;
    bool invBits = false;
    uint8_t frame[meteor::kCodedBytes];
    int64_t lastGoodSym = 0;
    goes::Assembler asmb;
    // counters
    uint64_t frames = 0, ok = 0, bad = 0, corrected = 0;
    int64_t nIn = 0;
    double nextReport = 0;
    double levelAcc = 0;
    int64_t levelN = 0;
    std::deque<cf32> constel;
    std::deque<float> snrH, errH, rateH;
    uint64_t lastOk = 0, lastCorr = 0, lastFrames = 0;
    std::deque<std::string> logLines;
    bool frameLockReported = false, carrierReported = false;
    uint64_t seq = 0;

    Impl() {
        asmPattern = meteor::encodedAsm();
        {
            meteor::ConvEncoder e;
            uint8_t x = 0;
            for (int i = 31; i >= 0; i--) { uint8_t a, b; x ^= (uint8_t)(meteor::kAsm >> i & 1); e.push(x, a, b); asmPatternNrz = asmPatternNrz << 2 | (uint64_t)a << 1 | b; }
        }
        asmb.onLog = [this](const std::string& s) { addLog(s, false); };
        asmb.onFile = [this](const std::vector<uint8_t>& f, int vc, int ap) {
            std::function<void(const std::vector<uint8_t>&, int, int)> cb;
            { std::lock_guard<std::mutex> lk(mu); cb = fileCb; }
            if (cb) cb(f, vc, ap);
        };
    }

    void addLog(const std::string& s, bool engine) {
        logLines.push_front(s);
        while (logLines.size() > kGoesLogMax) logLines.pop_back();
        if (engine) {
            std::function<void(const std::string&)> cb;
            { std::lock_guard<std::mutex> lk(mu); cb = logCb; }
            if (cb) cb(s);
        }
    }

    void setup(double r) {
        rate = r;
        useCoarse = r > 3.0 * symbolRate;      // the signal leaves room in the capture: locate it first
        D = std::max(1, (int)std::floor(r / (2.3 * symbolRate)));
        fs2 = r / D;
        sps = fs2 / symbolRate;
        if (D > 1) {
            // pass the whole root raised cosine band (0.75 of the symbol rate, flat), stop what would alias onto it after the decimation
            // (fs2 - 0.75 Rs): the middle of that gap is the cutoff, the gap sets the length
            const double passE = 0.8 * symbolRate, stopE = fs2 - 0.8 * symbolRate;
            const int n = 2 * (int)std::ceil(1.8 * r / std::max(1.0, stopE - passE)) * 1 + 1 + 4 * D;
            std::vector<float> h((size_t)n);
            const double fc = 0.5 * (passE + stopE) / r;
            double sum = 0;
            for (int i = 0; i < n; i++) {
                const double m = i - (n - 1) / 2.0, w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));   // Blackman
                h[(size_t)i] = (float)(w * (std::fabs(m) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m)));
                sum += h[(size_t)i];
            }
            for (auto& v : h) v = (float)(v / sum);
            lpf.set(h);
        }
        const int half = (int)std::ceil(6 * sps);
        std::vector<float> h((size_t)(2 * half + 1));
        double e = 0;
        for (int i = -half; i <= half; i++) { h[(size_t)(i + half)] = (float)rrc(i / sps, kRolloff); e += h[(size_t)(i + half)] * h[(size_t)(i + half)]; }
        for (auto& v : h) v = (float)(v / std::sqrt(e));
        mf.set(h);
        fft = std::make_unique<Fft>(kAcqFft);
        loopGains(0.004, 0.707, tAlpha, tBeta);
        resetState();
    }

    void setRot() {
        const double w = -2 * kPi * (appliedOffset + cfo) / rate;
        rot = std::complex<double>(std::cos(w), std::sin(w));
    }

    void resetCarrier() { cPhase = 0; cFreq = 0; lockM = 0; double a, b; loopGains(0.01, 0.707, a, b); cAlpha = a; cBeta = b; }

    void resetFec() {
        symSync = false; vit.reset(); vit.out().clear();
        fstate = 0; reg = 0; regBits = 0; collected = 0; misses = 0;
        syncBuf.clear(); syncBuf2.clear(); syncBase = symIdx; nextSearch = symIdx + (int64_t)kSyncFrames * kFrameSyms + 64;
    }

    void resetState() {
        cfo = 0; appliedOffset = offset.load(); setRot(); nco = 1;
        lpf.clear(); mf.clear(); decPhase = 0;
        y.clear(); pos = 2 + sps; spsEst = sps; yprev = 0; agc = 1;
        resetCarrier();
        rejected.clear(); coarseRej.clear(); coarseDone = false; nWide = 0; wideBuf.clear();
        acq = true; carrierLock = false; acqBuf.clear(); acqSpec.assign(kAcqFft, 0.f); acqBlocks = 0; sinceAcq = 0; lowLock = 0;
        meanAbs = 0.5f; meanIm2 = 0.1f; meanErr2 = 0.1f; swapIQ = false;
        resetFec();
        asmb.reset();
        frameLockReported = carrierReported = false;
    }

    // ---- coarse location: the strongest band as wide as the signal in the averaged spectrum of the raw samples (energy detection: a weak
    // signal shows in the average long before the squared signal's line does), spurs clipped; the mixer is moved onto it and the fine search
    // below finishes the job
    void coarseFeed(cf32 v) {
        if (nWide == 0) {
            nWide = 4096;
            while (nWide < 65536 && rate / nWide > 0.12 * symbolRate) nWide *= 2;   // bins of at most an eighth of the symbol rate
            fftWide = std::make_unique<Fft>(nWide);
            widePsd.assign((size_t)nWide, 0.f);
            wideBlocks = 0;
        }
        wideBuf.push_back(v);
        if ((int)wideBuf.size() < nWide) return;
        for (int i = 0; i < nWide; i++) wideBuf[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / nWide));
        fftWide->forward(wideBuf.data());
        for (int i = 0; i < nWide; i++) widePsd[(size_t)i] += std::norm(wideBuf[(size_t)i]);
        wideBuf.clear();
        wideBlocks++;
        const double binHz = rate / nWide;
        // enough blocks that the average over the window is good to under 1 % (blocks x window bins >= 20000), at most a quarter second
        if ((double)wideBlocks * std::max(3.0, std::ceil(1.5 * symbolRate / binHz)) < 20000 && (double)wideBlocks * nWide < 0.25 * rate && wideBlocks < 400) return;
        std::vector<float> srt(widePsd);
        std::nth_element(srt.begin(), srt.begin() + nWide / 2, srt.end());
        const double med = std::max(1e-30f, srt[(size_t)nWide / 2]);
        const int W = std::max(3, (int)std::ceil(1.5 * symbolRate / binHz));
        std::vector<double> c((size_t)nWide + 1, 0.0);
        for (int i = 0; i < nWide; i++) c[(size_t)i + 1] = c[(size_t)i] + std::min((double)widePsd[(size_t)i], 4 * med);   // a spur counts as at most 4 x the floor
        double best = 0; int bi = -1;
        for (int i = 0; i < nWide; i++) {
            double sum = 0;
            const int e = i + W;
            sum = e <= nWide ? c[(size_t)e] - c[(size_t)i] : (c[(size_t)nWide] - c[(size_t)i]) + c[(size_t)(e - nWide)];
            const double ctr = i + W / 2.0, f = (ctr < nWide / 2.0 ? ctr : ctr - nWide) * binHz;
            bool skip = false;
            for (double r : coarseRej) skip |= std::fabs(appliedOffset + cfo + f - r) < 0.4 * symbolRate;
            if (!skip && sum > best) { best = sum; bi = i; }
        }
        coarseDone = true;
        const double ratio = best / (W * med);
        if (bi >= 0 && ratio > std::max(1.12, 1.0 + 6.0 / std::sqrt((double)wideBlocks * W))) {
            const double ctr = bi + W / 2.0, f = (ctr < nWide / 2.0 ? ctr : ctr - nWide) * binHz;
            if (std::fabs(f) > 0.15 * symbolRate) {            // only a signal clearly off the centre of the band needs the mixer moved
                cfo += f;
                setRot();
                lpf.clear(); mf.clear(); decPhase = 0; y.clear(); pos = 2 + sps; yprev = 0; agc = 1;
                acqBuf.clear(); std::fill(acqSpec.begin(), acqSpec.end(), 0.f); acqBlocks = 0;
            }
            coarseAbs = appliedOffset + cfo;
        } else coarseAbs = 1e30;
        std::fill(widePsd.begin(), widePsd.end(), 0.f);
        wideBlocks = 0;
    }

    // ---- coarse carrier from the squared signal
    void acquire(cf32 v) {
        {
            const cf32 v2 = v * v;
            acqBuf.push_back(qpsk ? v2 * v2 : v2);          // BPSK: the squared signal has a line at twice the offset; QPSK: the fourth power at four times
        }
        if ((int)acqBuf.size() < kAcqFft) return;
        // Hann window: the radio's DC spike (a line in the squared signal too) must not leak into the bins of a weak real line
        for (int i = 0; i < kAcqFft; i++) acqBuf[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kAcqFft));
        fft->forward(acqBuf.data());
        for (int i = 0; i < kAcqFft; i++) acqSpec[(size_t)i] += std::norm(acqBuf[(size_t)i]);
        acqBuf.clear();
        if (++acqBlocks < kAcqBlocks || (acqBlocks & (kAcqBlocks - 1)) != 0) return;   // look after 8, 16, 32 and 64 blocks: a weak line gets longer to show
        // the strongest line (or the strongest group of bins: an oscillator with phase noise smears the line over several bins, a quarter of
        // the symbol rate in a bad case), leaving out offsets that were tried and did not lock (the radio's DC spike, the lines a symbol
        // rate either side of the carrier's)
        if (rejected.size() > 6) rejected.clear();
        const double mult = qpsk ? 4.0 : 2.0;
        const double binHz = fs2 / kAcqFft / mult;
        double mean = 0;
        for (int i = 0; i < kAcqFft; i++) mean += acqSpec[(size_t)i];
        mean /= kAcqFft;
        auto binFreq = [&](double k) { return cfo + (k < kAcqFft / 2.0 ? k : k - kAcqFft) * binHz; };
        auto isRejected = [&](double f) { for (double r : rejected) if (std::fabs(f - r) < 4 * binHz) return true; return false; };
        double bestMargin = 1.0, bestK = 0;
        bool found = false;
        static const int kWidths[] = {1, 5, 13, 33};
        for (int W : kWidths) {
            if (W > 1 && W * binHz > 0.4 * symbolRate) break;
            double run = 0;
            for (int i = 0; i < W; i++) run += acqSpec[(size_t)((kAcqFft - W / 2 + i) % kAcqFft)];
            // run holds the window centred on bin 0; slide it round the circle
            for (int c = 0; c < kAcqFft; c++) {
                if (c > 0) { run += acqSpec[(size_t)((c + (W - 1) / 2) % kAcqFft)] - acqSpec[(size_t)(((c - 1 - W / 2) % kAcqFft + kAcqFft) % kAcqFft)]; }
                const double thr = W == 1 ? std::max(3.0, 1.0 + 7.0 / std::sqrt((double)acqBlocks)) : 1.0 + 7.0 / std::sqrt((double)acqBlocks * W) + 0.15;
                const double margin = run / (W * mean) / thr;
                if (margin > bestMargin && !isRejected(binFreq(c))) {
                    bestMargin = margin; bestK = c; found = true;
                    if (W == 1) {
                        const float l = acqSpec[(size_t)((c + kAcqFft - 1) % kAcqFft)], cc = acqSpec[(size_t)c], r = acqSpec[(size_t)((c + 1) % kAcqFft)];
                        const float den = l - 2 * cc + r;
                        if (std::fabs(den) > 1e-20f) bestK = c + 0.5 * (l - r) / den;
                    }
                }
            }
        }
        if (found) {
            const double k = bestK < kAcqFft / 2.0 ? bestK : bestK - kAcqFft;
            cfo += k * binHz;
            setRot();
            resetCarrier();
            acq = false; sinceAcq = 0; okAtAcq = ok;
        }
        if (acqBlocks < kAcqMaxBlocks && acq) return;     // keep adding blocks
        std::fill(acqSpec.begin(), acqSpec.end(), 0.f);
        acqBlocks = 0;
    }

    // ---- a symbol out of the timing loop
    // vq (offset QPSK): the sample half a symbol later, where the Q rail has its symbol; z takes I from v and Q from vq, each turned back by its own carrier phase
    void symbol(cf32 v, const cf32* vq = nullptr) {
        cf32 z = v * cf32((float)std::cos(-cPhase), (float)std::sin(-cPhase));
        if (vq) z = cf32(z.real(), (*vq * cf32((float)std::cos(-(cPhase + 0.5 * cFreq)), (float)std::sin(-(cPhase + 0.5 * cFreq)))).imag());
        const float re = z.real(), im = z.imag();
        // carrier loop (BPSK: the quadrature part against the sign of I; QPSK: decision directed with the points on the diagonals)
        float e, lockNow;
        const float p = re * re + im * im + 1e-12f;
        if (!qpsk) {
            e = (re >= 0 ? im : -im);
            lockNow = (re * re - im * im) / p;
            meanAbs += 0.002f * (std::fabs(re) - meanAbs);
            meanIm2 += 0.002f * (im * im - meanIm2);
        } else {
            e = 0.70710678f * ((re >= 0 ? im : -im) - (im >= 0 ? re : -re));
            const float r2 = re * re - im * im, i2 = 2 * re * im;
            lockNow = -(r2 * r2 - i2 * i2) / (p * p);                // -Re(z^4) / |z|^4
            const float ma = 0.5f * (std::fabs(re) + std::fabs(im));
            meanAbs += 0.002f * (ma - meanAbs);
            const float er = std::fabs(re) - meanAbs, ei = std::fabs(im) - meanAbs;
            meanErr2 += 0.002f * (er * er + ei * ei - meanErr2);
        }
        cPhase += cAlpha * e + cFreq;
        cFreq += cBeta * e;
        if (cPhase > kPi) cPhase -= 2 * kPi; else if (cPhase < -kPi) cPhase += 2 * kPi;
        lockM += 0.002f * (lockNow - lockM);
        constel.push_back(z);
        if (constel.size() > 512) constel.pop_front();
        if (!acq) {
            sinceAcq++;
            if (sinceAcq == 20000) { double a, b; loopGains(qpsk ? 0.004 : 0.008, 0.707, a, b); cAlpha = a; cBeta = b; }
            if (sinceAcq > 30000) {
                const float onT = qpsk ? 0.2f : 0.35f, offT = qpsk ? 0.1f : 0.2f;
                const bool l = lockM > onT;
                if (l != carrierLock) {
                    carrierLock = l;
                    if (l && !carrierReported) { carrierReported = true; char b[96]; snprintf(b, sizeof b, "%s: carrier locked, offset %.1f kHz", tag.c_str(), carrierHz() / 1000); addLog(b, true); }
                }
                lowLock = lockM < offT ? lowLock + 1 : 0;
                if (lowLock > 60000) { acq = true; coarseDone = false; carrierLock = false; lowLock = 0; resetFec(); }
                // never locked to this one: try another line
                if (!carrierLock && ok == okAtAcq && sinceAcq > 120000) { rejected.push_back(cfo); if (coarseAbs < 1e29) coarseRej.push_back(coarseAbs); if (coarseRej.size() > 8) coarseRej.erase(coarseRej.begin()); acq = true; coarseDone = false; resetFec(); }
            }
        }
        // soft code bits: BPSK +1 is code bit 0; QPSK sends G1 on I and G2 on Q
        if (!qpsk) codeSym(-re, 0);
        else { codeSym(-re, -im); codeSym(-im, -re); }
        if (symIdx >= nextSearch) syncSearch();
    }

    // one soft code symbol; `other` is what the I/Q-swapped stream has at this place (QPSK)
    void codeSym(float s, float other) {
        syncBuf.push_back(s);
        if (qpsk) syncBuf2.push_back(other);
        const int64_t k = symIdx++;
        if (symSync) {
            const float live = swapIQ ? other : s;
            if (((k - alignParity) & 1) == 0) firstOfPair = live;
            else {
                const float sc = 40.f / std::max(0.05f, meanAbs);
                const int a = (int)std::max(-127.f, std::min(127.f, firstOfPair * sc));
                int b = (int)std::max(-127.f, std::min(127.f, live * sc));
                if (g2inv) b = -b;
                vit.push(a, b);
                auto& o = vit.out();
                if (!o.empty()) { for (uint8_t bit : o) frameBit(bit); o.clear(); }
            }
        }
    }

    double minInputRate() const { return std::max(200000.0, 2.1 * symbolRate); }
    double carrierHz() const { return cfo + cFreq / (2 * kPi) * symbolRate; }

    // ---- where the frames start in the code symbols, the pairing and the G2 polarity
    void syncSearch() {
        const int64_t need = (int64_t)kSyncFrames * kFrameSyms + 64;
        if ((int64_t)syncBuf.size() > need) {
            syncBuf.erase(syncBuf.begin(), syncBuf.end() - need);
            if (qpsk) syncBuf2.erase(syncBuf2.begin(), syncBuf2.end() - need);
            syncBase = symIdx - need;
        }
        nextSearch = symIdx + (int64_t)kFrameSyms * (fstate == 0 ? 2 : 4);
        if (fstate != 0 || (int64_t)syncBuf.size() < need || acq) return;
        // the 52 code bits of the sync word that do not depend on the encoder state, as +-1 for each G2 polarity
        // (plain, and NRZ-M coded: a differentially coded stream only differs by a sign, which the search ignores)
        float pat[4][64];
        for (int j = 0; j < 64; j++)
            for (int d = 0; d < 2; d++) {
                const int bit = (int)(((d ? asmPatternNrz : asmPattern) >> (63 - j)) & 1);
                pat[d * 2][j] = bit ? 1.f : -1.f;
                pat[d * 2 + 1][j] = (bit ^ (j & 1)) ? 1.f : -1.f;
            }
        double norm = 0;
        for (float v : syncBuf) norm += std::fabs(v);
        norm /= (double)syncBuf.size();
        double best = 0;
        int bo = 0, bg = 0, bb = 0;
        // QPSK: the stream in the order I, Q and with I and Q swapped (a quarter turn of the carrier loop's lock point swaps them), only
        // at whole symbols (even offsets)
        for (int buf = 0; buf < (qpsk ? 2 : 1); buf++) {
            const std::vector<float>& sb = buf ? syncBuf2 : syncBuf;
            for (int o = 0; o < kFrameSyms; o += qpsk ? 2 : 1)
                for (int g = 0; g < 4; g++) {
                    double sc = 0;
                    for (int m = 0; m < kSyncFrames; m++) {
                        const float* sp = sb.data() + o + (size_t)m * kFrameSyms;
                        float c = 0;
                        for (int j = 12; j < 64; j++) c += sp[j] * pat[g][j];
                        sc += std::fabs(c);
                    }
                    if (sc > best) { best = sc; bo = o; bg = g; bb = buf; }
                }
        }
        const double q = best / (kSyncFrames * 52.0 * std::max(1e-9, norm));
        if (q < 0.45) return;
        const std::vector<float>& sb = bb ? syncBuf2 : syncBuf;
        const int64_t start = syncBase + bo;
        symSync = true;
        swapIQ = bb == 1;
        g2inv = (bg & 1) == 1;
        if (nrzm != (bg >= 2)) { nrzm = bg >= 2; addLog(nrzm ? (tag + ": differential (NRZ-M) coded frames") : (tag + ": frames coded without NRZ-M"), true); }
        nrzPrev = 0;
        alignParity = (int)(start & 1);
        vit.reset(); vit.out().clear();
        fstate = 0; reg = 0; regBits = 0;
        // run the Viterbi decoder over what is buffered from a little before the newest sync word on, so this frame is not lost
        const int64_t from = std::max(syncBase, start + (int64_t)(kSyncFrames - 1) * kFrameSyms - 256);
        const float sc = 40.f / std::max(0.05f, meanAbs);
        int64_t k = from;
        if (((k - alignParity) & 1) != 0) k++;
        for (; k + 1 < symIdx; k += 2) {
            const float a = sb[(size_t)(k - syncBase)], b = sb[(size_t)(k + 1 - syncBase)];
            int ia = (int)std::max(-127.f, std::min(127.f, a * sc)), ib = (int)std::max(-127.f, std::min(127.f, b * sc));
            if (g2inv) ib = -ib;
            vit.push(ia, ib);
        }
        if (k < symIdx) firstOfPair = sb[(size_t)(k - syncBase)];
        auto& o = vit.out();
        for (uint8_t bit : o) frameBit(bit);
        o.clear();
    }

    // ---- decoded bits: the sync word and the 1020 bytes behind it
    void frameBit(uint8_t bit) {
        if (nrzm) { const uint8_t d = (uint8_t)(bit ^ nrzPrev); nrzPrev = bit; bit = d; }
        if (fstate == 0) {
            reg = reg << 1 | bit;
            if (++regBits < 32) return;
            const int e = ::dect2::bits::popcount((uint32_t)(reg ^ meteor::kAsm)), ei = ::dect2::bits::popcount((uint32_t)(reg ^ ~meteor::kAsm));
            if (e <= 3 || ei <= 3) { invBits = ei < e; fstate = 1; collected = 0; misses = 0; memset(frame, 0, sizeof frame); }
            return;
        }
        if (fstate == 1) {
            if (bit ^ (invBits ? 1 : 0)) frame[collected >> 3] |= (uint8_t)(0x80 >> (collected & 7));
            if (++collected == meteor::kCodedBytes * 8) { doFrame(); fstate = 2; reg = 0; regBits = 0; }
            return;
        }
        reg = reg << 1 | bit;
        if (++regBits < 32) return;
        const int e = ::dect2::bits::popcount((uint32_t)(reg ^ (invBits ? ~meteor::kAsm : meteor::kAsm)));
        const int ei = ::dect2::bits::popcount((uint32_t)(reg ^ (invBits ? meteor::kAsm : ~meteor::kAsm)));
        if (ei <= 3) { invBits = !invBits; misses = 0; }       // a 180 degree slip of the carrier loop
        else if (e <= 6) misses = 0;
        else if (++misses > 3) { fstate = 0; regBits = 0; reg = 0; return; }
        fstate = 1; collected = 0; memset(frame, 0, sizeof frame);
    }

    void doFrame() {
        frames++;
        uint8_t v[meteor::kVcduBytes];
        // the code is the CCSDS (255,223) one; whether the symbols are sent in the dual basis or the conventional one is tried both ways
        // (a wrong basis never decodes), and the one that works is used first from then on
        int r = meteor::decodeCadu(frame, v, rsDual);
        if (r < 0) {
            const int r2 = meteor::decodeCadu(frame, v, !rsDual);
            if (r2 < 0) { bad++; return; }
            rsDual = !rsDual; rsSwitches++;
            char b[96]; snprintf(b, sizeof b, "%s: Reed-Solomon symbols are in the %s basis", tag.c_str(), rsDual ? "dual (CCSDS)" : "conventional");
            addLog(b, true);
            r = r2;
        }
        {
            std::function<void(const uint8_t*)> cb;
            { std::lock_guard<std::mutex> lk(mu); cb = caduCb; }
            if (cb) { uint8_t cadu[meteor::kCaduBytes]; cadu[0] = 0x1A; cadu[1] = 0xCF; cadu[2] = 0xFC; cadu[3] = 0x1D; memcpy(cadu + 4, frame, sizeof frame); meteor::pnXor(cadu + 4, sizeof frame); cb(cadu); }
        }
        ok++;
        corrected += (uint64_t)r;
        lastGoodSym = symIdx;
        if (!frameLockReported) { frameLockReported = true; addLog(tag + ": frames found, decoding", true); }
        asmb.push(v, (double)nIn / std::max(1.0, rate));
    }

    // ---- a .cadu file (1024 byte frames: sync word, then the 1020 bytes derandomised or as sent) through Reed-Solomon and the upper layers
    void drainCadu() {
        for (;;) {
            std::vector<uint8_t> d;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (caduIn.empty()) return;
                d = std::move(caduIn.front());
                caduIn.pop_front();
            }
            uint64_t good = 0, badN = 0;
            // one frame through Reed-Solomon (decodeCadu() takes the randomised bytes; rnd: the file holds them derandomised, so randomise again)
            auto tryFrame = [&](size_t o, int rnd, uint8_t* v, bool strict) {
                uint8_t cw[meteor::kCodedBytes];
                memcpy(cw, d.data() + o + 4, sizeof cw);
                if (rnd) meteor::pnXor(cw, sizeof cw);
                for (int b = 0; b < 2; b++) {
                    const int r = meteor::decodeCadu(cw, v, b == 0 ? rsDual : !rsDual);
                    // the randomising sequence is itself a code word, so the wrong choice can pass Reed-Solomon, but not the VCDU header
                    meteor::Vcdu hv;
                    if (r >= 0 && (!strict || meteor::parseVcdu(v, hv))) { if (b == 1) rsDual = !rsDual; return r; }
                }
                return -1;
            };
            // as sent (randomised) or derandomised like SatDump's files: the one that decodes more of the first frames is used for the file
            int score[2] = {0, 0};
            for (size_t o = 0, k = 0; o + meteor::kCaduBytes <= d.size() && k < 24; o += meteor::kCaduBytes, k++)
                for (int rnd = 0; rnd < 2; rnd++) { uint8_t v[meteor::kVcduBytes]; score[rnd] += tryFrame(o, rnd, v, true) >= 0; }
            const int rnd = score[1] > score[0] ? 1 : 0;
            for (size_t o = 0; o + meteor::kCaduBytes <= d.size(); o += meteor::kCaduBytes) {
                uint8_t v[meteor::kVcduBytes];
                const int r = tryFrame(o, rnd, v, false);
                frames++;
                if (r < 0) { bad++; badN++; continue; }
                ok++; good++; corrected += (uint64_t)r;
                asmb.push(v, 0.0);
            }
            char b[96]; snprintf(b, sizeof b, "%s: CADU file, %llu frames decoded, %llu failed", tag.c_str(), (unsigned long long)good, (unsigned long long)badN);
            addLog(b, true);
        }
    }

    // ---- input
    void timing() {
        // symbols at pos (fractional index into y), Gardner's error from the sample half a symbol earlier
        const double half = spsEst / 2;
        while (pos + (oqpsk ? half : 0) + 3 < (double)y.size()) {
            if (pos - half - 2 < 0) { pos += 1; continue; }
            const cf32 yk = cubic(y.data(), pos), ym = cubic(y.data(), pos - half);
            agc += 0.001f * (std::norm(yk) - agc);
            const float g = 1.f / std::sqrt(std::max(1e-12f, agc));
            const cf32 a = yk * g, m = ym * g;
            float e;
            cf32 q(0, 0);
            if (oqpsk) {
                // offset QPSK: Gardner on each rail in the carrier's frame. I has its symbols at pos (the middle at pos - half), Q at pos + half (its middle at pos)
                q = cubic(y.data(), pos + half) * g;
                const cf32 r((float)std::cos(-cPhase), (float)std::sin(-cPhase));
                const cf32 ar = a * r, mr = m * r, qr = q * r;
                e = (yprev.real() - ar.real()) * mr.real() + (yprev.imag() - qr.imag()) * ar.imag();
                yprev = cf32(ar.real(), qr.imag());
            } else {
                e = ((yprev - a) * std::conj(m)).real();
                yprev = a;
            }
            if (std::isfinite(e)) spsEst += tBeta * e;
            spsEst = std::max(sps * 0.995, std::min(sps * 1.005, spsEst));
            // a burst after silence (the AGC still tiny) or a damaged sample gives a wild error: the step stays between half a symbol and
            // two, so the loop always moves forward (a negative step hung it) and never jumps past the buffer (fuzz_engine_modes)
            const double step = spsEst + tAlpha * (std::isfinite(e) ? e : 0.f) * sps;
            pos += std::max(0.5 * spsEst, std::min(2.0 * spsEst, step));
            if (oqpsk) symbol(a, &q); else symbol(a);
        }
        if (!(pos >= 0 && pos < 1e12)) pos = (double)y.size();   // never a NaN or a wild index into the cast below (UB) or into y
        const double dropD = std::floor(pos - spsEst - 4);
        const int drop = dropD > 0 && dropD < 2e9 ? (int)dropD : 0;
        if (drop > 0 && drop <= (int)y.size()) { y.erase(y.begin(), y.begin() + drop); pos -= drop; }
    }

    void feed(const cf32* x, size_t n) {
        y.reserve(y.size() + n / D + 8);
        const double off = offset.load();
        if (off != appliedOffset) { appliedOffset = off; setRot(); }
        for (size_t i = 0; i < n; i++) {
            nco *= rot;
            if (++ncoCount >= 1024) { nco /= std::abs(nco); ncoCount = 0; }
            const cf32 v = x[i] * cf32((float)nco.real(), (float)nco.imag());
            levelAcc += std::norm(x[i]); levelN++;
            cf32 d = v;
            if (acq && useCoarse && !coarseDone) { coarseFeed(v); continue; }
            if (D > 1) {
                lpf.push(v);
                if (++decPhase < D) continue;
                decPhase = 0;
                d = lpf.out();
            }
            mf.push(d);
            const cf32 o = mf.out();
            y.push_back(o);
            if (acq) acquire(o);
            
        }
        timing();
    }

    void report() {
        GoesTelemetry t;
        const bool decoding = ok && symIdx - lastGoodSym < 4 * kFrameSyms;
        t.state = decoding ? 3 : fstate != 0 ? 2 : carrierLock ? 1 : 0;
        t.cfoHz = carrierHz();
        const float n2 = std::max(1e-9f, 2 * meanIm2);
        // Es/N0 of a symbol: BPSK A^2 / (2 sigma^2); QPSK 2 a^2 / E|error|^2 (a: the amplitude of each component)
        t.snrDb = qpsk ? (float)(10 * std::log10(std::max(1e-6f, 2 * meanAbs * meanAbs / std::max(1e-9f, meanErr2))))
                       : (float)(10 * std::log10(std::max(1e-6f, meanAbs * meanAbs / n2)));
        t.qpsk = qpsk;
        t.levelDb = levelN ? (float)(10 * std::log10(std::max(1e-20, levelAcc / levelN))) : -200.f;
        levelAcc = 0; levelN = 0;
        t.dataValid = t.state >= 3;
        t.blocksOk = ok; t.blocksBad = bad;
        t.timeSec = nIn / std::max(1.0, rate);
        t.inputRate = rate;
        t.symbolRate = fs2 / std::max(1e-9, spsEst);
        t.carrierLock = carrierLock || decoding;
        t.frameLock = fstate != 0;
        t.invertedG2 = g2inv;
        t.rsDualBasis = rsDual;
        t.lockMetric = std::max(0.f, lockM);
        const uint64_t df = frames - lastFrames, dc = corrected - lastCorr, dok = ok - lastOk;
        t.rsByteErrors = df ? (float)dc / (float)(df * meteor::kCodedBytes) : 0.f;
        t.frames = frames; t.rsCorrected = corrected;
        t.vcdus = asmb.vcdus; t.fillVcdus = asmb.fillVcdus; t.vcduGaps = asmb.vcduGaps; t.packets = asmb.packets; t.crcErrors = asmb.crcErrors;
        t.files = asmb.files; t.fileErrors = asmb.fileErrors; t.scid = asmb.scid;
        for (int v = 0; v < 63; v++) if (asmb.perVcid[v]) t.vcids.emplace_back(v, asmb.perVcid[v]);
        t.constellation.assign(constel.begin(), constel.end());
        auto push = [](std::deque<float>& h, float v) { h.push_back(v); if (h.size() > kGoesHist) h.pop_front(); };
        push(snrH, t.snrDb); push(errH, t.rsByteErrors * 100.f); push(rateH, (float)dok * 4.f);
        lastFrames = frames; lastCorr = corrected; lastOk = ok;
        t.snrHist.assign(snrH.begin(), snrH.end());
        t.errHist.assign(errH.begin(), errH.end());
        t.rateHist.assign(rateH.begin(), rateH.end());
        t.products = asmb.products();
        t.log.assign(logLines.begin(), logLines.end());
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++seq;
        pub = std::move(t);
    }
};

GoesReceiver::GoesReceiver() : p_(std::make_unique<Impl>()) {}
GoesReceiver::~GoesReceiver() = default;

void GoesReceiver::configure(double r) {
    Impl& m = *p_;
    { std::lock_guard<std::mutex> lk(m.mu); m.rateSet = r; }
    if (r <= 0) return;
    m.setup(r);
    m.nIn = 0; m.nextReport = 0.25 * r;
}
void GoesReceiver::setSymbolRate(double hz) {
    if (!(hz > 1000)) hz = kGoesSymbolRate;
    p_->symbolRate = hz;
    const double r = p_->rateSet;
    if (r > 0) configure(r);
}
double GoesReceiver::symbolRate() const { return p_->symbolRate; }
void GoesReceiver::setSignalOffset(double hz) { p_->offset = hz; }
void GoesReceiver::setQpsk(bool on) {
    if (p_->qpsk == on) return;
    p_->qpsk = on;
    const double r = p_->rateSet;
    if (r > 0) configure(r);
}
void GoesReceiver::setOqpsk(bool on) {
    if (p_->oqpsk == on && p_->qpsk == on) return;
    p_->oqpsk = on; p_->qpsk = on;
    const double r = p_->rateSet;
    if (r > 0) configure(r);
}
void GoesReceiver::setDialect(int d, const std::string& tag) {
    p_->dialect = d;
    p_->tag = tag;
    p_->asmb.setDialect(d);
}
void GoesReceiver::setKeys(const std::vector<std::pair<int, std::array<uint8_t, 8>>>& keys) { p_->asmb.setKeys(keys); }
bool GoesReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= p_->minInputRate() - 1; }
void GoesReceiver::reset() { p_->resetReq = true; }
void GoesReceiver::clearProducts() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.products.clear(); p_->pub.log.clear(); p_->loadReq = false; p_->loadIn.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}

namespace {
// a product as the results file holds it (Product itself carries a mutex)
struct GoesProductRec {
    uint64_t id = 0;
    int kind = 0, vcid = 0, apid = 0;
    goes::Headers hdr;
    std::string name;
    double startSec = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> pix, rowDone;
    int rowsDone = 0, segsDone = 0, segsTotal = 0;
    std::vector<uint8_t> data;
    std::string text, innerName;
    bool complete = false, errors = false;
    uint64_t version = 0, bytes = 0;
};
constexpr size_t kGoesSaveBytes = 48u << 20;   // the newest products up to this many bytes of pictures and data go into the file
} // namespace

// the products already stay through reset() and configure(): only Clear empties them
void GoesReceiver::clearResults() { clearProducts(); }
std::string GoesReceiver::saveResults() const {
    std::vector<std::shared_ptr<goes::Product>> list;
    { std::lock_guard<std::mutex> lk(p_->mu); list = p_->loadReq ? p_->loadIn : p_->pub.products; }
    std::vector<GoesProductRec> v;
    size_t total = 0;
    for (const auto& q : list) {
        if (!q) continue;
        GoesProductRec r;
        r.id = q->id; r.kind = q->kind; r.vcid = q->vcid; r.apid = q->apid; r.hdr = q->hdr; r.name = q->name; r.startSec = q->startSec;
        {
            std::lock_guard<std::mutex> pl(q->mu);
            r.w = q->w; r.h = q->h; r.pix = q->pix; r.rowDone = q->rowDone; r.rowsDone = q->rowsDone; r.segsDone = q->segsDone; r.segsTotal = q->segsTotal;
            r.data = q->data; r.text = q->text; r.innerName = q->innerName; r.complete = q->complete; r.errors = q->errors; r.version = q->version; r.bytes = q->bytes;
        }
        total += r.pix.size() + r.data.size() + r.text.size();
        if (total > kGoesSaveBytes && !v.empty()) break;
        v.push_back(std::move(r));
    }
    return results::pack(v);
}
bool GoesReceiver::loadResults(const std::string& blob) {
    std::vector<GoesProductRec> v;
    if (!results::unpack(blob, v)) return false;
    std::vector<std::shared_ptr<goes::Product>> list;
    for (auto& r : v) {
        auto q = std::make_shared<goes::Product>();
        q->id = r.id; q->kind = r.kind; q->vcid = r.vcid; q->apid = r.apid; q->hdr = r.hdr; q->name = r.name; q->startSec = r.startSec;
        q->w = r.w; q->h = r.h; q->pix = std::move(r.pix); q->rowDone = std::move(r.rowDone); q->rowsDone = r.rowsDone; q->segsDone = r.segsDone; q->segsTotal = r.segsTotal;
        q->data = std::move(r.data); q->text = std::move(r.text); q->innerName = std::move(r.innerName); q->complete = r.complete; q->errors = r.errors; q->version = r.version; q->bytes = r.bytes;
        list.push_back(std::move(q));
    }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.products = list;
    p_->loadIn = std::move(list); p_->loadReq = true;
    p_->pub.seq++;
    return true;
}

void GoesReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false) && m.rate > 0) m.resetState();
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.loadReq) { m.asmb.adopt(std::move(m.loadIn)); m.loadIn.clear(); m.loadReq = false; }
    }
    if (m.clearReq.exchange(false)) {
        m.asmb.clear(); m.logLines.clear();
        m.frames = m.ok = m.bad = m.corrected = 0; m.lastFrames = m.lastCorr = m.lastOk = 0;
    }
    m.drainCadu();
    if (m.rate <= 0 || n == 0) return;
    // in pieces of at most a quarter second, so the reports keep their pace
    size_t i = 0;
    while (i < n) {
        const size_t left = (size_t)std::max<int64_t>(1, (int64_t)m.nextReport - m.nIn);
        const size_t k = std::min(n - i, left);
        if (m.rate >= m.minInputRate() - 1) m.feed(x + i, k);
        m.nIn += (int64_t)k;
        i += k;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
}

bool GoesReceiver::telemetry(GoesTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void GoesReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void GoesReceiver::setCaduCallback(std::function<void(const uint8_t*)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->caduCb = std::move(cb); }
void GoesReceiver::importCadu(std::vector<uint8_t> data) { std::lock_guard<std::mutex> lk(p_->mu); p_->caduIn.push_back(std::move(data)); }
void GoesReceiver::setFileCallback(std::function<void(const std::vector<uint8_t>&, int, int)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->fileCb = std::move(cb); }

ModeTuning goesTuning() {
    ModeTuning t;
    t.stdMode = 62; t.id = "goes"; t.name = "GOES HRIT";
    t.minMhz = 1670; t.maxMhz = 1710; t.defMhz = 1694.1;   // GOES-R series HRIT / EMWIN downlink
    t.sampleRate = 2400000;                               // RTL-SDR friendly; 2.6 samples a symbol
    t.basebandHz = 0;
    t.bandwidthMhz = 1.4;                                 // 927 ksym/s x (1 + 0.5) roll-off
    t.minSampleRate = 2000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
