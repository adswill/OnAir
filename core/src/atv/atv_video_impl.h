// The analog TV picture decoder's private state (Impl), shared by atv_video.cpp and the atv_video_*.cpp files that hold its bigger
// member functions (line sync, colour, picture output). Not part of the API.
#pragma once
#include "atv_video.h"
#include "atv_dsp.h"
#include "atv_secam.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <numeric>

namespace dect2 {

using namespace atvdsp;

namespace atv_video_detail {
constexpr size_t kRing = 1 << 15, kMask = kRing - 1;
constexpr int kInterpTaps = 8, kInterpPhases = 64;

struct Pulse { double t0 = 0, t1 = 0; };
struct LineRec { double tL = 0; bool hadPulse = false; uint64_t no = 0; };
} // namespace atv_video_detail
using namespace atv_video_detail;

struct AtvVideo::Impl {
    // ---- configuration
    double fv = 10e6, spu = 10;
    bool colourCapable = true;
    AtvVideoParams prm;
    double soundSpacing = 0;
    AtvFormat fmt;
    bool fmtKnown = false;
    AtvVideo* owner = nullptr;
    void doLog(const char* m) { if (owner && owner->log) owner->log(m); }
    void doFrame(std::shared_ptr<const AtvFrame> f) { if (owner && owner->frameReady) owner->frameReady(std::move(f)); }
    bool haveCarrierErr() const { return owner && owner->carrierError; }
    void doCarrierErr(double a, double b) { owner->carrierError(a, b); }

    // ---- rings
    std::vector<float> rv, ri, rq, vsr, m1r;
    std::vector<double> wcs, wsc, wtp;               // weak-signal sync matching: prefix sums of the video, matched-filter scores
    uint64_t nAbs = 0;

    // ---- slicer signal: two running means in cascade; the delay is (L1 + L2 - 2) / 2 samples
    int L1 = 10, L2 = 11;
    double dVs = 9.5;
    double sum1 = 0, sum2 = 0;
    uint64_t smoothCount = 0;

    // ---- acquisition
    std::vector<float> acq;
    uint64_t acqStart = 0;
    size_t acqNeed = 0;
    bool levelsValid = false, loopRunning = false;
    double Tr = 0, Br = 0;                      // sync tip and blanking level of the detected video (EMA, raw units)
    double thr = 0, hyst = 0;
    // ---- pulse detector
    int pState = 0;                             // 0 low, 1 pending start, 2 in pulse, 3 pending end
    double pT0 = 0, pT1 = 0;
    float prevVs = 0;
    // ---- line loop
    double P = 640, Pslow = 640, tNext = 0;      // line period in samples: the loop's own, and a slow average of it for the display
    int goodRun = 0, missRun = 0, oogRun = 0;
    float syncQ = 0;
    double secsNoLock = 0;
    uint64_t lineNo = 0, curNo = 0;
    std::deque<LineRec> lineQ;
    double lastGoodT0 = 0;
    // ---- field sync
    int bCount = 0;
    double bFirst = 0, bLast = 0;
    bool haveVsync = false;
    double vsyncT = 0;
    double vsyncH = 0;     // the field sync in half lines on the loop's own line count (lineNo): rows follow from counted lines, not from time / P, so a line period that is a few ppm off or jitters in noise cannot slip the row
    int vsyncField = 0;
    int vsyncMiss = 0;
    double lastVsyncT = 0;
    uint64_t vsyncCount = 0;
    bool vbiPending = false;
    double vbiT = 0;
    // ---- levels per line
    double Bline = 0;
    double noiseVar = 0;
    bool gainValid = false;
    double carrierAmp = 0, cnVar = 0, noiseScale = 1;
    bool fmVideo = false;                       // FM video (analog FPV): the levels follow the baseline of an AC coupled transmitter
    // ---- polarity and raster
    int pol = 1;                                // 1 negative modulation (sync = most carrier), -1 positive (A, C, E, L): the input is negated
    bool mirroredHint = false;                  // the receiver found the channel turned round (A, L')
    double TrMeas = 0;                          // positive modulation: the measured sync tip (Tr itself is derived from blanking)
    // The line rasters the decoder can lock to; the 625 and 525-line ones share a group (the same sync pulse, line periods 0.7 % apart)
    struct Raster { double lineUs, syncUs, wMin, wMax, spMin, spMax, medMin, medMax, pMin, pMax, nMin, nMax, bMin, bMax; };
    static const Raster& raster(int g) {
        static const Raster r[3] = {
            {64.0, 4.7, 3.9, 5.6, 62.5, 65.5, 63.2, 64.4, 62.8, 64.9, 3.3, 6.4, 15, 40},          // 625 and 525 lines
            {98.765, 9.0, 7.4, 10.8, 96.0, 101.5, 97.5, 100.0, 97.0, 100.5, 6.5, 12.5, 25, 46},  // 405 lines (A)
            {48.840, 2.5, 1.9, 3.2, 47.3, 50.3, 48.2, 49.5, 48.0, 49.7, 1.6, 3.6, 12, 23}};      // 819 lines (E)
        return r[g < 0 ? 0 : g > 2 ? 2 : g];
    }
    static int rasterOf(int lines) { return lines == 405 ? 1 : lines == 819 ? 2 : 0; }
    int rast = 0;                               // the raster group of the line loop
    // the polarity the forced system asks for (0: free, found by trial)
    int forcedPol() const { return fmVideo ? 1 : prm.forceSys >= 0 ? (atvSysPositive(prm.forceSys) ? -1 : 1) : 0; }   // FM video: the receiver tries the polarity
    // the decoder's input changes sign: everything it holds of the signal does too (all of it is linear in the input)
    void flipPolarity() {
        pol = -pol;
        for (size_t k = 0; k < kRing; k++) { rv[k] = -rv[k]; ri[k] = -ri[k]; rq[k] = -rq[k]; vsr[k] = -vsr[k]; m1r[k] = -m1r[k]; }
        sum1 = -sum1; sum2 = -sum2; prevVs = -prevVs;
        for (float& a : acq) a = -a;
    }
    // where the levels are measured on a line (us from 0H): the sync tip, the back porch and the front porch (fpB < fpA: none)
    void porches(double& tA, double& tB, double& bpA, double& bpB, double& fpA, double& fpB) const {
        const AtvFormat& F = fmt;
        if (F.lines == 625 || F.lines == 525) {
            tA = 1.4; tB = F.syncUs - 0.7;
            bpA = F.lines == 625 ? 8.3 : 8.1; bpB = F.lines == 625 ? 10.0 : 9.1;
            fpA = -1.15; fpB = -0.35;
        } else {
            tA = 0.3 * F.syncUs; tB = 0.8 * F.syncUs;
            bpA = F.syncUs + 0.45 * (F.blankEndUs - F.syncUs); bpB = F.blankEndUs - 0.4;
            if (F.frontPorchUs >= 1.2) { fpA = -(F.frontPorchUs - 0.4); fpB = -0.35; } else { fpA = 0; fpB = -1; }
        }
    }
    double phErr2 = 1;
    int phGood = 0;
    bool phaseLockedFlag = false;
    bool syncDetOn = false;
    int levelsRestart = 0;
    // ---- pictures
    int W = 768, Hh = 576;
    std::vector<uint8_t> fbuf;
    std::vector<uint8_t> tfPrev;                // the previous published picture (temporal noise reduction)
    std::vector<float> yRow, uRow, vRow, prevU, prevV;
    bool prevValid = false;
    uint64_t prevLineNo = 0;
    int fieldLines = 0;
    int curField = -1;
    uint64_t frameSeq = 0;
    uint64_t fieldCount = 0, fieldsOk = 0, fieldsBad = 0, lineCount = 0;
    float whiteTrack = 0, whiteLast = 0;
    double shiftEma = 0;                       // mean level of the burst against blanking, normalised video units, smoothed (burst of a compressed sync sits above blanking)
    double compEma = 1, compK = 1;             // burst size against the sync-referenced scale, smoothed; the luminance gain divisor when above the tolerance
    bool anyPicture = false;
    // ---- colour
    int colourKind = -1;                        // -1 not decided, otherwise AtvColourKind
    double burstEma = 0;
    int killCount = 0;
    bool killer = false;
    bool colourLocked = false;
    double chromaErr = 0;
    int vsign = 1;
    std::vector<float> zi, zq, yv;              // per-line scratch
    std::vector<float> lpC;                     // chroma low-pass
    // ---- interpolation
    std::vector<float> kern;
    // ---- scope
    std::vector<float> lineWave, vbiWave;
    int scopeSkip = 0;
    // ---- statistics
    double lineHzMeas = 0;
    double soundHint = 0;
    uint64_t sinceCarrierErr = 0;

    // ---- SECAM (see atv_secam.h)
    AtvSecam secam;
    bool secamReady = false;
    int sLead = 0, sPairs = 0, sAlt = 0, sBottle = 0, lastLeadKind = -1;     // colour search: lead-in found, consecutive pairs, pairs that alternate, field identification lines
    uint64_t lastLeadNo = 0, voteField = ~0ull;
    int lastBottleKind = -1;
    long lastBottleLine = 0;
    double rVote = 0;                            // what the lines of this field say about their kind: > 0 odd frame lines carry D'R, < 0 even ones
    uint64_t secRYNo = 0, secBYNo = 0;           // line numbers the stored colour differences come from
    bool secamLineOk = false;                    // this line's chroma rows (secU, secV) are good
    float secAmpEma = 0;
    int secKill = 0;
    std::vector<float> secRY, secBY, secU, secV, secD, zsi, zsq;
    std::vector<AtvSecam::cx> secBB;

    Impl() {}

    // ------------------------------------------------------------------------------ setup
    void configure(double rate, bool colourOk) {
        fv = rate; spu = rate / 1e6; colourCapable = colourOk;
        rv.assign(kRing, 0.f); ri = rv; rq = rv; vsr = rv; m1r = rv;
        setSlicerWidth(1.0);
        acqNeed = (size_t)(0.05 * fv);
        secamReady = colourOk;
        if (secamReady) secam.configure(rate);
        kern.assign((size_t)(kInterpPhases + 1) * kInterpTaps, 0.f);
        for (int p = 0; p <= kInterpPhases; p++) {
            const double fr = (double)p / kInterpPhases;
            double sum = 0;
            double w[kInterpTaps];
            for (int m = 0; m < kInterpTaps; m++) {
                const double d = fr - (m - (kInterpTaps / 2 - 1));
                const double sinc = d == 0 ? 1.0 : std::sin(kPi * 0.93 * d) / (kPi * d);
                w[m] = sinc * kaiser(d / (kInterpTaps / 2), 4.5);
                sum += w[m];
            }
            for (int m = 0; m < kInterpTaps; m++) kern[(size_t)p * kInterpTaps + m] = (float)(w[m] / sum);
        }
        fullReset();
    }

    void fullReset() {
        std::fill(rv.begin(), rv.end(), 0.f); std::fill(ri.begin(), ri.end(), 0.f); std::fill(rq.begin(), rq.end(), 0.f); std::fill(vsr.begin(), vsr.end(), 0.f); std::fill(m1r.begin(), m1r.end(), 0.f);
        nAbs = 0;
        pol = forcedPol() < 0 ? -1 : 1;
        sum1 = sum2 = 0; smoothCount = 0;
        resetSync();
        counters_reset();
    }
    void counters_reset() {}

    // Below about 9.5 Msps the video band stops short of the colour subcarrier (4.4 MHz above the carrier): the picture is monochrome and says so
    // at once instead of waiting for a colour search that cannot succeed.
    int startColour() const { return colourCapable ? prm.forceColour : (int)kAtvMono; }

    // forget levels and the loop, keep counters and the picture
    void resetSync() {
        acq.clear(); acqStart = 0;
        levelsValid = false; loopRunning = false; weakMode = false; weakMidPending = false;
        pState = 0; prevVs = 0;
        P = Pslow = raster(rast).lineUs * 1e-6 * fv; tNext = 0; goodRun = missRun = oogRun = 0; syncQ = 0;
        lineQ.clear();
        bCount = 0; haveVsync = false; vsyncMiss = 0; vbiPending = false;
        gainValid = false; Bline = 0; noiseVar = 0;
        fmtKnown = false;
        colourKind = startColour();
        mainLock = Lock(); cands.clear(); candLines = 0; burstEma = 0; killCount = 0; killer = false; colourLocked = false; compEma = compK = 1; shiftEma = 0;
        prevValid = false; curField = -1; fieldLines = 0;
        phErr2 = 1; phGood = 0; phaseLockedFlag = false;
        sLead = sPairs = sAlt = sBottle = 0; lastLeadKind = -1; lastLeadNo = 0; lastBottleKind = -1; rVote = 0; voteField = ~0ull; secRYNo = secBYNo = 0; secamLineOk = false; secAmpEma = 0; secKill = 0;
        carrierAmp = 0; cnVar = 0;
    }

    // ------------------------------------------------------------------------------ the standard
    // The system from what is known: the line count, the polarity, the sound carrier spacing and whether the channel is turned round.
    // H and K' differ from B/G and D/K only in the width of the vestigial sideband: they are found only when chosen.
    int sysFromHints(int lines) const {
        if (prm.forceSys >= 0) return prm.forceSys;
        if (lines == 525) return kAtvM;
        if (lines == 405) return kAtvA;
        if (lines == 819) return kAtvE;
        if (pol < 0) {
            if (mirroredHint) return kAtvLp;
            if (soundSpacing > 5.4 && soundSpacing < 5.6) return kAtvC;
            return kAtvL;
        }
        if (soundSpacing > 5.9 && soundSpacing < 6.1) return kAtvI;
        if (soundSpacing > 6.4 && soundSpacing < 6.6) return kAtvDK;
        if (soundSpacing > 4.4 && soundSpacing < 4.6) return kAtvN;
        return kAtvG;
    }

    void makeFormat(int lines) {
        if (lines == 405 || lines == 819) colourKind = kAtvMono;     // the 405 and 819-line systems had no colour
        int col = colourKind >= 0 ? colourKind : kAtvMono;
        int sys = sysFromHints(lines);
        if (atvSysLines(sys) != lines) sys = lines == 525 ? kAtvM : lines == 405 ? kAtvA : lines == 819 ? kAtvE : pol < 0 ? kAtvL : kAtvG;
        AtvFormat f;
        if (!atvMakeFormat(sys, col, f)) { if (!atvMakeFormat(sys, kAtvMono, f)) atvMakeFormat(kAtvG, kAtvMono, f); }
        if (prm.setupMode == 0) f.setup = 0;
        else if (prm.setupMode == 1) f.setup = 0.075;
        if (prm.chromaDelayNs >= 0) f.chromaDelayNs = prm.chromaDelayNs;
        if (!(fmt.picW == f.picW && fmt.picH == f.picH) || fbuf.size() != (size_t)f.picW * f.picH * 4) { W = f.picW; Hh = f.picH; fbuf.assign((size_t)W * Hh * 4, 0); for (size_t i = 3; i < fbuf.size(); i += 4) fbuf[i] = 255; }
        fmt = f;
        fmtKnown = true;
        yRow.assign((size_t)W, 0.f); uRow = yRow; vRow = yRow; prevU = yRow; prevV = yRow;
        designChroma();
    }

    // The slicer signal is the video smoothed by two running means of about `us` microseconds. Wider means less noise in the sync pulses (and a
    // slower edge, which the crossing time does not mind): the receiver goes there when it cannot find the lines with the normal width.
    void setSlicerWidth(double us) {
        L1 = std::max(2, (int)std::lround(us * spu)); L2 = std::max(2, (int)std::lround(1.1 * us * spu));
        dVs = 0.5 * (L1 + L2 - 2);
    }

    // ------------------------------------------------------------------------------ the sample loop
    inline float rvAt(uint64_t n) const { return rv[n & kMask]; }

    void process(const float* v, const float* i, const float* q, size_t n);

    // ------------------------------------------------------------------------------ pulses
    // crossing time (absolute, fractional, delay removed) between the previous and the current slicer sample
    inline double crossT(float a, float b, float level) const {
        const double fr = (b == a) ? 0.0 : (double)(level - a) / (double)(b - a);
        return (double)(nAbs - 2) + fr - dVs;
    }

    void detect(float vs);

    void onPulse(double t0, double t1) {
        const double w = (t1 - t0) / spu;
        const Raster& R = raster(rast);
        if (w >= R.nMin && w <= R.nMax) normalPulse(t0, t1);
        else if (w > R.bMin && w < R.bMax) broadPulse(t0);
        // In noise the width of the slicer's pulse scatters by +-2 us (the edges move on their own), and a locked loop would lose every sixth line:
        // a pulse that starts inside the gate of the line that is due is that line's sync whatever width it measured. (Equalising pulses of the
        // field sync start there as well, at the line start, which is where the line is; those in the middle of a line are far outside the gate.)
        else if (loopRunning && goodRun >= 8 && w >= 0.4 * R.nMin && w <= 1.4 * R.nMax && std::fabs(t0 - tNext) <= gate()) normalPulse(t0, t1);
    }

    void normalPulse(double t0, double t1);

    double gate() const { return (goodRun < 4 ? 3.0 : 2.0) * spu; }
    // a line whose sync pulse did not come: decided once the gate has passed, unless a pulse that started inside it is still going on
    void checkMiss() {
        const double now = (double)nAbs - 1 - dVs;
        if (now <= tNext + gate() + 0.3 * spu) return;
        if (pState != 0 && std::fabs(pT0 - tNext) <= gate() && now < tNext + 12 * spu) return;
        lineMiss();
    }

    void lineMiss() {
        lineQ.push_back({tNext, false, ++lineNo});
        tNext += P;
        // an established loop keeps its slow gains over a missed line or two (in noise the sync is missed now and then; the fast acquisition gains
        // would then kick the line period by the noise of the next pulses and slip the loop)
        if (missRun >= 3) goodRun = 0;
        syncQ += (0.f - syncQ) / 32.f;
        if (++missRun > 40 || syncQ < 0.08f) { loseLock(); }
    }

    void loseLock() {
        loopRunning = false; levelsValid = false; weakMode = false; weakMidPending = false;
        acq.clear(); pState = 0;
        syncQ = 0; haveVsync = false;
        doLog("analog TV: line sync lost");
    }

    // broad (field synchronising) pulses: a run of eqPulses of them, half a line apart
    void broadPulse(double t0) {
        if (!loopRunning) return;
        const double half = 0.5 * P;
        // (in noise a broad pulse is cut in two or goes under now and then: one that is missing in the run, a full line after the last, still
        // counts, as two)
        if (bCount > 0 && std::fabs((t0 - bLast) - half) < 0.18 * P) { bCount++; bLast = t0; }
        else if (bCount > 0 && bCount + 2 <= fmt.broadPulses && std::fabs((t0 - bLast) - 2 * half) < 0.18 * P) { bCount += 2; bLast = t0; }
        else { bCount = 1; bFirst = bLast = t0; }
        const int need = fmt.broadPulses;
        if (fmtKnown && bCount == need) vsync(bFirst);
    }

    void vsync(double t_v);

    // ------------------------------------------------------------------------------ acquisition: find the levels and the line timing in the slicer signal
    struct Cr { double t0, t1; };
    // Pulses (crossings of th, with hysteresis hy) in a stretch of the slicer signal; times in samples of that stretch. The crossing is
    // interpolated only where there is one.
    static void scanArr(const float* x, size_t n, double th, double hy, std::vector<Cr>& out) {
        out.clear();
        int st = 0;
        double c0 = 0, c1 = 0;
        const float up = (float)(th + hy), dn = (float)(th - hy), t = (float)th;
        auto cross = [&](size_t k) { const float a = x[k - 1], b = x[k]; return (double)(k - 1) + ((b == a) ? 0.0 : (double)(t - a) / (double)(b - a)); };
        for (size_t k = 1; k < n; k++) {
            const float a = x[k - 1], b = x[k];
            switch (st) {
            case 0: if (a < t && b >= t) { c0 = cross(k); st = 1; } break;
            case 1: if (b >= up) st = 2; else if (b < dn) st = 0; else if (a < t && b >= t) c0 = cross(k); break;
            case 2: if (a >= t && b < t) { c1 = cross(k); st = 3; } break;
            case 3: if (b <= dn) { st = 0; out.push_back({c0, c1}); } else if (b >= up) st = 2; else if (a >= t && b < t) c1 = cross(k); break;
            }
        }
    }
    void scan(double th, double hy, std::vector<Cr>& out) const { scanArr(acq.data(), acq.size(), th, hy, out); }

    // What one look at a stretch of slicer signal found (analyse() has no side effects; commit() starts the loop with it)
    struct Acq {
        int group = 0, lines = 625;
        double th = 0, hy = 0, T = 0, B = 0, Pm = 0, lastT = 0;   // lastT: the start of the last line on the fit, in samples of the stretch
        double nin = 0, expect = 0;                                // pulses on the fit, and lines in the stretch
        bool weak = false;                                         // found by the matched filter (weak signal): the line loop then runs on matched filters too
    };

    bool analyse(const std::vector<float>& a, Acq& r) const;
    bool analyseThr(const std::vector<float>& a, Acq& r) const;    // threshold crossings: pulses of the width of a line sync, fitted to a line grid
    bool analyseFold(const std::vector<float>& a, Acq& r) const;   // weak signals: the stretch folded at every possible line period, sync matched filter
    int foldTick = 0;
    bool foldNow = false;
    // Weak-signal tracking: the line (and field) sync found by matched filters at the slots the loop expects them, instead of threshold crossings
    bool weakMode = false, weakMidPending = false;
    double weakMid = 0;
    float weakQ = 0.6f;                         // smoothed height of the matched sync pulses against the levels (about 1 on a good signal)
    int weakLow = 0, weakN = 0, weakStrong = 0;
    bool weakLock = true;
    void weakService();
    void weakLine();
    void weakBlock(int g, int pre, int post, int len);
    bool weakBroad(double s) const;

    void commit(const Acq& r);

    // Positive modulation: the sync tip is the carrier at its lowest, where noise (and an envelope detector) say least about the level. The
    // reference is the blanking level instead, with the sync tip where the standard puts it against blanking (0 % of the carrier: zero).
    void positiveLevels() {
        const double k = fmt.blankPct > 0 ? fmt.syncPct / fmt.blankPct : 0;
        Tr = Br * k;
        hyst = std::max(0.1 * (Tr - Br), 1e-9);
        thr = 0.5 * (Tr + Br);
    }

    // The line sync in the stretch held: with the polarity in use, and when that finds no clear line structure (and the system is not forced),
    // with the other one. The polarity that finds more lines wins: a negatively modulated signal turned over has no sync pulses, and the other way.
    bool acquire();

    // ------------------------------------------------------------------------------ lines
    void processLines();

    double lineGain() const { return (Tr - Br) / std::max(1e-9, -fmt.syncLevel); }     // raw units for one unit of normalised video

    // mean of the raw video over [a, b] samples (absolute, fractional ends rounded)
    double meanRaw(double a, double b) const {
        const uint64_t ia = (uint64_t)std::max(0.0, std::ceil(a)), ib = (uint64_t)std::max(0.0, std::floor(b));
        if (ib < ia) return rvAt(ia);
        double s = 0;
        for (uint64_t n = ia; n <= ib; n++) s += rvAt(n);
        return s / (double)(ib - ia + 1);
    }

    // FM video (analog FPV): the transmitter is AC coupled, so the long broad pulses of the field sync shift the whole baseline and it creeps back
    // over the next lines. The threshold of the slicer follows the levels of the lines whose pulse was found; once the baseline moved by half the
    // sync height no pulse is found any more and the levels would stay where they were: the decoder gave up after 40 lines. The levels of a missed
    // line are measured where its pulse is due instead, and used when they look like a sync pulse (about the height known).
    void trackLevelsOnMiss(double tL);

    // the blanking level of a line: the back porch and the front porch, weighted by their lengths
    double blankingAt(double tL, double bpA, double bpB, double fpA, double fpB) const {
        const double b1m = meanRaw(tL + bpA * spu, tL + bpB * spu);
        if (fpB <= fpA) return b1m;
        const double b2m = meanRaw(tL + fpA * spu, tL + fpB * spu);
        const double n1 = (bpB - bpA) * spu, n2 = (fpB - fpA) * spu;
        return (b1m * n1 + b2m * n2) / (n1 + n2);
    }

    void processLine(const LineRec& r);

    // ------------------------------------------------------------------------------ colour
    // A subcarrier reference with a burst loop. The reference phase at time t is ph + 2 pi fsc (t - tRef) / fv.
    struct Lock {
        double fsc = 0, ph = 0, tRef = 0;
        bool init = false;
        std::complex<double> prev;               // burst of the line before (PAL pairs)
        uint64_t prevLine = 0;
        bool prevValid = false;
        double err2 = 1, amp = 0;                // EMA of the squared phase error and of the burst amplitude (share of nominal)
        double nz = 0;                           // EMA of the noise power in one burst measurement
        int lines = 0;
        std::complex<double> last;               // the burst of the last line (against the reference, normalised video domain)
        double vacc[2] = {0, 0};                 // PAL: real part of the burst on even and odd lines, smoothed: its sign is the V switch
        double fsc0 = 0;
        void reset(double f) { *this = Lock(); fsc = fsc0 = f; }
    };
    Lock mainLock;
    std::vector<Lock> cands;

    void advance(Lock& L, double tL) {
        if (!L.init) { L.ph = 0; L.tRef = tL; L.init = true; return; }
        L.ph = std::fmod(L.ph + 2 * kPi * L.fsc * (tL - L.tRef) / fv, 2 * kPi);
        L.tRef = tL;
    }

    // The burst against a reference, in the normalised video domain (p = (B - r) / G, so the complex amplitude flips sign). The burst sits on
    // the carrier at blanking level, a constant in the complex baseband: that is taken away first.
    bool measureBurst(double tL, const Lock& L, std::complex<double>& b, double& nb2) const;

    // How much the sync pulses are squeezed, from their height against blanking alone (no burst, no gain involved): the carrier at blanking and
    // at the sync tip are fixed by the standard (tip 100 %, white whitePct %, so blanking sits at 1 - m * (-sL) / (1 - sL), m = 1 - whitePct / 100).
    // 0 = as the standard has it, 0.4 = the pulse is 60 % of its height. Modulation depths differ a little between transmitters, so this is
    // only good to about 10 %; it decides whether the burst may be used for the gain, it is not the gain itself.
    double syncSqueeze() const {
        if (Tr <= Br || Br <= 0) return 0;
        const double sL = fmt.syncLevel, m = 1 - fmt.whitePct / 100;
        const double br0 = 1 - m * (-sL) / (1 - sL), r0 = (1 - br0) / br0;
        const double r = (Tr - Br) / Br;
        return std::max(0.0, 1 - r / r0);
    }

    // The mean level of the burst window against blanking, in normalised video units (positive = brighter than blanking). A burst is symmetric about
    // blanking; a compressor that squeezes what is below blanking (the sync pulses, and with them the lower half of every burst cycle) lifts it.
    // Hann weights keep the 4.4 MHz of the burst itself out of the mean.
    bool burstShift(double tL, double& dv) const;

    // One line of a loop. Returns false when there was no burst to measure.
    bool stepLock(Lock& L, double tL, bool pal, double nominal, double alpha, double beta);

    void burstAndColour(double tL, bool picture);
    int candLines = 0;
    std::vector<int> candKind;                  // the colour system each candidate loop stands for

    void decideColour();
    int secamHow = 0;

    // ------------------------------------------------------------------------------ SECAM
    // the complex baseband of samples [a, b] (absolute) copied into zsi, zsq; returns the first sample
    long secamSpan(double a, double b) {
        const long sa = (long)std::floor(a), sb = std::min((long)std::ceil(b), (long)nAbs - 1);
        const size_t n = sb >= sa ? (size_t)(sb - sa + 1) : 0;
        zsi.resize(n); zsq.resize(n);
        for (size_t k = 0; k < n; k++) { const uint64_t idx = (uint64_t)(sa + (long)k) & kMask; zsi[k] = ri[idx]; zsq[k] = rq[idx]; }
        return sa;
    }
    // position of an absolute sample in the outputs of the baseband that starts at sa
    double secamOut(double tAbs, long sa) const { return (tAbs - (double)sa - secam.half()) / secam.decim(); }

    // The lead-in: the subcarrier at the rest frequency of the line from 5.6 us after 0H. 1 = D'R, 0 = D'B, -1 = nothing usable.
    // fHz and amp are what was measured (amp 0: nothing). The hard decision needs the frequency within 45 kHz of a rest frequency.
    int secamLead(double tL, double g, const std::vector<AtvSecam::cx>& bb, long sa, double& amp, double* fHz = nullptr) {
        const double a = secamOut(tL + 6.6 * spu, sa), b = secamOut(tL + std::min(9.9, fmt.blankEndUs - 0.4) * spu, sa);
        amp = 0;
        if (a < 0 || b >= (double)bb.size()) return -1;
        double f, am;
        secam.tone(bb, (size_t)std::ceil(a), (size_t)std::floor(b) + 1, f, am);
        amp = am / g;
        if (fHz) *fHz = f;
        if (amp < 0.05) return -1;                                  // nominal: 0.115 times 1.04 (D'B) or 1.33 (D'R)
        if (std::fabs(f - kSecamF0R) < 45e3) return 1;
        if (std::fabs(f - kSecamF0B) < 45e3) return 0;
        return -1;
    }

    // the votes are a field's own: the sequence of the lines may start differently in the next
    void newFieldVote() { if (voteField != vsyncCount) { voteField = vsyncCount; rVote = 0; lastBottleKind = -1; } }

    // One of the identification lines of the field blanking: the subcarrier ends the line at +350 kHz from f0R (a D'R line) or -350 kHz from f0B
    void secamBottle(double tL, double g, long L);

    // Everything SECAM that one line needs: field identification lines, the lead-in during the colour search, and the colour of a picture line.
    void secamLine(double tL, bool havePicture);

    // ------------------------------------------------------------------------------ the picture
    inline float interp(const float* x, double pos) const {          // x[0] is sample 0; pos in samples
        const long i0 = (long)std::floor(pos);
        const double fr = pos - (double)i0;
        const int ph = (int)std::lround(fr * kInterpPhases);
        const float* k = &kern[(size_t)ph * kInterpTaps];
        const float* p = x + (i0 - (kInterpTaps / 2 - 1));
        float acc = 0;
        for (int t = 0; t < kInterpTaps; t++) acc += k[t] * p[t];
        return acc;
    }

    // chroma filters for the standard that was found: a band-pass around the subcarrier (what is taken out of the video to leave the
    // luminance) and the low-pass after the synchronous demodulator
    void designChroma();

    void extractPicture(double tL, int row, int field);
    static inline uint8_t to8(float v) { const int i = (int)(v * 255.f + 0.5f); return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i); }
    bool lastColourNow = false;
    std::vector<float> bpf, lumBuf, bp, ti, tq, yiL, yqL, yRowP, cuRow, cvRow, cvtY, cvtU, cvtV, cvtR, czP, czW;
    std::vector<int> czIdx;
    int zdec = 2;
    double zoff = 0;

    void finishField() {
        if (!anyPicture && fieldLines == 0) return;
        fieldCount++;
        const AtvFormat& F = fmt;
        const bool ok = fieldLines >= (int)(0.9 * F.fieldRows);
        if (ok) fieldsOk++; else fieldsBad++;
        if (curField == 1 || prm.deinterlace == 1) publish(curField);
        whitePeakReport = whiteLast; whiteLast = 0;
    }
    float whitePeakReport = 0;

    void publish(int field);
    void temporalFilter(AtvFrame& f);

    // ------------------------------------------------------------------------------ scope
    void captureLine(double tL);

    void captureVbi();

    // ------------------------------------------------------------------------------ results
    Info info() const {
        Info i;
        if (fmtKnown) {
            i.system = fmt.name; i.lines = fmt.lines;
            i.colourSystem = colourKind < 0 ? "" : fmt.colourName;
            i.lineErrPpm = (fv / Pslow / fmt.lineHz - 1) * 1e6;
            i.fieldHz = 2 * (fv / Pslow) / fmt.lines;
        }
        i.lineHz = loopRunning ? fv / Pslow : 0;
        i.colour = colourLocked && !killer && (colourKind == kAtvPal || colourKind == kAtvNtsc || colourKind == kAtvSecam);
        i.killer = killer;
        i.syncQuality = syncQ;
        i.state1 = loopRunning && syncQ > 0.5f;
        i.state2 = i.state1 && haveVsync && vsyncMiss < 6;
        if (gainValid && fmtKnown) {
            const double g = lineGain();
            i.syncTip = Tr > 0 ? (float)(100.0 * (Tr - Br) / Tr) : 0.f;       // sync height in percent of the peak carrier (nominal 26 / 25)
            // positive modulation: blanking against the measured sync tip, in percent of the peak (white) carrier (nominal 30, or 25 in E)
            if (pol < 0) i.syncTip = Br < 0 ? (float)(fmt.blankPct * (TrMeas - Br) / -Br) : 0.f;
            const double sig = std::sqrt(std::max(noiseVar, 1e-30)) / std::max(1e-9, g);
            i.snrDb = (float)std::min(60.0, std::max(0.0, 20 * std::log10(1.0 / std::max(sig, 1e-6))));
        }
        i.whitePeak = whitePeakReport;
        i.burstLevel = (float)burstEma;
        i.syncCompressionPct = compK > 1 ? (float)(100.0 * (1.0 - 1.0 / compK)) : 0.f;
        i.chromaErrDeg = (float)(chromaErr * 180 / kPi);
        i.carrierLevel = carrierAmp;
        if (cnVar > 0 && carrierAmp > 0) i.carrierToNoiseDb = (float)std::min(70.0, std::max(-10.0, 10 * std::log10(carrierAmp * carrierAmp * noiseScale / cnVar)));
        i.lines_ = lineCount; i.fields = fieldCount; i.frames = frameSeq; i.fieldsOk = fieldsOk; i.fieldsBad = fieldsBad;
        i.fieldNo = curField < 0 ? 0 : curField;
        i.lineWave = lineWave; i.vbiWave = vbiWave;
        return i;
    }
};

} // namespace dect2
