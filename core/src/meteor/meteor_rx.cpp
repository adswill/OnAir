// Meteor-M LRPT receiver (see meteor_rx.h).
#include "dect2/meteor/meteor_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/meteor/meteor_tel.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using cd = std::complex<double>;
using dect2::math::kPi;
constexpr double kAlpha = 0.6;          // roll-off
constexpr double kPubSec = 0.25;
constexpr double kHandOver = 0.003;     // rad/symbol (~34 Hz): the Costas frequency state moves into the front-end mixer
constexpr int kHist = 4096;             // decimated samples kept (power of two)
constexpr int kAcqLog2 = 10, kAcqN = 1 << kAcqLog2, kAcqBlocks = 48;
constexpr double kLineHold = 300, kLineHoldSec = 1;   // Hz, s: until frames decode (at most 1 s), the carrier stays this close to the line estimate
constexpr int kAsmErr = 10;          // hard errors allowed in each of the two sync words of the ambiguity search (of 52)
constexpr double kLineBn = 0.002, kLineKp = 0.25;   // loops after the line acquisition: carrier Bn T, timing gains as after 0.5 s
constexpr double kNarrowBn = 0.002, kNarrowKp = 0.25; // and once frames decode (other acquisitions)
constexpr int kLineLog2 = 15, kLineN = 1 << kLineLog2;   // OQPSK line acquisition: about 0.1 s

double rrcTap(double t, double a) {     // t in symbols, unit energy-ish
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / kPi;
    if (std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    return (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) / (kPi * t * (1 - 16 * a * a * t * t));
}

struct Fir {   // decimating FIR on a doubled ring
    std::vector<float> h;
    std::vector<cf32> buf;
    int n = 1, pos = 0, d = 1, ph = 0;
    void init(const std::vector<float>& taps, int dec) { h = taps; n = (int)h.size(); buf.assign(2 * (size_t)n, cf32(0, 0)); d = dec; pos = 0; ph = 0; }
    bool push(cf32 x, cf32& y) {
        buf[(size_t)pos] = x; buf[(size_t)(pos + n)] = x;
        pos = pos + 1 == n ? 0 : pos + 1;
        if (++ph < d) return false;
        ph = 0;
        const cf32* b = &buf[(size_t)pos];
        float re = 0, im = 0;
        for (int i = 0; i < n; i++) { re += b[i].real() * h[(size_t)i]; im += b[i].imag() * h[(size_t)i]; }
        y = cf32(re, im);
        return true;
    }
};

inline int8_t sat8(float v) { return (int8_t)std::clamp((int)std::lround(v), -127, 127); }
inline int sgn(float v) { return v >= 0 ? 1 : -1; }

// the 8 phase ambiguities on a pair of soft values seen as (I, Q): 4 rotations, mirrored or not
inline void xform(int t, int a, int b, int& oa, int& ob) {
    if (t & 4) b = -b;
    switch (t & 3) {
    case 0: oa = a; ob = b; break;
    case 1: oa = -b; ob = a; break;
    case 2: oa = -a; ob = -b; break;
    default: oa = b; ob = -a; break;
    }
}

inline int8_t diffSoft(int x, int px) {   // the product's sign, the weaker magnitude
    const int m = std::min(std::abs(x), std::abs(px));
    return (int8_t)(((x >= 0) == (px >= 0)) ? m : -m);
}

// N2-x: the transmitter applies NRZ-M to the bits before the (linear, time-invariant) convolutional encoder, so the XOR of two successive
// code bits on each rail is the code of the undifferenced bits. The sync word is searched for in that per-rail difference domain (as
// diffSoft forms it, a with its sign flipped); once found, the transform t found there is turned into the one on the raw pairs (r) so
// Viterbi works on the full-strength soft values and NRZ-M is undone on its decided bits, as SatDump does. -1: none fits.
inline int rawTransform(int t) {
    for (int r = 0; r < 8; r++) {
        bool ok = true;
        for (int s = 0; s < 16 && ok; s++) {
            const int a = s & 1 ? 1 : -1, b = s & 2 ? 1 : -1, pa = s & 4 ? 1 : -1, pb = s & 8 ? 1 : -1;
            int ta, tb, ra, rb, pra, prb;
            xform(t, diffSoft(a, pa), -diffSoft(b, pb), ta, tb);
            xform(r, a, b, ra, rb);
            xform(r, pa, pb, pra, prb);
            ok = ((ta > 0) == ((ra > 0) != (pra > 0))) && ((tb > 0) == ((rb > 0) != (prb > 0)));
        }
        if (ok) return r;
    }
    return -1;
}

} // namespace

struct MeteorReceiver::Impl {
    std::mutex mu;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<int> modSet{0};
    double rateSet = 0, offsetSet = 0;
    std::function<void(const std::string&)> log;

    // run settings
    double rate = 0, offset = 0;
    int mod = 1, modRunSetting = 0;
    double rs = 72000;
    bool oqpsk = true, diff = true, inter = false;

    // front end
    cd rot{1, 0}, step{1, 0};
    double coarseHz = 0;
    int dec = 1;
    double fd = 0, sps = 4;
    Fir lpf, rrc;
    std::vector<cf32> hist;
    int64_t hN = 0;
    double tau = 0, spsEst = 4;
    double theta = 0, omega = 0;
    double alpha = 0, beta = 0, kp = 0, ki = 0;
    float amp = 0.1f;
    float prevI = 0, prevQ = 0;
    int64_t nSym = 0;
    float lockAvg = 0, varAvg = 1;
    double m2Avg = 1, m4Avg = 2;           // moments of the soft values per rail, for the displayed SNR
    // coarse search
    bool coarseDone = false;
    std::vector<float> acqRe, acqIm, psd;
    int acqFill = 0, acqBlocks = 0;

    // decoder chain
    int diffPrevA = 0, diffPrevB = 0;
    // 80k: marker sync and deinterleaver
    std::vector<int8_t> mkRing;          // recent soft bits (I, Q per symbol)
    int64_t mkN = 0, mkLast = 0;
    bool mkLock = false;
    int mkT = 0, mkPos = 0, mkBad = 0, mkPair = 0;
    int8_t mkPairBuf = 0;
    std::vector<int8_t> deint;
    int64_t deJ = 0;
    int deHalf = 0;
    int8_t deFirst = 0;
    // phase ambiguity search
    std::vector<int8_t> pairs;           // soft pairs a, b (before any differential decoding)
    int64_t pairsSinceSearch = 0;
    bool searchLock = false;
    int xt = -1, rawT = -1;              // rawT >= 0: N2-x, Viterbi on the raw pairs, NRZ-M after it
    meteor::NrzmDecoder nrzm;
    meteor::Viterbi27 vit;
    // frame sync on decoded bits
    uint32_t sr = 0;
    int64_t bitCount = 0;
    bool inFrame = false, inv = false, frameLock = false;
    int frameBits = 0, misses = 0;
    uint8_t frame[meteor::kCodedBytes];
    int64_t sinceFrame = 0;              // bits since the last frame was taken
    double lastGoodSec = -1e9, lastAsmSec = -1e9, modStartSec = 0, lockLostSec = 0;

    // packets and picture
    meteor::PacketDemux demux[64];
    MeteorImage img;
    bool imgLoad = false;                // a loaded picture (mu): the feed thread takes it as the picture in progress
    std::shared_ptr<const MeteorImage> imgIn;
    bool imgDirty = false;
    uint64_t imgId = 0;
    meteor::StripRows rows;

    // telemetry
    MeteorTelemetry tel, pub;
    uint64_t seq = 0;
    int64_t nIn = 0;
    double nextReport = 0, power = 0;
    int64_t nPower = 0;
    std::vector<std::pair<float, float>> constel;
    size_t constelPos = 0;

    double nowSec() const { return rate > 0 ? (double)nIn / rate : 0; }

    void say(const std::string& s) { if (log) log("Meteor LRPT: " + s); }

    void setMod(int m) {
        mod = m;
        rs = m == 3 ? 80000 : 72000;
        oqpsk = m != 2; diff = m != 2; inter = m == 3;
    }

    void initDemod() {
        if (rate <= 0) return;
        dec = std::max(1, (int)std::floor(rate / (4 * rs)));
        fd = rate / dec;
        sps = fd / rs;
        const double bw1 = rs * (1 + kAlpha) / 2;
        const double df = std::max(fd - 2 * bw1, 0.2 * fd);
        int n = 2 * (int)std::ceil(3.5 * rate / df / 2) + 1;
        n = std::clamp(n, 9, 401);
        std::vector<float> h((size_t)n);
        double sum = 0;
        const double fc = (fd / 2) / rate;   // cycles per input sample
        for (int i = 0; i < n; i++) {
            const double t = i - (n - 1) / 2.0;
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1));
            const double s = std::fabs(t) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
            h[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& v : h) v = (float)(v / sum);
        if (dec == 1) h.assign(1, 1.f);
        lpf.init(h, dec);
        const int span = 6;
        const int m = 2 * (int)std::ceil(span * sps) + 1;
        std::vector<float> g((size_t)m);
        double e = 0;
        for (int i = 0; i < m; i++) { const double t = (i - (m - 1) / 2.0) / sps; g[(size_t)i] = (float)rrcTap(t, kAlpha); e += g[(size_t)i]; }
        for (auto& v : g) v = (float)(v / e);
        rrc.init(g, 1);
        hist.assign(kHist, cf32(0, 0));
        hN = 0;
        tau = 8; spsEst = sps;
        theta = 0; omega = 0;
        amp = 0; prevI = prevQ = 0; nSym = 0; lockAvg = 0; varAvg = 1; m2Avg = 1; m4Avg = 2;
        loopGains(0.02);
        kp = 0.02 * sps; ki = 2e-5 * sps;
        coarseDone = false; acqFill = 0; acqBlocks = 0; lnFill = 0; lnTries = 0; holdSym = 0; lineHold = false; narrowed = false;
        acqRe.assign(kAcqN, 0); acqIm.assign(kAcqN, 0); psd.assign(kAcqN, 0);
        step = std::polar(1.0, -2 * kPi * (offset + coarseHz) / rate);
        resetChain();
    }

    void loopGains(double bn) {
        const double z = 0.707, th = bn / (z + 1 / (4 * z)), d = 1 + 2 * z * th + th * th;
        alpha = 4 * z * th / d; beta = 4 * th * th / d;
    }

    void resetChain() {
        diffPrevA = diffPrevB = 0;
        mkRing.assign(80 * 48, 0); mkN = 0; mkLast = 0; mkLock = false; mkPos = 0; mkBad = 0; mkPair = 0;
        deint.clear(); deJ = 0; deHalf = 0;
        pairs.clear(); pairsSinceSearch = 0; searchLock = false; xt = -1; rawT = -1; nrzm = {};
        vit.reset();
        sr = 0; bitCount = 0; inFrame = false; frameLock = false; frameBits = 0; misses = 0; sinceFrame = 0;
        for (auto& d : demux) d.reset();
    }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        modRunSetting = modSet.load();
        setMod(modRunSetting ? modRunSetting : 1);
        coarseHz = 0;
        rot = cd(1, 0);
        initDemod();
        modStartSec = nowSec(); lastGoodSec = -1e9; lastAsmSec = -1e9;
        tel.modSetting = modRunSetting;
    }

    void newImage() {
        img = MeteorImage();
        img.id = ++imgId;
        rows.reset();
        imgDirty = true;
    }

    // --- coarse carrier: the raised-cosine spectrum fitted to the averaged power spectrum ---
    void coarseSample(cf32 y) {
        acqRe[(size_t)acqFill] = y.real(); acqIm[(size_t)acqFill] = y.imag();
        if (++acqFill < kAcqN) return;
        acqFill = 0;
        for (int i = 0; i < kAcqN; i++) { const float w = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kAcqN)); acqRe[(size_t)i] *= w; acqIm[(size_t)i] *= w; }
        fftSplit(acqRe.data(), acqIm.data(), kAcqLog2, false);
        for (int i = 0; i < kAcqN; i++) psd[(size_t)i] += acqRe[(size_t)i] * acqRe[(size_t)i] + acqIm[(size_t)i] * acqIm[(size_t)i];
        if (++acqBlocks < kAcqBlocks) return;
        acqBlocks = 0;
        const double bin = fd / kAcqN;
        const double f1 = rs * (1 - kAlpha) / 2, f2 = rs * (1 + kAlpha) / 2;
        const int maxShift = std::max(1, (int)std::min(15000.0 / bin, (fd / 2 - f2) / bin));
        std::vector<double> c((size_t)(2 * maxShift + 1));
        for (int s = -maxShift; s <= maxShift; s++) {
            double acc = 0;
            for (int k = -kAcqN / 2; k < kAcqN / 2; k++) {
                const double f = std::fabs((k - s) * bin);
                if (f >= f2) continue;
                const double tpl = f <= f1 ? 1.0 : 0.5 * (1 + std::cos(kPi * (f - f1) / (rs * kAlpha)));
                acc += tpl * psd[(size_t)((k + kAcqN) % kAcqN)];
            }
            c[(size_t)(s + maxShift)] = acc;
        }
        int bi = 0;
        for (int i = 1; i < (int)c.size(); i++) if (c[(size_t)i] > c[(size_t)bi]) bi = i;
        double frac = 0;
        if (bi > 0 && bi + 1 < (int)c.size()) {
            const double a = c[(size_t)bi - 1], b = c[(size_t)bi], d = c[(size_t)bi + 1];
            const double den = a - 2 * b + d;
            if (den < 0) frac = 0.5 * (a - d) / den;
        }
        const double est = (bi - maxShift + frac) * bin;
        std::fill(psd.begin(), psd.end(), 0.f);
        coarseHz += est;
        step = std::polar(1.0, -2 * kPi * (offset + coarseHz) / rate);
        coarseDone = true;
        theta = 0; omega = 0; nSym = 0; loopGains(0.02);
    }

    // --- symbols ---
    cf32 interp(double t) const {
        const int64_t i = (int64_t)std::floor(t);
        const float mu = (float)(t - (double)i);
        const cf32 xm = hist[(size_t)((i - 1) & (kHist - 1))], x0 = hist[(size_t)(i & (kHist - 1))], x1 = hist[(size_t)((i + 1) & (kHist - 1))],
                   x2 = hist[(size_t)((i + 2) & (kHist - 1))];
        const float cm = -mu * (mu - 1) * (mu - 2) / 6, c0 = (mu + 1) * (mu - 1) * (mu - 2) / 2, c1 = -(mu + 1) * mu * (mu - 2) / 2, c2 = (mu + 1) * mu * (mu - 1) / 6;
        return xm * cm + x0 * c0 + x1 * c1 + x2 * c2;
    }

    // --- OQPSK: carrier, symbol clock and timing together from the squared matched-filter output ---
    // For OQPSK (I and Q half a symbol apart) the mean of z^2 is e^{2j(2 pi df t + phi)} * 4 g1 cos(2 pi (t - t0) / T): two lines, at 2 df +
    // and - the symbol rate, standing well over the noise down to 0 dB Es/N0. Their mean gives the carrier, their spacing the symbol clock and
    // the phase of one against the other the symbol timing t0 (modulo T/2: the I / Q pairing, which the ambiguity search settles). The
    // feedback loops then start narrow, on frequency and on time; the timing detector is not phase independent, so it is held for 50 ms
    // while the carrier loop pulls in (it used to walk to its +-0.5 % limit first, and the lock took 1 to 6 s at 4 dB). The shape fit
    // (coarseSample) stays for QPSK and as the fallback when no lines stand out (two blocks).
    std::vector<float> lnRe, lnIm;
    std::vector<cf32> lnZ;
    int lnFill = 0, lnTries = 0;
    int64_t lnStart = 0, holdSym = 0;
    double lineHz = 0;      // the carrier the lines gave; for a while the loop stays within kLineHold of it (no false lock)
    bool lineHold = false;
    bool narrowed = false;
    void lineSample(cf32 z) {
        if (lnZ.empty()) { lnZ.resize(kLineN); lnRe.resize(kLineN); lnIm.resize(kLineN); }
        if (lnFill == 0) lnStart = hN - 1;
        lnZ[(size_t)lnFill] = z;
        if (++lnFill < kLineN) return;
        lnFill = 0;
        for (int i = 0; i < kLineN; i++) {
            const cf32 q = lnZ[(size_t)i] * lnZ[(size_t)i];
            const float w = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kLineN));
            lnRe[(size_t)i] = q.real() * w; lnIm[(size_t)i] = q.imag() * w;
        }
        fftSplit(lnRe.data(), lnIm.data(), kLineLog2, false);
        const double bin = fd / kLineN;
        auto P = [&](int k) { const size_t j = (size_t)((k % kLineN + kLineN) % kLineN); return (double)lnRe[j] * lnRe[j] + (double)lnIm[j] * lnIm[j]; };
        const int s0 = (int)std::lround(rs / bin), J = std::max(1, (int)std::ceil(2e-3 * rs / bin));   // the clock within +-0.2 %
        const int maxC = (int)std::min(2 * 15000.0 / bin, (fd / 2 - rs - 2 * J * bin) / bin - s0);
        double best = -1, mean = 0; int bp = 0, bm = 0;
        for (int k = -maxC - s0 - J; k <= maxC + s0 + J; k++) mean += P(k);
        mean /= (double)(2 * (maxC + s0 + J) + 1);
        for (int c = -maxC; c <= maxC; c++) {
            double pp = -1, pm = -1; int kp_ = 0, km_ = 0;
            for (int j = -J; j <= J; j++) {
                const double a = P(c + s0 + j), b = P(c - s0 - j);
                if (a > pp) { pp = a; kp_ = c + s0 + j; }
                if (b > pm) { pm = b; km_ = c - s0 - j; }
            }
            if (pp + pm > best) { best = pp + pm; bp = kp_; bm = km_; }
        }
        const bool found = mean > 0 && P(bp) > 12 * mean && P(bm) > 12 * mean;
        if (!found) { if (++lnTries >= 2) { acqFill = 0; acqBlocks = 0; std::fill(psd.begin(), psd.end(), 0.f); } return; }
        auto refine = [&](int k) {
            const double a = std::sqrt(P(k - 1)), b = std::sqrt(P(k)), d = std::sqrt(P(k + 1)), den = a - 2 * b + d;
            return (double)k + (den < 0 ? 0.5 * (a - d) / den : 0.0);
        };
        const double fp = refine(bp) * bin, fm = refine(bm) * bin;
        const double df = (fp + fm) / 4, rsEst = (fp - fm) / 2;
        // the line phases, exactly at the two frequencies, without the window
        cd ap(0, 0), am(0, 0);
        {
            const cd sp = std::polar(1.0, -2 * kPi * fp / fd), sm = std::polar(1.0, -2 * kPi * fm / fd);
            cd rp(1, 0), rm(1, 0);
            for (int i = 0; i < kLineN; i++) {
                const cf32 q = lnZ[(size_t)i] * lnZ[(size_t)i];
                const cd qd(q.real(), q.imag());
                ap += qd * rp; am += qd * rm;
                rp *= sp; rm *= sm;
                if ((i & 1023) == 1023) { rp /= std::abs(rp); rm /= std::abs(rm); }
            }
        }
        const double T = std::clamp(fd / rsEst, sps * 0.995, sps * 1.005);
        double t0 = -std::arg(ap * std::conj(am)) / (4 * kPi) * T;   // modulo T/2, from lnStart
        coarseHz += df;
        step = std::polar(1.0, -2 * kPi * (offset + coarseHz) / rate);
        spsEst = T;
        // the next timing instant of that grid (the samples in the filters when the mixer moved make a transient of a few symbols)
        const double now = (double)hN + 4;
        double t = (double)lnStart + t0;
        t += std::ceil((now - t) / (T / 2)) * (T / 2);
        tau = t;
        prevI = prevQ = 0;
        coarseDone = true;
        // carrier and clock known to a few Hz and ppm: both loops start narrow (wide ones slipped by half a symbol and a quarter turn
        // together, again and again, below 3 dB before the sync word was ever found)
        theta = 0; omega = 0; nSym = 0; loopGains(kLineBn);
        kp = 0.02 * sps * kLineKp; ki = 2e-5 * sps * kLineKp * kLineKp;
        narrowed = true;
        holdSym = (int64_t)(0.05 * rs);
        lineHz = coarseHz; lineHold = true;
    }

    void decimated(cf32 y) {
        if (!coarseDone && (!oqpsk || lnTries >= 2)) { coarseSample(y); return; }
        cf32 z;
        rrc.push(y, z);
        hist[(size_t)(hN & (kHist - 1))] = z;
        hN++;
        if (!coarseDone) { lineSample(z); return; }
        if (amp <= 0) amp = 1e-9f;
        while (tau + spsEst / 2 + 3 < (double)hN) {
            if (tau - spsEst - 2 < (double)(hN - kHist)) { tau = (double)hN - spsEst; continue; }
            symbol();
        }
    }

    void symbol() {
        const double T = spsEst;
        const auto rotm = [](double ph) { return cf32((float)std::cos(ph), (float)-std::sin(ph)); };
        const cf32 a = interp(tau) * rotm(theta);
        const cf32 m = interp(tau - T / 2) * rotm(theta - omega / 2);
        float I, Q, eT;
        if (oqpsk) {
            const cf32 q1 = interp(tau + T / 2) * rotm(theta + omega / 2);
            I = a.real(); Q = q1.imag();
            eT = (prevI - I) * m.real() + (prevQ - Q) * a.imag();
        } else {
            I = a.real(); Q = a.imag();
            eT = (prevI - I) * m.real() + (prevQ - Q) * m.imag();
        }
        prevI = I; prevQ = Q;
        const float mag = 0.5f * (std::fabs(I) + std::fabs(Q));
        if (nSym == 0) amp = std::max(mag, 1e-9f);
        amp += 0.002f * (mag - amp);
        const float In = I / amp, Qn = Q / amp;
        const float eC = std::clamp((float)sgn(In) * Qn - (float)sgn(Qn) * In, -1.f, 1.f);
        theta += omega + alpha * eC;
        omega += beta * eC;
        omega = std::clamp(omega, -0.25, 0.25);
        // Doppler (issue #79): the loop's frequency state is handed to the front-end mixer as soon as it is more than a few tens of Hz, so
        // the Costas loop stays centred (its +-0.25 rad/symbol clamp is only ~2.9 kHz, a pass sweeps +-3.5 kHz from wherever the coarse
        // search put it) and the matched filter always sees the signal on frequency. The correction applied in total stays continuous:
        // theta is untouched, omega gives up exactly what the mixer takes (the matched filter's few symbols of delay make a transient of
        // omega * ~6 symbols, a few hundredths of a radian, which the loop absorbs).
        if (std::fabs(omega) > kHandOver) {
            coarseHz += omega * rs / (2 * kPi);
            if (lineHold && nSym > (int64_t)(rs * kLineHoldSec)) lineHold = false;   // Doppler moves the carrier by more than that later
            if (lineHold) coarseHz = std::clamp(coarseHz, lineHz - kLineHold, lineHz + kLineHold);
            step = std::polar(1.0, -2 * kPi * (offset + coarseHz) / rate);
            omega = 0;
        }
        if (theta > kPi) theta -= 2 * kPi; else if (theta < -kPi) theta += 2 * kPi;
        const float eTn = std::clamp(eT / (amp * amp), -1.f, 1.f);
        if (holdSym > 0) holdSym--;
        else { tau += kp * eTn; spsEst += ki * eTn; }
        tau += T;
        spsEst = std::clamp(spsEst, sps * 0.995, sps * 1.005);
        nSym++;
        // after 0.5 s: carrier loop 0.004 (was 0.008) and the symbol clock 4x slower (its noise walked the clock by ~100 ppm at
        // 3-6 dB and slipped symbols). QPSK 72k now decodes at 2-2.5 dB Es/N0 (was 4), OQPSK 72k at 3.5 dB (was 5).
        if (nSym == (int64_t)(rs * 0.5) && !narrowed) { loopGains(0.004); kp *= 0.25; ki *= 0.0625; }
        // lock and spread
        const float r2 = In * In + Qn * Qn;
        if (r2 > 1e-6f) {
            const float re4 = (In * In - Qn * Qn) * (In * In - Qn * Qn) - 4 * In * In * Qn * Qn;
            lockAvg += 0.002f * (-re4 / (r2 * r2) - lockAvg);
        }
        const float dv = (std::fabs(In) - 1) * (std::fabs(In) - 1) + (std::fabs(Qn) - 1) * (std::fabs(Qn) - 1);
        varAvg += 0.002f * (0.5f * dv - varAvg);
        {
            const double i2 = (double)In * In, q2 = (double)Qn * Qn;
            m2Avg += 0.002 * (0.5 * (i2 + q2) - m2Avg);
            m4Avg += 0.002 * (0.5 * (i2 * i2 + q2 * q2) - m4Avg);
        }
        if (constel.size() < 256) constel.emplace_back(In, Qn); else { constel[constelPos] = {In, Qn}; constelPos = (constelPos + 1) % 256; }
        softPair(sat8(In * 40), sat8(Qn * 40));
    }

    // --- bit chain ---
    void softPair(int8_t a, int8_t b) {
        if (inter) { markerBit(a); markerBit(b); return; }
        afterDeint(a, b);
    }

    // a pair of code bits (pre-differential) on its way to the ambiguity search or, once that is settled, the Viterbi decoder
    void afterDeint(int8_t a, int8_t b) {
        if (searchLock) {
            int8_t x = a, y = b;
            if (slip) { x = heldB; y = a; heldB = b; }   // OQPSK found half a symbol late: the pairs are (Q, next I)
            lockedPair(x, y);
            return;
        }
        pairs.push_back(a); pairs.push_back(b);
        pairsSinceSearch++;
        const int64_t need = 8192 * 2 + 64;
        if ((int64_t)pairs.size() / 2 > 2 * need) pairs.erase(pairs.begin(), pairs.begin() + (std::ptrdiff_t)(pairs.size() - (size_t)need * 2));
        if (pairsSinceSearch >= 4096 && (int64_t)pairs.size() / 2 >= 8192 + 64) { pairsSinceSearch = 0; ambiguitySearch(); }
    }

    void lockedPair(int8_t x, int8_t y) {
        if (rawT >= 0) {
            int oa, ob;
            xform(rawT, x, y, oa, ob);
            vitPush(oa, ob);
            return;
        }
        if (diff) {
            const int8_t ox = diffSoft(x, diffPrevA), oy = (int8_t)-diffSoft(y, diffPrevB);
            diffPrevA = x; diffPrevB = y;
            x = ox; y = oy;
        }
        int oa, ob;
        xform(xt, x, y, oa, ob);
        vitPush(oa, ob);
    }

    // 80k: a sync marker of 8 bits every 80, then 72 data bits into the deinterleaver
    void markerBit(int8_t v) {
        mkRing[(size_t)(mkN % (int64_t)mkRing.size())] = v;
        mkN++;
        if (!mkLock) {
            if (!(mkN & 1) && mkN - mkLast >= 1600 && mkN >= (int64_t)mkRing.size()) { mkLast = mkN; markerSearch(); }
            return;
        }
        // locked: this bit at mkPos of the period, after the ambiguity transform (on pairs)
        if (mkPair == 0) { mkPairBuf = v; mkPair = 1; return; }
        mkPair = 0;
        int oa, ob;
        xform(mkT, mkPairBuf, v, oa, ob);
        markerData((int8_t)oa); markerData((int8_t)ob);
    }

    int mkErr = 0;
    void markerData(int8_t v) {
        if (mkPos < 8) {
            if ((v > 0) != ((meteor::kMarker >> (7 - mkPos) & 1) != 0)) mkErr++;
            if (++mkPos == 8) {
                mkBad = mkErr >= 3 ? mkBad + 1 : std::max(0, mkBad - 1);
                mkErr = 0;
                if (mkBad > 10) { mkLock = false; mkBad = 0; say("lost the interleaver markers"); }
            }
            return;
        }
        deintPush(v);
        if (++mkPos == meteor::kMarkerStride) mkPos = 0;
    }

    void markerSearch() {
        const int64_t L = (int64_t)mkRing.size();
        const int64_t start = mkN - L;   // even
        std::vector<uint8_t> hb((size_t)L);
        // The marker's I bits alternate, so the half-symbol-late pairing, mirrored, matches it too with at most one error (its last I
        // bit is a data bit) and the data then deinterleaves to garbage for good (the deinterleaver spans 17 s). The pick is therefore
        // by weight: 2 for a marker without error, 1 with one error; the true pairing gets about 1.7 per period at 2 dB Es/N0, the
        // alias about 1.25. Found by the line acquisition, which lands on either pairing (the old timing loop mostly settled on the right one).
        int bestScore = -1, bestW = -1, bestT = 0, bestOff = 0, bestP = 0;
        for (int pr = 0; pr < 2; pr++)       // pairing: (I, Q) or, half a symbol late, (Q, next I)
            for (int t = 0; t < 8; t++) {
                for (int64_t i = 0; i + 2 < L; i += 2) {
                    int oa, ob;
                    xform(t, mkRing[(size_t)((start + pr + i) % L)], mkRing[(size_t)((start + pr + i + 1) % L)], oa, ob);
                    hb[(size_t)i] = oa > 0; hb[(size_t)i + 1] = ob > 0;
                }
                for (int off = 0; off < meteor::kMarkerStride; off += 2) {
                    int score = 0, wt = 0;
                    for (int64_t q = off; q + 8 <= L - 2; q += meteor::kMarkerStride) {
                        int e = 0;
                        for (int k = 0; k < 8; k++) e += hb[(size_t)(q + k)] != ((meteor::kMarker >> (7 - k) & 1) != 0);
                        score += e <= 1;
                        wt += std::max(0, 2 - e);
                    }
                    if (wt > bestW) { bestW = wt; bestScore = score; bestT = t; bestOff = off; bestP = pr; }
                }
            }
        const int periods = (int)((L - 2) / meteor::kMarkerStride);
        if (bestScore * 4 < periods * 3) return;
        mkLock = true; mkT = bestT; mkBad = 0; mkErr = 0;
        // pairs from now on start at the pairing found; the first one processed starts at bit mkN - bestP
        if (bestP) { mkPairBuf = mkRing[(size_t)((mkN - 1) % L)]; mkPair = 1; } else mkPair = 0;
        mkPos = (int)(((L - 2 * bestP - bestOff) % meteor::kMarkerStride + meteor::kMarkerStride) % meteor::kMarkerStride);
        deint.assign((size_t)meteor::kInterBranches * meteor::kInterBranches * meteor::kInterDelay, 0);
        deJ = 0; deHalf = 0;
        skipToMarker = true;   // the deinterleaver starts with the first data bit of a period (branch 0)
        say("interleaver markers found");
    }
    bool skipToMarker = false;

    void deintPush(int8_t v) {
        if (skipToMarker) { if (mkPos == 8) skipToMarker = false; else return; }
        const int64_t S = (int64_t)deint.size();
        const int64_t per = (int64_t)meteor::kInterBranches * meteor::kInterDelay;
        deint[(size_t)(deJ % S)] = v;
        const int b = (int)(deJ % meteor::kInterBranches);
        const int64_t src = deJ - (int64_t)(meteor::kInterBranches - 1 - b) * per;
        const int8_t out = src >= 0 ? deint[(size_t)(src % S)] : 0;
        deJ++;
        if (deHalf == 0) { deFirst = out; deHalf = 1; } else { deHalf = 0; afterDeint(deFirst, out); }
    }

    // --- phase ambiguity: the encoded sync word in the hard decisions (after the differential decoder), then Viterbi ---
    void ambiguitySearch() {
        static const uint64_t pat = meteor::encodedAsm();
        const uint64_t mask = (1ull << 52) - 1;
        const int np = (int)(pairs.size() / 2);

        // Every pairing and transform is scored and the best pair of sync words a frame apart wins (each with at most kAsmErr of 52 hard
        // errors). The first one under 8 errors used to be taken: at 3 dB the true words have 5 to 12 errors in this (difference) domain,
        // so a frame in two was missed and the lock took 1 to 3 s; and a looser limit taken first-come let the half-symbol pairing, which
        // is a few errors off the true word, win on a clean signal.
        std::vector<int8_t> P((size_t)np * 2), D((size_t)np * 2);
        std::vector<int8_t> eAt((size_t)np);
        auto pairing = [&](int sl) {
            const int n = np - sl;
            for (int i = 0; i < n; i++) {
                P[(size_t)i * 2] = sl ? pairs[(size_t)i * 2 + 1] : pairs[(size_t)i * 2];
                P[(size_t)i * 2 + 1] = sl ? pairs[(size_t)i * 2 + 2] : pairs[(size_t)i * 2 + 1];
            }
            return n;
        };
        int bestScore = 1 << 30, bestSl = -1, bestT = 0, bestAt = -1;
        for (int sl = 0; sl < (oqpsk && !inter ? 2 : 1); sl++) {
            const int n = pairing(sl);
            for (int i = 0; i < n; i++) {
                if (!diff) { D[(size_t)i * 2] = P[(size_t)i * 2]; D[(size_t)i * 2 + 1] = P[(size_t)i * 2 + 1]; continue; }
                const int pa = i ? P[(size_t)i * 2 - 2] : 0, pb = i ? P[(size_t)i * 2 - 1] : 0;
                D[(size_t)i * 2] = diffSoft(P[(size_t)i * 2], pa);
                D[(size_t)i * 2 + 1] = (int8_t)-diffSoft(P[(size_t)i * 2 + 1], pb);
            }
            for (int t = 0; t < 8; t++) {
                uint64_t w = 0;
                for (int i = 0; i < n; i++) {
                    int oa, ob;
                    xform(t, D[(size_t)i * 2], D[(size_t)i * 2 + 1], oa, ob);
                    w = w << 2 | (uint64_t)(oa > 0) << 1 | (uint64_t)(ob > 0);
                    if (i < 31) { eAt[(size_t)i] = 99; continue; }
                    const int e = __builtin_popcountll((w ^ pat) & mask);
                    eAt[(size_t)i] = (int8_t)std::min(e, 99);
                    if (e > kAsmErr || i < 8192 + 31) continue;
                    const int e0 = eAt[(size_t)(i - 8192)];
                    if (e0 > kAsmErr) continue;
                    // ties: the later pair of the same hypothesis; between hypotheses the first one (normal pairing first)
                    const int score = e + e0;
                    if (score < bestScore || (score == bestScore && sl == bestSl && t == bestT)) { bestScore = score; bestSl = sl; bestT = t; bestAt = i; }
                }
            }
        }
        if (bestSl < 0) return;
        const int sl = bestSl, t = bestT, at = bestAt;
        if (sl && diff && varAvg > 0.16f) {
            // N2-x found half a symbol late (pairs Q, next I) on a weak signal (under ~8 dB): there it would be decoded in the
            // difference domain, ~2 dB worse. Move the symbol clock on by half a symbol and the carrier by a quarter turn (the same
            // signal in the normal state) and search again (a fraction of a second). A strong signal keeps the quick lock.
            tau += spsEst / 2; theta += kPi / 2; if (theta > kPi) theta -= 2 * kPi;
            pairs.clear(); pairsSinceSearch = 0;
            say("half a symbol late: symbol clock moved");
            return;
        }
        const int n = pairing(sl);
        searchLock = true; xt = t; slip = sl;
        // The raw-pair transform guessed from the difference domain gives garbage frames when the symbol clock settled half a symbol
        // late (OQPSK, slip 1; found with static offsets too, about one acquisition in three): decode in the difference domain then.
        rawT = diff && !sl ? rawTransform(t) : -1;
        nrzm = {};
        tel.rotation = xt + 8 * slip;
        lastAsmSec = nowSec();
        badRun = 0;
        vit.reset();
        sr = 0; inFrame = false; frameLock = false;
        const int from = std::max(1, at - 31 - 8192 - 40);
        diffPrevA = P[(size_t)from * 2 - 2]; diffPrevB = P[(size_t)from * 2 - 1];
        for (int i = from; i < n; i++) lockedPair(P[(size_t)i * 2], P[(size_t)i * 2 + 1]);
        heldB = pairs[pairs.size() - 1];
        pairs.clear();
        say(std::string("sync word found (") + meteorModName(mod) + ", ambiguity " + std::to_string(tel.rotation) + ")");
    }
    int slip = 0;
    int8_t heldB = 0;
    int badRun = 0;                      // frames in a row that did not decode

    void vitPush(int a, int b) {
        vit.push(a, b);
        auto& o = vit.out();
        for (uint8_t bit : o) frameBit(bit);
        o.clear();
    }

    void frameBit(uint8_t bit) {
        if (rawT >= 0) bit = nrzm.push(bit);
        sr = sr << 1 | bit;
        sinceFrame++;
        if (inFrame) {
            const uint8_t v = (uint8_t)(bit ^ (inv ? 1 : 0));
            uint8_t& by = frame[frameBits >> 3];
            by = (uint8_t)(by << 1 | v);
            if (++frameBits == meteor::kCodedBytes * 8) { inFrame = false; gotFrame(); sinceFrame = 0; }
            return;
        }
        const int e = __builtin_popcount(sr ^ meteor::kAsm), ei = __builtin_popcount(~sr ^ meteor::kAsm);
        if (frameLock) {
            if (sinceFrame < 32) return;
            // the next sync word is due now
            if (std::min(e, ei) <= 10) misses = 0; else misses++;
            // lost: search again now (it was 3 s later), the lock may have slipped
            if (misses > 4) { frameLock = false; searchLock = false; pairs.clear(); pairsSinceSearch = 0; tel.rotation = -1; say("frame sync lost: searching again"); }
            else { if (std::min(e, ei) <= 10) inv = ei < e; startFrame(); tel.asmHits++; lastAsmSec = nowSec(); return; }
        }
        if (e <= 3 || ei <= 3) { inv = ei < e; startFrame(); frameLock = true; misses = 0; tel.asmHits++; lastAsmSec = nowSec(); }
    }

    void startFrame() { inFrame = true; frameBits = 0; std::memset(frame, 0, sizeof frame); }

    void gotFrame() {
        uint8_t v[meteor::kVcduBytes];
        const int e = meteor::decodeCadu(frame, v, false);   // Meteor: conventional basis
        if (e < 0) {
            tel.blocksBad++;
            // three in a row: a lock taken while the loops settled, or one that slipped since; search again
            if (++badRun >= 3) { searchLock = false; frameLock = false; pairs.clear(); pairsSinceSearch = 0; tel.rotation = -1; say("frames do not decode: searching again"); }
            return;
        }
        badRun = 0;
        lineHold = false;
        // decoding: both loops narrower (at 3 dB the OQPSK timing and carrier loops slipped together by half a symbol and a quarter turn,
        // a few times in 8 s, each slip costing 1 to 3 s of frames)
        if (!narrowed && nSym > (int64_t)(rs * 0.5)) { narrowed = true; loopGains(kNarrowBn); kp *= kNarrowKp; ki *= kNarrowKp * kNarrowKp; }
        tel.blocksOk++; tel.rsCorrected += (uint64_t)e;
        lastGoodSec = nowSec();
        meteor::Vcdu vc;
        if (!meteor::parseVcdu(v, vc)) return;
        tel.vcdus++; tel.scid = vc.scid; tel.vcid = vc.vcid; tel.vcduCounter = vc.counter;
        meteor::PacketDemux& d = demux[vc.vcid & 63];
        if (!d.onPacket) d.onPacket = [this](const meteor::Packet& p) { packet(p); };
        const uint64_t lost0 = d.dropped;
        d.push(vc);
        tel.packetsLost += d.dropped - lost0;
    }

    void packet(const meteor::Packet& p) {
        tel.packets++;
        if (p.apid >= meteor::kFirstApid && p.apid < meteor::kFirstApid + meteor::kNumChannels) {
            tel.apidPackets[p.apid - meteor::kFirstApid]++;
            meteor::McuStrip s;
            if (!meteor::decodeImagePacket(p.data, s)) return;
            tel.onboardMs = s.ms; tel.lastQuality = s.q;
            place(p.apid - meteor::kFirstApid, p.seq, s);
        } else if (p.apid == 70) tel.apidPackets[6]++;
        else tel.apidPackets[7]++;
    }

    // the row: meteor::StripRows (time stamps over long gaps)
    void place(int c, int seqNo, const meteor::McuStrip& s) {
        const int k = s.mcu / meteor::kMcuPerPacket;
        const int curRow = rows.place(c, seqNo, k, s.ms);
        if (curRow >= kMeteorMaxRows) return;
        auto& chv = img.ch[(size_t)c];
        if ((int)chv.size() <= curRow) chv.resize((size_t)curRow + 1);
        auto ns = chv[(size_t)curRow] ? std::make_shared<MeteorStrip>(*chv[(size_t)curRow]) : std::make_shared<MeteorStrip>();
        if (ns->px.empty()) { ns->px.assign((size_t)8 * kMeteorWidth, 0); ns->row = curRow; }
        for (int y = 0; y < 8; y++) std::memcpy(&ns->px[(size_t)y * kMeteorWidth + (size_t)k * 112], s.px[y], 112);
        ns->got = (uint16_t)(ns->got | 1u << k);
        chv[(size_t)curRow] = ns;
        img.rows = std::max(img.rows, curRow + 1);
        img.packets[(size_t)c]++;
        imgDirty = true;
    }

    // --- supervision: lost lock, automatic modulation ---
    void supervise() {
        const double now = nowSec();
        if (searchLock && now - std::max(lastGoodSec, lastAsmSec) > 3) {
            searchLock = false; frameLock = false; pairs.clear(); tel.rotation = -1;
            say("no frames: searching again");
        }
        const double since = now - std::max(lastGoodSec, modStartSec);
        if (coarseDone && !searchLock && since > (inter ? 40 : 8) && now - lockLostSec > 2) {
            lockLostSec = now;
            if (modRunSetting == 0) setMod(mod == 1 ? 2 : mod == 2 ? 3 : 1);
            coarseHz = 0;
            initDemod();   // a new coarse search too
            modStartSec = now;
        }
    }

    void report() {
        supervise();
        const double now = nowSec();
        tel.seq = ++seq;
        tel.modulation = mod; tel.modSetting = modRunSetting;
        tel.symbolRate = rs; tel.inputRate = rate; tel.timeSec = now;
        tel.cfoHz = coarseHz + omega * rs / (2 * kPi);
        tel.clockPpm = (sps / spsEst - 1) * 1e6;
        tel.carrierLock = std::clamp(lockAvg, 0.f, 1.f);
        // Es/N0 from the second and fourth moments of each rail (+-A plus noise: m2 = A^2 + s^2, m4 = A^4 + 6 A^2 s^2 + 3 s^4); the
        // spread around the decisions read 1.5 dB high at 2.5 dB (the noise folds onto the decided sign)
        {
            const double a4 = std::max(0.0, 1.5 * m2Avg * m2Avg - 0.5 * m4Avg);
            const double a2 = std::sqrt(a4), s2 = std::max(1e-6, m2Avg - a2);
            tel.snrDb = a2 > 0 ? (float)std::min(40.0, 10 * std::log10(a2 / s2)) : -10.f;
        }
        tel.levelDb = nPower ? (float)(10 * std::log10(std::max(1e-20, power / (double)nPower))) : -200.f;
        power = 0; nPower = 0;
        tel.frameLock = frameLock;
        const bool good = now - lastGoodSec < 2;
        tel.state = good ? 3 : frameLock ? 2 : coarseDone && tel.carrierLock > 0.5f ? 1 : 0;
        tel.dataValid = good;
        tel.constellation = constel;
        if (imgDirty) { tel.image = std::make_shared<const MeteorImage>(img); tel.imageSeq++; imgDirty = false; }
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    void input(cf32 x) {
        const cd v = cd(x.real(), x.imag()) * rot;
        rot *= step;
        if ((nIn & 1023) == 0) rot /= std::abs(rot);
        nIn++;
        cf32 y;
        if (lpf.push(cf32((float)v.real(), (float)v.imag()), y)) decimated(y);
    }
};

MeteorReceiver::MeteorReceiver() : p_(std::make_unique<Impl>()) {}
MeteorReceiver::~MeteorReceiver() = default;

void MeteorReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }   // the picture stays (clearImage)
void MeteorReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void MeteorReceiver::setModulation(int m) { m = std::clamp(m, 0, 3); if (p_->modSet.exchange(m) != m) p_->resetReq = true; }
int MeteorReceiver::modulation() const { return p_->modSet.load(); }
bool MeteorReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= meteorTuning().minSampleRate - 1; }
void MeteorReceiver::reset() { p_->resetReq = true; }
void MeteorReceiver::clearImage() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->imgLoad = false; p_->imgIn.reset(); p_->pub.image.reset(); p_->pub.imageSeq++; p_->pub.seq++; }
    p_->clearReq = true;
}
void MeteorReceiver::clearResults() { clearImage(); }
std::string MeteorReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.image);
}
bool MeteorReceiver::loadResults(const std::string& blob) {
    std::shared_ptr<const MeteorImage> im;
    if (!results::unpack(blob, im)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.image = im; p_->pub.imageSeq++; p_->pub.seq++;
    p_->imgIn = std::move(im); p_->imgLoad = p_->imgIn != nullptr;
    return true;
}

void MeteorReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.newImage();
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.imgLoad) { m.img = *m.imgIn; m.imgId = std::max(m.imgId, m.img.id); m.imgIn.reset(); m.imgLoad = false; m.imgDirty = true; }
    }
    if (m.rate < meteorTuning().minSampleRate - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += kPubSec * m.rate; m.report(); } }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.input(cf32(re, im));
        if ((double)m.nIn >= m.nextReport) { m.nextReport += kPubSec * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
}

bool MeteorReceiver::telemetry(MeteorTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void MeteorReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning meteorTuning() {
    ModeTuning t;
    t.stdMode = 61; t.id = "meteor"; t.name = "Meteor-M LRPT";
    t.minMhz = 136; t.maxMhz = 139; t.defMhz = 137.9;
    t.sampleRate = 1e6; t.basebandHz = 0; t.bandwidthMhz = 0.15;
    t.minSampleRate = 250e3;
    t.tuneOffsetHz = 200e3;   // the 150 kHz signal clear of the DC spike
    return t;
}

const MeteorPreset* meteorPresets(int& count) {
    static const MeteorPreset p[] = {{"137.900 (N2-4, N2-3)", 137.9}, {"137.100", 137.1}};
    count = (int)(sizeof p / sizeof p[0]);
    return p;
}

} // namespace dect2
