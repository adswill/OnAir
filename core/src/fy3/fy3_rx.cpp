// FY-3 AHRPT and MPT receivers (see fy3_rx.h). The demodulator follows hrpt_rx.cpp (itself the Meteor LRPT one) with the changes for this signal:
// a symbol rate search, a soft-decision carrier detector with a narrowing loop, soft symbols scaled by a slow level, and the two-rail code.
// The channel coding and the instrument frame layouts follow the open FY-3 decoders (SatDump's fengyun3 plug-in, GPL-3.0-or-later); see THIRD_PARTY.md.
#include "dect2/fy3/fy3_rx.h"
#include "dect2/fy3/fy3_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/fast_sincos.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {

using cd = std::complex<double>;
using dect2::math::kPi;
constexpr double kPubSec = 0.25;
constexpr int kHist = 4096;             // decimated samples kept (power of two)
constexpr int kAcqLog2 = 10, kAcqN = 1 << kAcqLog2, kAcqBlocks = 48;
constexpr int kRateLog2 = 14, kRateN = 1 << kRateLog2, kRateBlocks = 96;
constexpr int kWideLog2 = 13, kWideN = 1 << kWideLog2, kWideBlocks = 40;
constexpr int kSearchSyms = 40000;      // symbols the code search looks at
// No signal: between two code searches the receiver waits 0.15 s of symbols (holdSyms). When the Es/N0 estimate also says there is
// nothing there (under -4 dB; the code needs about +3 dB and the estimate reads 0.05 - 0.16 on white noise, 1.0 at +1 dB), the
// demodulator runs on one block of input in eight during that wait and the rest goes by. The loops keep running on what they see; a
// signal that comes up lifts the estimate within a block and the receiver runs on every sample again.
constexpr int kIdleBlock = 32768;        // input samples processed per idle cycle
constexpr int kIdleSkip = 7;             // blocks let go by after each one processed
constexpr float kIdleSnr = 0.4f;
constexpr int64_t kWarmSyms = 20000;     // symbols run after a skip before the next code search collects any
constexpr double kSearchHzLband = 75000; // coarse carrier search range (L-band Doppler is up to about +-40 kHz)

double rrcTap(double t, double a) {     // t in symbols
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / kPi;
    if (std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    return (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) / (kPi * t * (1 - 16 * a * a * t * t));
}

struct Fir {   // decimating FIR on a doubled ring, real and imaginary parts apart and the taps padded to 8 so the loop vectorizes
    std::vector<float> h, bre, bim;
    int n = 1, np = 8, pos = 0, d = 1, ph = 0;
    void init(const std::vector<float>& taps, int dec) {
        n = (int)taps.size(); np = (n + 7) / 8 * 8;
        h.assign((size_t)np, 0.f);
        for (int i = 0; i < n; i++) h[(size_t)i] = taps[(size_t)i];
        bre.assign(2 * (size_t)np, 0.f); bim.assign(2 * (size_t)np, 0.f);
        d = dec; pos = 0; ph = 0;
    }
    bool push(cf32 x, cf32& y) {
        bre[(size_t)pos] = bre[(size_t)(pos + np)] = x.real();
        bim[(size_t)pos] = bim[(size_t)(pos + np)] = x.imag();
        pos = pos + 1 == np ? 0 : pos + 1;
        if (++ph < d) return false;
        ph = 0;
        const float* br = &bre[(size_t)pos];
        const float* bi = &bim[(size_t)pos];
        float ar[8] = {}, ai[8] = {};
        for (int i = 0; i < np; i += 8)
            for (int k = 0; k < 8; k++) { ar[k] += br[i + k] * h[(size_t)(i + k)]; ai[k] += bi[i + k] * h[(size_t)(i + k)]; }
        float re = 0, im = 0;
        for (int k = 0; k < 8; k++) { re += ar[k]; im += ai[k]; }
        y = cf32(re, im);
        return true;
    }
};

// Interpolation by two for a signal sampled at less than 2.8 samples a symbol: every input sample is followed by the value half a sample later
// (a windowed sinc over 32 samples); the even outputs are the input itself, so the cost is one 32 tap filter a sample.
struct Interp2 {
    static constexpr int L = 16;
    float g[2 * L] = {};
    std::vector<float> bre, bim;
    int pos = 0;
    void init() {
        double sum = 0;
        for (int k = 0; k < 2 * L; k++) {
            const double d = (L - 0.5) - k, r = d / L;
            const double w = 0.42 + 0.5 * std::cos(kPi * r) + 0.08 * std::cos(2 * kPi * r);
            g[k] = (float)(std::sin(kPi * d) / (kPi * d) * w); sum += g[k];
        }
        for (int k = 0; k < 2 * L; k++) g[k] = (float)(g[k] / sum);
        bre.assign(4 * L, 0.f); bim.assign(4 * L, 0.f); pos = 0;
    }
    void push(cf32 x, cf32& y0, cf32& y1) {
        bre[(size_t)pos] = bre[(size_t)(pos + 2 * L)] = x.real();
        bim[(size_t)pos] = bim[(size_t)(pos + 2 * L)] = x.imag();
        pos = pos + 1 == 2 * L ? 0 : pos + 1;
        const float* br = &bre[(size_t)pos];
        const float* bi = &bim[(size_t)pos];
        float ar[8] = {}, ai[8] = {};
        for (int i = 0; i < 2 * L; i += 8)
            for (int k = 0; k < 8; k++) { ar[k] += br[i + k] * g[i + k]; ai[k] += bi[i + k] * g[i + k]; }
        float re = 0, im = 0;
        for (int k = 0; k < 8; k++) { re += ar[k]; im += ai[k]; }
        y0 = cf32(br[L - 1], bi[L - 1]);
        y1 = cf32(re, im);
    }
};

inline int8_t sat8(float v) { return (int8_t)std::clamp((int)std::lround(v), -127, 127); }
inline float fastTanh(float x) {   // x (27 + x^2) / (27 + 9 x^2) is within 2% of tanh up to |x| = 3; beyond it the value is 1
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

struct Cand { double rs, alpha; };
// AHRPT: FY-3A / B 1704.5 MHz 2.8 Msym/s, FY-3C 1701.4 MHz 2.6, FY-3G X-band 6, FY-3D 30, FY-3E / F / H 38.4. MPT: 18.7 Msym/s
const Cand kAhrptCands[] = {{2.6e6, 0.5}, {2.8e6, 0.5}, {6e6, 0.35}, {30e6, 0.35}, {38.4e6, 0.35}};
const Cand kMptCands[] = {{18.7e6, 0.5}};

constexpr uint32_t kAsm = meteor::kAsm;

} // namespace

double fy3MinSampleRate(double rs, double alpha) { return rs * (1 + alpha) * 1.04; }
static double workRate(double rs) { return rs * 1.06; }   // below the full width of the signal the skirts are cut; the receiver still runs from here up

int fy3Candidates(int product, double rate, double* rs, double* alpha, int max) {
    const Cand* c = product ? kMptCands : kAhrptCands;
    const int n = product ? 1 : 5;
    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (rate >= workRate(c[i].rs)) { rs[k] = c[i].rs; alpha[k] = c[i].alpha; k++; }
    return k;
}

const Fy3Preset* fy3Presets(int product, int& count) {
    static const Fy3Preset a[] = {
        {"1701.400 FY-3C AHRPT (2.6 Msym/s)", 1701.4, 2.6e6, 0.5, 0, "FY-3C"},
        {"1704.500 FY-3A / FY-3B AHRPT (2.8 Msym/s)", 1704.5, 2.8e6, 0.5, 0, "FY-3A, FY-3B"},
        {"7790 FY-3G (6 Msym/s, X-band, needs a down-converter)", 7790.0, 6e6, 0.35, 0, "FY-3G"},
        {"7820 FY-3D (30 Msym/s, X-band, needs a down-converter and 40 Msps)", 7820.0, 30e6, 0.35, 0, "FY-3D"},
        {"7790 FY-3F (38.4 Msym/s, X-band)", 7790.0, 38.4e6, 0.35, 0, "FY-3F"},
        {"7860 FY-3E / FY-3H (38.4 Msym/s, X-band)", 7860.0, 38.4e6, 0.35, 0, "FY-3E, FY-3H"}};
    static const Fy3Preset m[] = {
        {"7750 FY-3A / FY-3B MPT (18.7 Msym/s, X-band, needs a down-converter and 28 Msps)", 7750.0, 18.7e6, 0.5, 1, "FY-3A, FY-3B"},
        {"7780 FY-3C MPT (18.7 Msym/s, X-band)", 7780.0, 18.7e6, 0.5, 1, "FY-3C"}};
    count = product ? 2 : 6;
    return product ? m : a;
}

ModeTuning fy3AhrptTuning() {
    ModeTuning t;
    t.stdMode = 237; t.id = "fy3ahrpt"; t.name = "FY-3 AHRPT";
    t.minMhz = 1690; t.maxMhz = 1710; t.defMhz = 1701.4;
    t.sampleRate = 6e6; t.basebandHz = 0; t.bandwidthMhz = 4.5;
    t.minSampleRate = 4e6;
    t.tuneOffsetHz = 0;
    return t;
}
ModeTuning fy3MptTuning() {
    ModeTuning t;
    t.stdMode = 238; t.id = "fy3mpt"; t.name = "FY-3 MPT";
    t.minMhz = 7700; t.maxMhz = 7850; t.defMhz = 7750;
    t.sampleRate = 20e6; t.basebandHz = 0; t.bandwidthMhz = 28;
    t.minSampleRate = 20e6;
    t.tuneOffsetHz = 0;
    return t;
}

struct Fy3Receiver::Impl {
    const int prod;
    std::mutex mu;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<int> presetSet{0};
    double rateSet = 0, offsetSet = 0, rsSet = 0, alphaSet = 0;
    std::function<void(const std::string&)> log;

    // run settings
    double rate = 0, offset = 0;
    int presetRun = 0, autoIdx = 0;
    double rsUser = 0, alphaUser = 0;
    double rs = 2.6e6, alpha = 0.5, rsMeasured = 0;
    int stage = 0;                       // 0 measure the symbol rate, 1 carrier search, 2 tracking

    // front end
    cd rot{1, 0}, step{1, 0};
    double coarseHz = 0;
    double dcRe = 0, dcIm = 0;
    int dec = 1, up = 1;
    double fd = 0, sps = 4, rateEff = 0;
    Interp2 upf;
    Fir lpf, rrc;
    std::vector<cf32> hist;
    int64_t hN = 0;
    double tau = 0, spsEst = 4;
    double theta = 0, omega = 0;
    double lalpha = 0, lbeta = 0, kp = 0, ki = 0;
    float amp = 0.1f, ampSlow = 0.1f;
    float prevI = 0, prevQ = 0;
    int64_t nSym = 0, symAbs = 0;
    int loopStage = 0;
    double dbgE = 0; int64_t dbgN = 0;
    const bool kDebug2 = getenv("FY3_DEBUG2") != nullptr;   // the angle error print (an atan2 a symbol: only when asked for)
    float lockAvg = 0, m2 = 0, m4 = 0, snrLin = 1;
    float iqZr = 0, iqZi = 0, iqPf = 0, iqWr = 0, iqWi = 0, iqInv = 1;   // blind IQ correction: E[y^2] / E[|y|^2]
    uint32_t iqN = 0;
    // wide carrier search: where in the whole capture the signal sits (the capture may be much wider than the signal, and the signal anywhere in it)
    bool wideOn = false;
    std::vector<float> wRe, wIm, wpsd;
    int wFill = 0, wBlocks = 0;
    // rate estimate and coarse carrier
    std::vector<float> acqRe, acqIm, psd;
    int acqFill = 0, acqBlocks = 0;
    bool coarseDone = false;
    std::vector<float> rtRe, rtIm, rpsd;
    int rtFill = 0, rtBlocks = 0;
    double rtMean = 0;

    // code search and the live chain
    std::vector<int8_t> sbI, sbQ;
    int64_t bufStartAbs = 0;
    int64_t holdSyms = 0;
    bool railLocked = false;
    int rail_a = 0, rail_t[2] = {0, 0}, hyp = 0;
    int phaseAbs = 0;
    int8_t firstI = 0, firstQ = 0;
    bool haveFirst = false;
    fy3::RailViterbi vit[2];
    uint32_t sr = 0;
    bool inFrame = false, inv = false, frameLock = false;
    int frameBits = 0, misses = 0;
    uint8_t frame[meteor::kCodedBytes];
    int64_t sinceFrame = 0;
    double lastGoodSec = -1e9, lastAsmSec = -1e9, startSec = 0, lockLostSec = 0, railSec = -1e9;
    float railQ[2] = {};

    // instruments
    struct Stream {
        std::unique_ptr<fy3::FrameSync> sync;
        std::unique_ptr<fy3::MersiReader> mersi;
        int64_t lastCounter = -1;
        meteor::PacketDemux demux;
    };
    std::map<int, Stream> streams;       // by virtual channel
    struct ImgState {
        Fy3Image img;
        bool open = false;
        int keep = 0;
        bool haveT = false;
        double t0 = 0;
        int lastSeg = -1;
        bool haveLine = false;
        uint32_t lastMs = 0, lastDay = 0;
        int lastIdx = 0;
    };
    std::map<int, ImgState> imgs;        // by instrument
    uint64_t imgId = 0;
    bool imgDirty = false;
    std::map<int, Fy3InstStat> instStat;

    // telemetry
    Fy3Telemetry tel, pub;
    uint64_t seq = 0;
    int64_t nIn = 0;
    int64_t idleLeft = 0, idleRun = 0;   // input samples still to let go by; processed since the last skip
    uint64_t idleTotal = 0;              // input samples let go by (tests)
    bool idled = false;
    bool idle() const { return !railLocked && holdSyms > kWarmSyms && stage >= 1 && coarseDone && !wideOn && nSym > 4000 && snrLin < kIdleSnr; }
    double nextReport = 0, power = 0;
    int64_t nPower = 0;
    std::vector<std::pair<float, float>> constel;
    size_t constelPos = 0;

    explicit Impl(int p) : prod(p) {}

    double nowSec() const { return rate > 0 ? (double)nIn / rate : 0; }
    void say(const std::string& s) { if (log) log(std::string(prod ? "FY-3 MPT: " : "FY-3 AHRPT: ") + s); }

    // --- set-up ---------------------------------------------------------------------------------------------------------------------
    // The symbol rate and roll-off to run: a user rate, a preset, or the first candidate that fits (the measured one replaces it in stage 0).
    void pickRate() {
        if (rsUser > 0) {
            rs = rsUser;
            alpha = alphaUser > 0 ? alphaUser : (rs < 10e6 ? 0.5 : 0.35);
            int n; const Fy3Preset* p = fy3Presets(prod, n);
            for (int i = 0; i < n; i++) if (std::fabs(p[i].symbolRate - rs) < 0.01 * rs) { alpha = alphaUser > 0 ? alphaUser : p[i].rolloff; break; }
        } else if (presetRun > 0) {
            int n; const Fy3Preset* p = fy3Presets(prod, n);
            const Fy3Preset& q = p[std::clamp(presetRun - 1, 0, n - 1)];
            rs = q.symbolRate; alpha = alphaUser > 0 ? alphaUser : q.rolloff;
        } else {
            double r[8], a[8];
            const int n = fy3Candidates(prod, rate, r, a, 8);
            if (n > 0) { rs = r[autoIdx % n]; alpha = alphaUser > 0 ? alphaUser : a[autoIdx % n]; }
            else { const Cand& c = prod ? kMptCands[0] : kAhrptCands[0]; rs = c.rs; alpha = c.alpha; }
        }
    }

    bool rateFits() const { return rate >= workRate(rs) - 1; }

    // Is the symbol rate to be measured (automatic, more than one candidate that fits, a sample rate that holds the envelope line)?
    bool wantRateSearch(double* maxRs) const {
        if (rsUser > 0 || presetRun > 0) return false;
        double r[8], a[8];
        const int n = fy3Candidates(prod, rate, r, a, 8);
        int ok = 0; double mx = 0;
        for (int i = 0; i < n; i++) if (rate >= 1.1 * r[i]) { ok++; mx = std::max(mx, r[i] * (1 + a[i])); }
        if (ok < 2 && prod == 1) return false;   // MPT has one rate: nothing to choose
        if (ok < 1) return false;
        if (maxRs) *maxRs = mx;
        return true;
    }

    void designFilters(double rsDesign, double alphaDesign) {
        // below 2.8 samples a symbol the timing interpolator is too coarse: the signal is first interpolated to twice the rate
        up = rate < 2.8 * rsDesign ? 2 : 1;
        rateEff = rate * up;
        if (up == 2) upf.init();
        dec = std::max(1, (int)std::floor(rateEff / (4 * rsDesign)));
        fd = rateEff / dec;
        sps = fd / rsDesign;
        const double bw1 = rsDesign * (1 + alphaDesign) / 2;
        const double df = std::max(fd - 2 * bw1, 0.2 * fd);
        int n = 2 * (int)std::ceil(3.5 * rateEff / df / 2) + 1;
        n = std::clamp(n, 9, 401);
        std::vector<float> h((size_t)n);
        double sum = 0;
        const double fc = (fd / 2) / rateEff;
        for (int i = 0; i < n; i++) {
            const double t = i - (n - 1) / 2.0;
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1));
            const double s = std::fabs(t) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
            h[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& v : h) v = (float)(v / sum);
        if (dec == 1) h.assign(1, 1.f);
        lpf.init(h, dec);
    }

    void initDemod() {
        if (rate <= 0) return;
        double mx = 0;
        const bool search = stage == 0 && wantRateSearch(&mx);
        if (stage == 0 && !search) stage = 1;
        if (search) {
            designFilters(mx / 1.25, 0.25);   // a filter that passes the widest candidate
            rtRe.assign(kRateN, 0); rtIm.assign(kRateN, 0); rpsd.assign(kRateN, 0);
            rtFill = 0; rtBlocks = 0; rtMean = 0;
        } else {
            designFilters(rs, alpha);
            const int span = 5;
            const int m = 2 * (int)std::ceil(span * sps) + 1;
            std::vector<float> g((size_t)m);
            double e = 0;
            for (int i = 0; i < m; i++) { const double t = (i - (m - 1) / 2.0) / sps; g[(size_t)i] = (float)rrcTap(t, alpha); e += g[(size_t)i]; }
            for (auto& v : g) v = (float)(v / e);
            rrc.init(g, 1);
        }
        hist.assign(kHist, cf32(0, 0));
        hN = 0;
        tau = 8; spsEst = sps;
        theta = 0; omega = 0;
        amp = 0; ampSlow = 0; prevI = prevQ = 0; nSym = 0; lockAvg = 0; m2 = m4 = 0; snrLin = 1;
        loopStage = 0;
        loopGains(0.02);
        kp = 0.02 * sps; ki = 2e-5 * sps;
        coarseDone = false; acqFill = 0; acqBlocks = 0;
        acqRe.assign(kAcqN, 0); acqIm.assign(kAcqN, 0); psd.assign(kAcqN, 0);
        step = std::polar(1.0, -2 * kPi * (offset + coarseHz) / rate);
        resetChain();
        // when the capture is wider than the signal by more than the coarse search reaches, the signal is first looked for in the whole of it
        const double width = (rsDesignWide > 0 ? rsDesignWide : rs) * (1 + alpha);
        wideOn = rate > width + 0.5e6 && stage <= 1 && !wideDone;
        if (wideOn) { wRe.assign(kWideN, 0); wIm.assign(kWideN, 0); wpsd.assign(kWideN, 0); wFill = 0; wBlocks = 0; }
    }
    bool wideDone = false, wideUsed = false;
    double rsDesignWide = 0;

    void loopGains(double bn) {
        const double z = 0.707, th = bn / (z + 1 / (4 * z)), d = 1 + 2 * z * th + th * th;
        lalpha = 4 * z * th / d; lbeta = 4 * th * th / d;
    }

    void resetChain() {
        sbI.clear(); sbQ.clear(); sbI.reserve(kSearchSyms); sbQ.reserve(kSearchSyms);
        holdSyms = 0; railLocked = false; haveFirst = false;
        vit[0].reset(); vit[1].reset();
        sr = 0; inFrame = false; frameLock = false; frameBits = 0; misses = 0; sinceFrame = 0;
        railQ[0] = railQ[1] = 0;
        tel.railLock = false; tel.railPhase[0] = tel.railPhase[1] = -1; tel.diffMode = -1;
    }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; rsUser = rsSet; alphaUser = alphaSet; }
        presetRun = presetSet.load();
        autoIdx = 0; rsMeasured = 0;
        idleLeft = 0; idleRun = 0; idled = false;
        stage = 0;
        pickRate();
        coarseHz = 0; wideDone = false; wideUsed = false; rsDesignWide = 0;
        rot = cd(1, 0);
        dcRe = dcIm = 0; iqZr = iqZi = iqPf = iqWr = iqWi = 0; iqInv = 1;
        initDemod();
        startSec = nowSec(); lastGoodSec = -1e9; lastAsmSec = -1e9;
        tel.satSetting = presetRun;
        tel.product = prod;
        if (rate > 0 && !rateFits()) say("the sample rate is too low for " + std::to_string(rs / 1e6) + " Msym/s: " + std::to_string(workRate(rs) / 1e6) + " Msps at the least, " + std::to_string(fy3MinSampleRate(rs, alpha) / 1e6) + " Msps for the whole signal");
    }

    void newImages() {
        imgs.clear();
        for (auto& s : streams) if (s.second.mersi) s.second.mersi->flush();
        for (auto& kv : instStat) kv.second.units = 0;
        tel.lines = 0;
        imgDirty = true;
    }

    // --- symbol rate: the line at the symbol rate in the spectrum of the signal power -----------------------------------------------
    void rateSample(cf32 y) {
        const float p = y.real() * y.real() + y.imag() * y.imag();
        rtRe[(size_t)rtFill] = p; rtIm[(size_t)rtFill] = 0;
        if (++rtFill < kRateN) return;
        rtFill = 0;
        double mean = 0;
        for (int i = 0; i < kRateN; i++) mean += rtRe[(size_t)i];
        mean /= kRateN;
        for (int i = 0; i < kRateN; i++) {
            const float w = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kRateN));
            rtRe[(size_t)i] = (float)((rtRe[(size_t)i] - mean) * w); rtIm[(size_t)i] = 0;
        }
        fftSplit(rtRe.data(), rtIm.data(), kRateLog2, false);
        for (int i = 0; i < kRateN / 2; i++) rpsd[(size_t)i] += rtRe[(size_t)i] * rtRe[(size_t)i] + rtIm[(size_t)i] * rtIm[(size_t)i];
        if (++rtBlocks < kRateBlocks) return;
        finishRate();
    }

    void finishRate() {
        double r[8], a[8];
        const int n = fy3Candidates(prod, rate, r, a, 8);
        const double bin = fd / kRateN;
        double bestRatio = 0, bestRs = 0, bestAlpha = alpha;
        int bestIdx = -1;
        for (int c = 0; c < n; c++) {
            if (rate < 1.1 * r[c] || r[c] * 1.05 >= fd / 2) continue;
            const int lo = std::max(2, (int)std::floor(r[c] * 0.97 / bin)), hi = std::min(kRateN / 2 - 2, (int)std::ceil(r[c] * 1.03 / bin));
            if (hi <= lo + 2) continue;
            int pk = lo; double pv = -1;
            for (int k = lo; k <= hi; k++) if (rpsd[(size_t)k] > pv) { pv = rpsd[(size_t)k]; pk = k; }
            // the floor around: the bins of 0.5 .. 1.5 symbol rates but the 5% around the peak
            double fl = 0; int cnt = 0;
            const int fl0 = std::max(2, (int)(r[c] * 0.5 / bin)), fl1 = std::min(kRateN / 2 - 1, (int)(r[c] * 1.4 / bin)), ex = std::max(3, (int)(r[c] * 0.05 / bin));
            for (int k = fl0; k <= fl1; k++) { if (std::abs(k - pk) <= ex) continue; fl += rpsd[(size_t)k]; cnt++; }
            if (cnt < 8) continue;
            fl /= cnt;
            const double ratio = pv / std::max(fl, 1e-30);
            double frac = 0;
            const double l = rpsd[(size_t)pk - 1], m = rpsd[(size_t)pk], u = rpsd[(size_t)pk + 1];
            const double den = l - 2 * m + u;
            if (den < 0) frac = std::clamp(0.5 * (l - u) / den, -0.5, 0.5);
            if (ratio > bestRatio) { bestRatio = ratio; bestRs = (pk + frac) * bin; bestAlpha = a[c]; bestIdx = c; }
        }
        if (bestIdx >= 0 && bestRatio > 2.5) {
            // within 0.3% of a candidate: that candidate (its exact rate); else what was measured (a rate of its own)
            const double snap = r[bestIdx];
            rsMeasured = std::fabs(bestRs - snap) < 0.003 * snap ? snap : bestRs;
            rs = rsMeasured; alpha = alphaUser > 0 ? alphaUser : bestAlpha;
            char b[160];
            snprintf(b, sizeof b, "symbol rate %.4f Msym/s (line %.0fx above the floor)", rs / 1e6, bestRatio);
            say(b);
        } else {
            autoIdx++;
            pickRate();
            rsMeasured = 0;
            say("no symbol rate line found; trying " + std::to_string(rs / 1e6) + " Msym/s");
        }
        stage = 1;
        initDemod();   // (the carrier the wide search found stays)
        startSec = nowSec();
    }

    // --- coarse carrier: the raised-cosine shape fitted to the averaged power spectrum ---------------------------------------------------
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
        const double f1 = rs * (1 - alpha) / 2, f2 = rs * (1 + alpha) / 2;
        // after the wide search only a rough place is known (the spectrum is flat over most of the signal's width): look within a third of the symbol rate
        const double searchHz = wideUsed ? 0.35 * rs : std::max(kSearchHzLband, 0.04 * rs);
        const int maxShift = std::max(1, (int)std::min(searchHz / bin, (fd / 2 - f2) / bin));
        std::vector<double> c((size_t)(2 * maxShift + 1));
        for (int s = -maxShift; s <= maxShift; s++) {
            double acc = 0;
            for (int k = -kAcqN / 2; k < kAcqN / 2; k++) {
                const double f = std::fabs((k - s) * bin);
                if (f >= f2) continue;
                const double tpl = f <= f1 ? 1.0 : 0.5 * (1 + std::cos(kPi * (f - f1) / (rs * alpha)));
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
        theta = 0; omega = 0; nSym = 0; loopStage = 0; loopGains(0.02);
    }

    // --- wide carrier search: the raised-cosine shape fitted to the power spectrum of the whole capture ----------------------------------------
    void wideSample(cf32 x) {
        wRe[(size_t)wFill] = x.real(); wIm[(size_t)wFill] = x.imag();
        if (++wFill < kWideN) return;
        wFill = 0;
        for (int i = 0; i < kWideN; i++) { const float w = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kWideN)); wRe[(size_t)i] *= w; wIm[(size_t)i] *= w; }
        fftSplit(wRe.data(), wIm.data(), kWideLog2, false);
        for (int i = 0; i < kWideN; i++) wpsd[(size_t)i] += wRe[(size_t)i] * wRe[(size_t)i] + wIm[(size_t)i] * wIm[(size_t)i];
        if (++wBlocks < kWideBlocks) return;
        wideOn = false; wideDone = true;
        const double bin = rate / kWideN;
        // the template of the signal that is wanted: the symbol rate in use, or in the automatic mode the first candidate (2.6 and 2.8 Msym/s differ by 7%:
        // the centre is the same; the coarse search that follows has the measured rate)
        double tr = rs, ta = alpha;
        { double r[8], a[8]; const int n = fy3Candidates(prod, rate, r, a, 8); if (rsUser <= 0 && presetRun == 0 && n > 0) { tr = r[0]; ta = a[0]; } }
        const double f1 = tr * (1 - ta) / 2, f2 = tr * (1 + ta) / 2;
        const int maxShift = std::max(1, (int)std::min((rate / 2 - f2) / bin, (double)kWideN / 2 - 2));
        // a smoothed spectrum first: the sum over the template at each shift, by running over the bins it covers
        std::vector<double> c((size_t)(2 * maxShift + 1), 0.0);
        const int kf = (int)std::ceil(f2 / bin);
        std::vector<double> tpl((size_t)(2 * kf + 1));
        for (int k = -kf; k <= kf; k++) { const double f = std::fabs(k * bin); tpl[(size_t)(k + kf)] = f >= f2 ? 0.0 : f <= f1 ? 1.0 : 0.5 * (1 + std::cos(kPi * (f - f1) / (tr * ta))); }
        double mean = 0;
        for (int i = 0; i < kWideN; i++) mean += wpsd[(size_t)i];
        mean /= kWideN;
        for (int s = -maxShift; s <= maxShift; s++) {
            double acc = 0;
            for (int k = -kf; k <= kf; k++) acc += tpl[(size_t)(k + kf)] * wpsd[(size_t)(((k + s) % kWideN + kWideN) % kWideN)];
            c[(size_t)(s + maxShift)] = acc;
        }
        int bi = 0;
        double tsum = 0;
        for (double v : tpl) tsum += v;
        for (int i = 1; i < (int)c.size(); i++) if (c[(size_t)i] > c[(size_t)bi]) bi = i;
        double frac = 0;
        if (bi > 0 && bi + 1 < (int)c.size()) {
            const double a = c[(size_t)bi - 1], b = c[(size_t)bi], d = c[(size_t)bi + 1];
            const double den = a - 2 * b + d;
            if (den < 0) frac = 0.5 * (a - d) / den;
        }
        const double ratio = c[(size_t)bi] / std::max(1e-30, mean * tsum);
        const double est = (bi - maxShift + frac) * bin;
        std::fill(wpsd.begin(), wpsd.end(), 0.f);
        if (getenv("FY3_DEBUG")) fprintf(stderr, "wide: tr %.3g ta %.2f maxShift %d bin %.1f est %.1f kHz ratio %.2f offset %.1f\n", tr, ta, maxShift, bin, est / 1e3, ratio, offset);
        wideUsed = ratio > 1.15;
        if (ratio > 1.15) {
            coarseHz = est - offset;
            step = std::polar(1.0, -2 * kPi * (offset + coarseHz) / rate);
            char b[120];
            snprintf(b, sizeof b, "signal found %+.1f kHz from the centre of the capture", est / 1e3);
            say(b);
        }
    }

    // --- symbols ----------------------------------------------------------------------------------------------------------------------
    cf32 interp(double t) const {
        const int64_t i = (int64_t)std::floor(t);
        const float mu = (float)(t - (double)i);
        const cf32 xm = hist[(size_t)((i - 1) & (kHist - 1))], x0 = hist[(size_t)(i & (kHist - 1))], x1 = hist[(size_t)((i + 1) & (kHist - 1))],
                   x2 = hist[(size_t)((i + 2) & (kHist - 1))];
        const float cm = -mu * (mu - 1) * (mu - 2) / 6, c0 = (mu + 1) * (mu - 1) * (mu - 2) / 2, c1 = -(mu + 1) * mu * (mu - 2) / 2, c2 = (mu + 1) * mu * (mu - 1) / 6;
        return xm * cm + x0 * c0 + x1 * c1 + x2 * c2;
    }

    void decimated(cf32 y) {
        if (stage == 0) { rateSample(y); return; }
        // blind IQ imbalance correction: a circular signal has E[y^2] = 0; what is left of it is the image
        {
            const float re = y.real(), im = y.imag();
            iqZr += 2e-5f * (re * re - im * im - iqZr);
            iqZi += 2e-5f * (2 * re * im - iqZi);
            iqPf += 2e-5f * (re * re + im * im - iqPf);
            if ((++iqN & 255) == 0 && iqPf > 1e-20f) {
                float wr = iqZr / iqPf, wi = iqZi / iqPf;
                const float m = std::sqrt(wr * wr + wi * wi);
                if (m > 0.5f) { wr *= 0.5f / m; wi *= 0.5f / m; }
                iqWr = wr; iqWi = wi; iqInv = 1.f / (1.f - (wr * wr + wi * wi));
            }
            // y - w conj(y)
            const float ore = (re - (iqWr * re + iqWi * im)) * iqInv, oim = (im - (iqWi * re - iqWr * im)) * iqInv;
            y = cf32(ore, oim);
        }
        if (!coarseDone) { coarseSample(y); return; }
        cf32 z;
        rrc.push(y, z);
        hist[(size_t)(hN & (kHist - 1))] = z;
        hN++;
        if (amp <= 0) amp = 1e-9f;
        while (tau + spsEst / 2 + 3 < (double)hN) {
            if (tau - spsEst - 2 < (double)(hN - kHist)) { tau = (double)hN - spsEst; continue; }
            symbol();
        }
    }

    void symbol() {
        const double T = spsEst;
        const cf32 cs = FastSinCos::get().polar(1.f, theta);   // error 3e-7: a table, not cos and sin in double each symbol
        const float c0 = cs.real(), s0 = cs.imag();
        const float hw = (float)(omega / 2), ch = 1.f - hw * hw / 2, sh = hw;
        const cf32 r0(c0, -s0), r1(c0 * ch + s0 * sh, -(s0 * ch - c0 * sh));
        const cf32 a = interp(tau) * r0;
        const cf32 m = interp(tau - T / 2) * r1;
        const float I = a.real(), Q = a.imag();
        const float eT = (prevI - I) * m.real() + (prevQ - Q) * m.imag();
        prevI = I; prevQ = Q;
        const float mag = 0.5f * (std::fabs(I) + std::fabs(Q));
        if (nSym == 0) { amp = std::max(mag, 1e-9f); ampSlow = amp; }
        amp += 0.002f * (mag - amp);
        // the slow level: fades are not normalised away, so the soft values of a weak stretch stay small
        ampSlow += std::max(2e-5f, 1.f / (float)(nSym + 8)) * (mag - ampSlow);
        const float In = I / amp, Qn = Q / amp;
        // soft-decision carrier detector: tanh of the rails at the confidence the current SNR allows (hard decisions when it is high)
        const float k = std::clamp(2.f * snrLin, 1.f, 8.f);
        const float eC = std::clamp(fastTanh(k * In) * Qn - fastTanh(k * Qn) * In, -1.f, 1.f);
        if (kDebug2) { double ang = std::atan2((double)Qn, (double)In) - kPi / 4; ang = std::remainder(ang, kPi / 2); dbgE += ang * ang; dbgN++; }
        theta += omega + lalpha * eC;
        omega += lbeta * eC;
        omega = std::clamp(omega, -0.25, 0.25);
        if (theta > kPi) theta -= 2 * kPi; else if (theta < -kPi) theta += 2 * kPi;
        const float eTn = std::clamp(eT / (amp * amp), -1.f, 1.f);
        tau += T + kp * eTn;
        spsEst += ki * eTn;
        spsEst = std::clamp(spsEst, sps * 0.99, sps * 1.01);
        nSym++; symAbs++;
        // the carrier loop narrows as it settles: 0.02 to find it, 0.004, then 0.0012 (a LEO Doppler rate is under 100 Hz/s)
        if (loopStage == 0 && nSym >= (int64_t)(rs * 0.002)) { loopStage = 1; loopGains(0.004); }
        else if (loopStage == 1 && nSym >= (int64_t)(rs * 0.02)) { loopStage = 2; loopGains(0.0012); }
        const float r2 = In * In + Qn * Qn;
        if (r2 > 1e-6f) {
            const float re4 = (In * In - Qn * Qn) * (In * In - Qn * Qn) - 4 * In * In * Qn * Qn;
            lockAvg += 0.002f * (-re4 / (r2 * r2) - lockAvg);
        }
        // Es/N0 from the second and fourth moments of the constant-envelope symbols: S^2 = 2 M2^2 - M4
        {
            const float p = (I * I + Q * Q) / (amp * amp * 2.f);
            m2 += 0.0005f * (p - m2);
            m4 += 0.0005f * (p * p - m4);
            const float s2 = 2 * m2 * m2 - m4;
            if (s2 > 1e-9f && nSym > 2000) {
                const float S = std::sqrt(s2), N = std::max(m2 - S, 1e-6f);
                snrLin = std::clamp(S / N, 0.05f, 1000.f);
            }
        }
        if (nSym % 4 == 0) {
            if (constel.size() < 256) constel.emplace_back(In, Qn); else { constel[constelPos] = {In, Qn}; constelPos = (constelPos + 1) % 256; }
        }
        const float sc = 40.f / std::max(ampSlow, 1e-9f);
        afterSoft(sat8(I * sc), sat8(Q * sc));
    }

    // --- the two-rail code ---------------------------------------------------------------------------------------------------------------
    void afterSoft(int8_t i, int8_t q) {
        if (railLocked) { liveSymbol(i, q); return; }
        if (holdSyms > 0) { holdSyms--; return; }
        if (sbI.empty()) bufStartAbs = symAbs - 1;
        sbI.push_back(i); sbQ.push_back(q);
        if ((int)sbI.size() >= kSearchSyms) {
            railSearch();
            sbI.clear(); sbQ.clear();
            if (!railLocked) holdSyms = (int64_t)(rs * 0.15);
        }
    }

    // The pairing and polarity of each rail's code symbols from the path metrics, then the I / Q arrangement from the sync word in the decoded bits.
    void railSearch() {
        const int N = (int)sbI.size();
        // Pairings that differ by a half turn (both symbols negated) decode the same code with the data complemented: the differential decoder takes
        // that out, so of the 8 symmetries only 4 are different, and with the 2 alignments the search has 8 hypotheses a rail.
        static const int kT[4] = {0, 1, 4, 5};
        double sc[2][8];
        const std::vector<int8_t>* rail[2] = {&sbI, &sbQ};
        for (int r = 0; r < 2; r++)
            for (int a = 0; a < 2; a++) {
                double s = 0;
                for (int k = a; k + 1 < N; k += 2) s += std::abs((int)(*rail[r])[(size_t)k]) + std::abs((int)(*rail[r])[(size_t)k + 1]);
                const double nrm = std::max(1.0, s);
                for (int ti = 0; ti < 4; ti++) {
                    fy3::RailViterbi v;
                    for (int k = a; k + 1 < N; k += 2) {
                        int oa, ob;
                        fy3::pairXform(kT[ti], (*rail[r])[(size_t)k], (*rail[r])[(size_t)k + 1], oa, ob);
                        v.push(oa, ob);
                        if (v.out().size() > 8192) v.out().clear();
                    }
                    sc[r][a * 4 + ti] = v.score() / nrm;
                }
            }
        // best alignment: the one where both rails have a code
        double bestTot = -1e9; int ba = 0, bti[2] = {0, 0};
        for (int a = 0; a < 2; a++) {
            int t0 = 0, t1 = 0;
            for (int t = 1; t < 4; t++) { if (sc[0][a * 4 + t] > sc[0][a * 4 + t0]) t0 = t; if (sc[1][a * 4 + t] > sc[1][a * 4 + t1]) t1 = t; }
            const double tot = sc[0][a * 4 + t0] + sc[1][a * 4 + t1];
            if (tot > bestTot) { bestTot = tot; ba = a; bti[0] = t0; bti[1] = t1; }
        }
        int bt[2] = {kT[bti[0]], kT[bti[1]]};
        double q[2];
        bool found = true;
        for (int r = 0; r < 2; r++) {
            const int me = ba * 4 + bti[r];
            q[r] = sc[r][me];
            // the wrong pairings all decode noise and score alike (a random sequence follows 87% of its symbols); the right one must stand clear of that spread
            double mean = 0, var = 0, mx = -1e9;
            for (int i = 0; i < 8; i++) if (i != me) { mean += sc[r][i]; mx = std::max(mx, sc[r][i]); }
            mean /= 7;
            for (int i = 0; i < 8; i++) if (i != me) var += (sc[r][i] - mean) * (sc[r][i] - mean);
            const double sd = std::max(std::sqrt(var / 6), 0.0015);
            railQ[r] = (float)std::clamp((q[r] - mean) / 0.25, 0.0, 1.0);
            if (getenv("FY3_DEBUG")) fprintf(stderr, "rail %d: best a%d t%d q %.4f mean %.4f sd %.4f max %.4f\n", r, ba, bt[r], q[r], mean, sd, mx);
            if (!(q[r] - mx > 3 * sd && q[r] - mean > 8 * sd)) found = false;
        }
        if (!found) return;
        // decode the window with the pairing found and look for the sync word under the 8 arrangements of the two rails
        std::vector<uint8_t> bitsR[2];
        for (int r = 0; r < 2; r++) {
            fy3::RailViterbi v;
            for (int k = ba; k + 1 < N; k += 2) {
                int oa, ob;
                fy3::pairXform(bt[r], (*rail[r])[(size_t)k], (*rail[r])[(size_t)k + 1], oa, ob);
                v.push(oa, ob);
            }
            v.flush();
            bitsR[r] = v.out();
        }
        const size_t M = std::min(bitsR[0].size(), bitsR[1].size());
        int bestH = -1, bestPairs = 0;
        for (int h = 0; h < 8; h++) {
            fy3::DiffDecoder dd;
            std::vector<uint8_t> out;
            out.reserve(2 * M);
            for (size_t m = 0; m < M; m++) {
                int x = (h & 1) ? bitsR[1][m] : bitsR[0][m], y = (h & 1) ? bitsR[0][m] : bitsR[1][m];
                if (h & 2) y ^= 1;
                int hi, lo;
                dd.push(x, y, hi, lo);
                if (h & 4) std::swap(hi, lo);
                out.push_back((uint8_t)hi); out.push_back((uint8_t)lo);
            }
            std::vector<size_t> hits;
            uint32_t w = 0;
            for (size_t i = 0; i < out.size(); i++) {
                w = w << 1 | out[i];
                if (i < 31) continue;
                const int e = __builtin_popcount(w ^ kAsm);
                if (e <= 4 || 32 - e <= 4) hits.push_back(i);
            }
            int pairs = 0;
            for (size_t x = 0; x < hits.size(); x++)
                for (size_t y = x + 1; y < hits.size(); y++) if (hits[y] - hits[x] == 8192) pairs++;
            if (pairs > bestPairs) { bestPairs = pairs; bestH = h; }
        }
        if (bestH < 0) return;
        // locked: the live chain from here on
        railLocked = true; rail_a = ba; rail_t[0] = bt[0]; rail_t[1] = bt[1]; hyp = bestH;
        phaseAbs = (int)((bufStartAbs + ba) & 1);
        haveFirst = false;
        vit[0].reset(); vit[1].reset();
        for (int h = 0; h < 8; h++) { dd[h].reset(); srW[h] = 0; confirm[h] = 0; lastHit[h] = 0; }
        bitCnt = 0;
        sr = 0; inFrame = false; frameLock = false; frameBits = 0; misses = 0; sinceFrame = 0;
        railSec = nowSec(); lastAsmSec = railSec;
        tel.railLock = true; tel.railPhase[0] = bt[0] + 8 * ba; tel.railPhase[1] = bt[1] + 8 * ba; tel.diffMode = bestH;
        char b[200];
        snprintf(b, sizeof b, "code found (rails %d/%d, alignment %d, arrangement %d, metrics %.2f / %.2f)", bt[0], bt[1], ba, bestH, q[0], q[1]);
        say(b);
    }


    void liveSymbol(int8_t i, int8_t q) {
        const int64_t k = symAbs - 1;
        if ((int)(k & 1) == phaseAbs) { firstI = i; firstQ = q; haveFirst = true; return; }
        if (!haveFirst) return;
        haveFirst = false;
        int oa, ob;
        fy3::pairXform(rail_t[0], firstI, i, oa, ob); vit[0].push(oa, ob);
        fy3::pairXform(rail_t[1], firstQ, q, oa, ob); vit[1].push(oa, ob);
        auto& o0 = vit[0].out();
        auto& o1 = vit[1].out();
        const size_t n = std::min(o0.size(), o1.size());
        for (size_t m = 0; m < n; m++) {
            // all eight arrangements of the rails run side by side: the one in use feeds the frames, the others only watch for the sync word, so a slip of
            // the carrier loop by a quarter turn (which exchanges or inverts the rails) costs the frames of a few milliseconds, not a new search
            for (int h = 0; h < 8; h++) {
                int x = (h & 1) ? o1[m] : o0[m], y = (h & 1) ? o0[m] : o1[m];
                if (h & 2) y ^= 1;
                int hi, lo;
                dd[h].push(x, y, hi, lo);
                if (h & 4) std::swap(hi, lo);
                if (h == hyp) { frameBit((uint8_t)hi); frameBit((uint8_t)lo); }
                else { watchBit(h, hi); watchBit(h, lo); }
                if (h == hyp) { srW[h] = srW[h] << 2 | (uint32_t)(hi << 1 | lo); }
            }
            bitCnt += 2;
        }
        o0.erase(o0.begin(), o0.begin() + (std::ptrdiff_t)n);
        o1.erase(o1.begin(), o1.begin() + (std::ptrdiff_t)n);
    }

    fy3::DiffDecoder dd[8];
    uint32_t srW[8] = {};
    int64_t bitCnt = 0, lastHit[8] = {};
    int confirm[8] = {};
    void watchBit(int h, int bit) {
        srW[h] = srW[h] << 1 | (uint32_t)bit;
        const int64_t at = bitCnt + (int64_t)0;
        const int e = __builtin_popcount(srW[h] ^ kAsm), ei = __builtin_popcount(~srW[h] ^ kAsm);
        if (std::min(e, ei) > 4) return;
        if (lastHit[h] > 0 && at - lastHit[h] > 8192 + 4) confirm[h] = 0;
        if (lastHit[h] > 0 && std::llabs(at - lastHit[h] - 8192) <= 1) confirm[h]++;
        lastHit[h] = at;
        // the arrangement in use has lost its frames and this one finds them: take it
        const double now = nowSec();
        if (confirm[h] >= 2 && (!frameLock || now - lastGoodSec > 0.15)) {
            char b[120];
            snprintf(b, sizeof b, "rails re-arranged (%d -> %d)", hyp, h);
            say(b);
            hyp = h;
            tel.diffMode = h;
            sr = srW[h]; inFrame = false; frameLock = false; misses = 0; sinceFrame = 0;
            for (int k = 0; k < 8; k++) confirm[k] = 0;
        }
    }

    // --- frames ----------------------------------------------------------------------------------------------------------------------
    void frameBit(uint8_t bit) {
        sr = sr << 1 | bit;
        sinceFrame++;
        if (inFrame) {
            const uint8_t v = (uint8_t)(bit ^ (inv ? 1 : 0));
            uint8_t& by = frame[frameBits >> 3];
            by = (uint8_t)(by << 1 | v);
            if (++frameBits == meteor::kCodedBytes * 8) { inFrame = false; gotFrame(); sinceFrame = 0; }
            return;
        }
        const int e = __builtin_popcount(sr ^ kAsm), ei = __builtin_popcount(~sr ^ kAsm);
        if (frameLock) {
            if (sinceFrame < 32) return;
            if (std::min(e, ei) <= 10) misses = 0; else misses++;
            if (misses > 4) { frameLock = false; say("frame sync lost"); }
            else { if (std::min(e, ei) <= 10) inv = ei < e; startFrame(); tel.asmHits++; lastAsmSec = nowSec(); return; }
        }
        if (e <= 3 || ei <= 3) { inv = ei < e; startFrame(); frameLock = true; misses = 0; tel.asmHits++; lastAsmSec = nowSec(); }
    }

    void startFrame() { inFrame = true; frameBits = 0; std::memset(frame, 0, sizeof frame); }

    void gotFrame() {
        fy3::Cadu c;
        if (!fy3::decodeCadu(frame, c)) { tel.blocksBad++; return; }
        tel.blocksOk++; tel.rsCorrected += (uint64_t)c.rsErrors;
        lastGoodSec = nowSec();
        onCadu(c);
    }

    // --- virtual channels and instruments ----------------------------------------------------------------------------------------------
    ImgState& image(int inst) {
        auto it = imgs.find(inst);
        if (it != imgs.end()) return it->second;
        ImgState& I = imgs[inst];
        I.img.id = ++imgId;
        I.img.inst = inst;
        I.img.width = fy3::instWidth(inst);
        const int nch = fy3::instChannels(inst);
        if (inst == fy3::Virr || inst == fy3::Mwri || inst == fy3::Mwri2) I.keep = nch;
        else { const fy3::MersiCfg& m = fy3::mersiCfg(inst); I.keep = std::min(nch, m.ch250 ? m.ch250 + 6 : 8); }
        I.img.channels = I.keep;
        return I;
    }

    void putRow(ImgState& I, int idx, uint32_t ms, std::vector<uint8_t>&& px) {
        if (idx < 0 || idx >= kFy3MaxRows) return;
        auto r = std::make_shared<Fy3Row>();
        r->idx = idx; r->ms = ms; r->px = std::move(px);
        if ((int)I.img.row.size() <= idx) I.img.row.resize((size_t)idx + 1);
        I.img.row[(size_t)idx] = r;
        I.img.rows = std::max(I.img.rows, idx + 1);
        imgDirty = true;
    }

    void placeVirr(const fy3::VirrLine& L) {
        ImgState& I = image(fy3::Virr);
        int idx = 0;
        if (I.haveLine) {
            double d = (double)L.ms - (double)I.lastMs;
            if (d < -43200000.0) d += 86400000.0;
            const long n = std::lround(d / fy3::kVirrLineMs);
            if (n < 0) return;
            idx = n > 20000 ? I.lastIdx + 1 : I.lastIdx + (int)n;
        }
        I.haveLine = true; I.lastMs = L.ms; I.lastIdx = idx;
        tel.lines++; tel.onboardMs = L.ms;
        instStat[fy3::Virr].units++;
        std::vector<uint8_t> px((size_t)fy3::kVirrChannels * fy3::kVirrWidth);
        for (size_t i = 0; i < px.size(); i++) px[i] = (uint8_t)(L.px[i] >> 2);
        putRow(I, idx, L.ms, std::move(px));
    }

    void placeMersi(int inst, const fy3::MersiSegment& s) {
        ImgState& I = image(inst);
        int seg;
        if (s.time > 0 && I.haveT) {
            seg = (int)std::lround((s.time - I.t0) / 1.5);
            if (seg <= I.lastSeg || seg > I.lastSeg + 400) seg = I.lastSeg + 1;
        } else {
            if (s.time > 0) { I.haveT = true; I.t0 = s.time; }
            seg = I.lastSeg + 1;
        }
        I.lastSeg = seg;
        const int w = I.img.width, nch = fy3::instChannels(inst);
        const uint32_t ms = s.time > 0 ? (uint32_t)(std::fmod(s.time, 86400.0) * 1000.0) : 0;
        if (s.time > 0) tel.onboardMs = ms;
        tel.lines += (uint64_t)s.lines;
        instStat[inst].units += (uint64_t)s.lines;
        for (int r = 0; r < 10; r++) {
            std::vector<uint8_t> px((size_t)I.keep * (size_t)w);
            for (int c = 0; c < I.keep; c++) std::memcpy(&px[(size_t)c * (size_t)w], &s.px[((size_t)r * (size_t)nch + (size_t)c) * (size_t)w], (size_t)w);
            putRow(I, seg * 10 + r, ms, std::move(px));
        }
    }

    void placeMwri(int inst, const fy3::MwriLine& L) {
        ImgState& I = image(inst);
        const fy3::MwriCfg& mc = fy3::mwriCfg(inst);
        int idx;
        if (L.time > 0 && I.haveT) {
            idx = (int)std::lround((L.time - I.t0) / mc.periodSec);
            if (idx <= I.lastSeg || idx > I.lastSeg + 400) idx = I.lastSeg + 1;
        } else {
            if (L.time > 0) { I.haveT = true; I.t0 = L.time; }
            idx = I.lastSeg + 1;
        }
        I.lastSeg = idx;
        const uint32_t ms = L.time > 0 ? (uint32_t)(std::fmod(L.time, 86400.0) * 1000.0) : 0;
        if (L.time > 0) tel.onboardMs = ms;
        tel.lines++; instStat[inst].units++;
        std::vector<uint8_t> px(L.px.size());
        for (size_t i = 0; i < px.size(); i++) px[i] = (uint8_t)(L.px[i] >> 8);
        putRow(I, idx, ms, std::move(px));
    }

    void onCadu(const fy3::Cadu& c) {
        tel.vcdus++; tel.scid = c.scid; tel.vcid = c.vcid; tel.vcduCounter = c.counter;
        tel.vcidFrames[c.vcid & 63]++;
        const int si = fy3::satIndexByScid(c.scid);
        if (si >= 0) tel.sat = si;
        if (c.vcid == 63) return;   // fill
        const int inst = fy3::instForVcid(c.scid, c.vcid, prod);
        Stream& S = streams[c.vcid];
        if (S.lastCounter >= 0) {
            const int64_t gap = ((int64_t)c.counter - S.lastCounter - 1) & 0xFFFFFF;
            if (gap > 0 && gap < 5000) {
                if (S.sync) S.sync->gap((size_t)gap * 882);
                if (S.mersi) S.mersi->gap((size_t)gap * 882);
            }
        }
        S.lastCounter = c.counter;
        if (inst) { Fy3InstStat& st = instStat[inst]; st.inst = inst; st.frames++; st.image = fy3::instIsImage(inst); }
        if (inst == fy3::Virr) {
            if (!S.sync) S.sync = std::make_unique<fy3::FrameSync>(fy3::kVirrSync, 60, fy3::kVirrFrameBytes * 8);
            S.sync->push(c.data, 882, [this](const std::vector<uint8_t>& f) {
                fy3::VirrLine L;
                if (fy3::decodeVirrFrame(f.data(), f.size(), L)) placeVirr(L);
            });
        } else if (inst == fy3::Mersi1 || inst == fy3::Mersi2 || inst == fy3::MersiLL || inst == fy3::Mersi3 || inst == fy3::MersiRM) {
            if (!S.mersi) {
                S.mersi = std::make_unique<fy3::MersiReader>(inst);
                S.mersi->onSegment = [this, inst](const fy3::MersiSegment& s) { placeMersi(inst, s); };
            }
            S.mersi->push(c.data, 882);
        } else if (inst == fy3::Mwri || inst == fy3::Mwri2) {
            const fy3::MwriCfg& mc = fy3::mwriCfg(inst);
            if (!S.sync) S.sync = std::make_unique<fy3::FrameSync>(mc.sync, mc.syncBits, mc.frameBytes * 8);
            S.sync->push(c.data, 882, [this, inst](const std::vector<uint8_t>& f) {
                fy3::MwriLine L;
                if (fy3::decodeMwriFrame(inst, f.data(), f.size(), L)) placeMwri(inst, L);
            });
        } else if (c.vcid == 12) {
            if (!S.demux.onPacket) S.demux.onPacket = [this](const meteor::Packet& p) {
                const int ins = p.apid == 16 ? fy3::Mwhs : (p.apid == 2 || p.apid == 7) ? fy3::Mwts : p.apid == 5 ? fy3::Erm : 0;
                if (!ins) return;
                Fy3InstStat& st = instStat[ins]; st.inst = ins; st.units++;
            };
            meteor::Vcdu v;
            v.version = 1; v.scid = c.scid; v.vcid = c.vcid; v.counter = c.counter; v.fhp = c.fhp; v.data = c.data;
            if (c.fhp == 0x7FF || c.fhp < 882) S.demux.push(v);
        }
    }

    // --- supervision: lost code, lost carrier, no signal ---------------------------------------------------------------------------------
    double lowLockSince = -1;
    void supervise() {
        const double now = nowSec();
        if (railLocked && now - std::max(lastGoodSec, railSec) > 0.6) {
            railLocked = false; frameLock = false; inFrame = false; haveFirst = false;
            sbI.clear(); sbQ.clear(); holdSyms = 0;
            tel.railLock = false; tel.diffMode = -1;
            say("no frames: searching the code again");
        }
        // acquisition aid: when the carrier loop has been lost for a while after it had settled, open it up again
        if (stage >= 1 && coarseDone && loopStage == 2 && lockAvg < 0.08f && now - lastGoodSec > 0.75) {
            if (lowLockSince < 0) lowLockSince = now;
            else if (now - lowLockSince > 0.75) { loopStage = 0; nSym = 0; loopGains(0.02); lowLockSince = -1; }
        } else lowLockSince = -1;
        const double since = now - std::max(lastGoodSec, startSec);
        if (stage >= 1 && !railLocked && since > 12 && now - lockLostSec > 2) {
            lockLostSec = now;
            autoIdx++;
            stage = 0;
            coarseHz = 0; wideDone = false; wideUsed = false;
            if (presetRun == 0 && rsUser <= 0) pickRate();
            initDemod();
            startSec = now;
        }
    }

    std::string whyNot(double now) const {
        if (rate > 0 && !rateFits()) return "The sample rate is too low for this symbol rate: use " + std::to_string((int)std::ceil(fy3MinSampleRate(rs, alpha) / 1e6)) + " Msps or more.";
        if (stage == 0) return "Measuring the symbol rate.";
        if (!coarseDone) return "Looking for the carrier.";
        if (tel.state >= 3) return {};
        if (now - startSec < 2) return "Looking for the signal.";
        if (tel.carrierLock < 0.5f) return "No QPSK signal at this symbol rate yet. FY-3 passes are short (about 10 minutes): is the satellite above the horizon and the dish pointing at it? Check the LNA power and the product (AHRPT or MPT) and symbol rate.";
        if (!railLocked) return "The carrier is locked but the convolutional code is not found: the signal is too weak (needs about 3 dB Es/N0) or the symbol rate or roll-off is not right.";
        if (!frameLock) return "The code is locked but no frame sync word is found: wrong arrangement of the rails, or a very noisy signal.";
        if (tel.blocksBad > tel.blocksOk) return "Frames are found but Reed-Solomon fails: the signal is too weak. A bigger dish or a lower-noise LNA closer to the feed will help.";
        return "Frames decode only now and then: the signal is marginal.";
    }

    void report() {
        if (kDebug2 && dbgN) { fprintf(stderr, "angle error rms %.3f rad, omega %.5f, lalpha %.5f snrLin %.2f\n", std::sqrt(dbgE / (double)dbgN), omega, lalpha, snrLin); dbgE = 0; dbgN = 0; }
        supervise();
        const double now = nowSec();
        tel.seq = ++seq;
        tel.product = prod; tel.satSetting = presetRun;
        tel.symbolRate = rs; tel.inputRate = rate; tel.timeSec = now; tel.rateMeasured = rsMeasured;
        tel.cfoHz = coarseHz + omega * rs / (2 * kPi);
        tel.clockPpm = (sps / spsEst - 1) * 1e6;
        tel.carrierLock = std::clamp(lockAvg, 0.f, 1.f);
        tel.snrDb = stage >= 1 && coarseDone ? (float)(10 * std::log10(std::max(1e-3f, snrLin))) : 0.f;
        tel.levelDb = nPower ? (float)(10 * std::log10(std::max(1e-20, power / (double)nPower))) : -200.f;
        power = 0; nPower = 0;
        tel.frameLock = frameLock;
        tel.railLock = railLocked;
        tel.railMetric[0] = railQ[0]; tel.railMetric[1] = railQ[1];
        const bool good = now - lastGoodSec < 2;
        tel.state = good ? 3 : frameLock ? 2 : (coarseDone && tel.carrierLock > 0.5f) ? 1 : 0;
        tel.dataValid = good;
        tel.viterbiBer = tel.blocksOk ? (float)((double)tel.rsCorrected * 3.0 / ((double)tel.blocksOk * 8160.0)) : 0.f;   // bytes corrected, about 3 bit errors each, of 8160 bits
        tel.constellation = constel;
        tel.hint = whyNot(now);
        tel.inst.clear();
        for (auto& kv : instStat) tel.inst.push_back(kv.second);
        if (imgDirty) {
            tel.images.clear();
            for (auto& kv : imgs) tel.images.push_back(std::make_shared<const Fy3Image>(kv.second.img));
            tel.imageSeq++; imgDirty = false;
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    void input(cf32 x) {
        // DC spike of the radio: a slow mean taken off
        dcRe += 1.5e-5 * (x.real() - dcRe); dcIm += 1.5e-5 * (x.imag() - dcIm);
        if (wideOn) { wideSample(cf32((float)(x.real() - dcRe), (float)(x.imag() - dcIm))); nIn++; return; }
        const cd v = cd(x.real() - dcRe, x.imag() - dcIm) * rot;
        rot *= step;
        if ((nIn & 1023) == 0) rot /= std::abs(rot);
        nIn++;
        const cf32 in((float)v.real(), (float)v.imag());
        cf32 y;
        if (up == 2) {
            cf32 y0, y1;
            upf.push(in, y0, y1);
            if (lpf.push(y0, y)) decimated(y);
            if (lpf.push(y1, y)) decimated(y);
        } else if (lpf.push(in, y)) decimated(y);
    }
};

Fy3Receiver::Fy3Receiver(int product) : p_(std::make_unique<Impl>(product ? 1 : 0)) {}
Fy3Receiver::~Fy3Receiver() = default;
int Fy3Receiver::product() const { return p_->prod; }

void Fy3Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void Fy3Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void Fy3Receiver::setPreset(int v) {
    int n; fy3Presets(p_->prod, n);
    v = std::clamp(v, 0, n);
    if (p_->presetSet.exchange(v) != v) p_->resetReq = true;
}
int Fy3Receiver::preset() const { return p_->presetSet.load(); }
void Fy3Receiver::setSymbolRate(double hz) { std::lock_guard<std::mutex> lk(p_->mu); hz = std::max(0.0, hz); if (hz != p_->rsSet) { p_->rsSet = hz; p_->resetReq = true; } }
void Fy3Receiver::setRolloff(double a) { std::lock_guard<std::mutex> lk(p_->mu); a = std::clamp(a, 0.0, 1.0); if (a != p_->alphaSet) { p_->alphaSet = a; p_->resetReq = true; } }
bool Fy3Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= (p_->prod ? fy3MptTuning() : fy3AhrptTuning()).minSampleRate - 1; }
void Fy3Receiver::reset() { p_->resetReq = true; }
void Fy3Receiver::clearImage() { p_->clearReq = true; }
void Fy3Receiver::clearResults() { clearImage(); }

void Fy3Receiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.newImages();
    if (m.rate <= 0 || !m.rateFits() || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += kPubSec * m.rate; m.report(); } }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (m.idleLeft > 0) {   // no signal: let this stretch go by (the wait between code searches counts it)
            const int64_t k = std::min<int64_t>(m.idleLeft, (int64_t)(n - i));
            m.idleLeft -= k;
            m.nIn += k;
            m.idleTotal += (uint64_t)k;
            m.holdSyms = std::max<int64_t>(kWarmSyms, m.holdSyms - (int64_t)((double)k * m.rs / m.rate));
            i += (size_t)k - 1;
            if ((double)m.nIn >= m.nextReport) { m.nextReport += kPubSec * m.rate; m.report(); }
            continue;
        }
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.nPower++;
        m.input(cf32(re, im));
        if ((double)m.nIn >= m.nextReport) { m.nextReport += kPubSec * m.rate; m.report(); }
        if (++m.idleRun >= kIdleBlock) {
            m.idleRun = 0;
            if (m.idle()) { m.idleLeft = (int64_t)kIdleSkip * kIdleBlock; m.idled = true; }
            else if (m.idled) {   // something came up: the carrier loop opens to pull it in, and the code search follows when it has settled
                m.idled = false;
                if (!m.railLocked) {
                    m.loopStage = 0; m.nSym = 0; m.loopGains(0.02);
                    m.holdSyms = std::min<int64_t>(std::max<int64_t>(m.holdSyms, 0), (int64_t)(m.rs * 0.03));
                }
            }
        }
    }
}

uint64_t Fy3Receiver::idleInputSamples() const { return p_->idleTotal; }

bool Fy3Receiver::telemetry(Fy3Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void Fy3Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

} // namespace dect2
