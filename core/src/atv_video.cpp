// Analog TV receiver, picture side. See atv_video.h.
#include "atv_video.h"
#include "atv_dsp.h"
#include "atv_secam.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <numeric>

namespace dect2 {

using namespace atvdsp;

namespace {
constexpr size_t kRing = 1 << 15, kMask = kRing - 1;
constexpr int kInterpTaps = 8, kInterpPhases = 64;

struct Pulse { double t0 = 0, t1 = 0; };
struct LineRec { double tL = 0; bool hadPulse = false; uint64_t no = 0; };
} // namespace

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
    double phErr2 = 1;
    int phGood = 0;
    bool phaseLockedFlag = false;
    bool syncDetOn = false;
    int levelsRestart = 0;
    // ---- pictures
    int W = 768, Hh = 576;
    std::vector<uint8_t> fbuf;
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
        levelsValid = false; loopRunning = false;
        pState = 0; prevVs = 0;
        P = Pslow = 64e-6 * fv; tNext = 0; goodRun = missRun = oogRun = 0; syncQ = 0;
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
    int sysFromHints(int lines) const {
        if (prm.forceSys >= 0) return prm.forceSys;
        if (lines == 525) return kAtvM;
        if (soundSpacing > 5.9 && soundSpacing < 6.1) return kAtvI;
        if (soundSpacing > 6.4) return kAtvDK;
        if (soundSpacing > 4.4 && soundSpacing < 4.6) return kAtvN;
        return kAtvG;
    }

    void makeFormat(int lines) {
        int col = colourKind >= 0 ? colourKind : kAtvMono;
        int sys = sysFromHints(lines);
        if (lines == 525) sys = kAtvM;
        if (lines == 625 && sys == kAtvM) sys = kAtvG;
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

    void process(const float* v, const float* i, const float* q, size_t n) {
        const double inv1 = 1.0 / L1, inv2 = 1.0 / L2;
        constexpr size_t kPiece = 1024;                        // samples a piece: the rings run at most this far ahead of the pulse search
        for (size_t k0 = 0; k0 < n; k0 += kPiece) {
            const size_t m = std::min(kPiece, n - k0);
            const uint64_t n0 = nAbs;
            // the rings
            {
                size_t done = 0;
                while (done < m) {
                    const size_t idx = (size_t)((n0 + done) & kMask), c = std::min(m - done, kRing - idx);
                    std::memcpy(&rv[idx], v + k0 + done, c * sizeof(float));
                    std::memcpy(&ri[idx], i + k0 + done, c * sizeof(float));
                    std::memcpy(&rq[idx], q + k0 + done, c * sizeof(float));
                    done += c;
                }
            }
            // two running means; what leaves the sum is read back from the rings
            {
                double s1 = sum1, s2 = sum2;
                const uint64_t o1 = (uint64_t)L1, o2 = (uint64_t)L2;
                for (size_t j = 0; j < m; j++) {
                    const size_t idx = (size_t)((n0 + j) & kMask);
                    s1 += rv[idx] - rv[(size_t)((n0 + j - o1) & kMask)];
                    const float m1 = (float)(s1 * inv1);
                    m1r[idx] = m1;
                    s2 += m1 - m1r[(size_t)((n0 + j - o2) & kMask)];
                    vsr[idx] = (float)(s2 * inv2);
                }
                sum1 = s1; sum2 = s2;
            }
            // the pulse search. Between pulses the signal is below the threshold and nothing happens until the next line is overdue: such a
            // stretch is skipped at once, with the line-overdue time kept as a limit on the sample count.
            float thrF = (float)thr;
            const uint64_t warm = (uint64_t)(L1 + L2 + 4);
            for (size_t j = 0; j < m;) {
                if (levelsValid && loopRunning && pState == 0 && smoothCount >= warm) {
                    // checkMiss does nothing until (nAbs - 1 - dVs) passes tNext + gate + 0.3 spu
                    const double lim = tNext + gate() + 0.3 * spu + dVs + 1.0;
                    const uint64_t limN = lim < 0 ? 0 : lim >= 1.8e19 ? ~0ull : (uint64_t)std::floor(lim);       // nAbs > lim  <=>  nAbs > limN
                    size_t jj = j;
                    uint64_t na = nAbs;
                    for (; jj < m; jj++) {
                        if (vsr[(size_t)((n0 + jj) & kMask)] >= thrF || na + 1 > limN) break;
                        na++;
                    }
                    if (jj > j) {
                        prevVs = vsr[(size_t)((n0 + jj - 1) & kMask)];
                        smoothCount += (uint64_t)(jj - j);
                        nAbs = na;
                        j = jj;
                        if (j >= m) break;
                    }
                }
                const float vs = vsr[(size_t)((n0 + j) & kMask)];
                j++;
                nAbs++;
                smoothCount++;
                if (smoothCount < warm) { prevVs = vs; continue; }
                if (!levelsValid) {
                    if (acq.empty()) acqStart = nAbs - 1;
                    acq.push_back(vs);
                    if (acq.size() >= acqNeed) { if (!acquire()) { acq.erase(acq.begin(), acq.begin() + (long)(acq.size() / 2)); acqStart += (uint64_t)acqNeed / 2; } }
                    prevVs = vs;
                    thrF = (float)thr;
                    continue;
                }
                if (pState != 0 || vs >= thrF) detect(vs);          // between pulses the signal is below the threshold: nothing to do
                prevVs = vs;
                if (loopRunning) checkMiss();
            }
        }
        processLines();
    }

    // ------------------------------------------------------------------------------ pulses
    // crossing time (absolute, fractional, delay removed) between the previous and the current slicer sample
    inline double crossT(float a, float b, float level) const {
        const double fr = (b == a) ? 0.0 : (double)(level - a) / (double)(b - a);
        return (double)(nAbs - 2) + fr - dVs;
    }

    void detect(float vs) {
        const float up = (float)(thr + hyst), dn = (float)(thr - hyst);
        const float th = (float)thr;
        switch (pState) {
        case 0:
            if (prevVs < th && vs >= th) { pT0 = crossT(prevVs, vs, th); pState = 1; }
            break;
        case 1:
            if (vs >= up) pState = 2;
            else if (vs < dn) pState = 0;
            else if (prevVs < th && vs >= th) pT0 = crossT(prevVs, vs, th);
            break;
        case 2:
            if (prevVs >= th && vs < th) { pT1 = crossT(prevVs, vs, th); pState = 3; }
            break;
        case 3:
            if (vs <= dn) { pState = 0; onPulse(pT0, pT1); }
            else if (vs >= up) pState = 2;
            else if (prevVs >= th && vs < th) pT1 = crossT(prevVs, vs, th);
            break;
        }
    }

    void onPulse(double t0, double t1) {
        const double w = (t1 - t0) / spu;
        if (w >= 3.3 && w <= 6.4) normalPulse(t0, t1);
        else if (w > 15 && w < 40) broadPulse(t0);
    }

    void normalPulse(double t0, double t1) {
        if (!loopRunning) return;
        const double e = t0 - tNext;
        if (std::fabs(e) <= gate()) {
            const double alpha = goodRun < 8 ? 0.4 : 0.12, beta = goodRun < 8 ? 0.02 : 0.004;
            const double tg = tNext + alpha * e;
            P += beta * e;
            P = std::min(std::max(P, 62.8e-6 * fv), 64.9e-6 * fv);
            Pslow += (P - Pslow) * (1.0 / 2048);
            lineQ.push_back({tg, true, ++lineNo});
            tNext = tg + P;
            goodRun++; missRun = 0; oogRun = 0;
            syncQ += (1.f - syncQ) / 32.f;
            lastGoodT0 = t0; (void)t1;
        } else {
            // a pulse where none was expected: after a longer stretch of those the line timing has jumped
            if (++oogRun >= 10) {
                tNext = t0 + P; oogRun = 0; goodRun = 0;
            }
        }
    }

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
        goodRun = 0;
        syncQ += (0.f - syncQ) / 32.f;
        if (++missRun > 40 || syncQ < 0.08f) { loseLock(); }
    }

    void loseLock() {
        loopRunning = false; levelsValid = false;
        acq.clear(); pState = 0;
        syncQ = 0; haveVsync = false;
        doLog("analog TV: line sync lost");
    }

    // broad (field synchronising) pulses: a run of eqPulses of them, half a line apart
    void broadPulse(double t0) {
        if (!loopRunning) return;
        const double half = 0.5 * P;
        if (bCount > 0 && std::fabs((t0 - bLast) - half) < 0.18 * P) { bCount++; bLast = t0; }
        else { bCount = 1; bFirst = bLast = t0; }
        const int need = fmt.lines == 525 ? 6 : 5;
        if (fmtKnown && bCount == need) vsync(bFirst);
    }

    void vsync(double t_v) {
        // where does it fall on the line grid: on a line sync (first field) or half-way (second field)?
        const double d = (t_v - tNext) / P;
        const double fr = d - std::floor(d + 0.5);
        const int field = std::fabs(fr) < 0.25 ? 0 : 1;
        // plausibility: about half a frame since the last one, and the other field
        bool ok = true;
        if (haveVsync) {
            const double dt = (t_v - vsyncT) / P;                 // in lines
            const double expect = fmt.lines / 2.0;
            if (std::fabs(dt - expect) > 3.0 && std::fabs(dt - 2 * expect) > 3.0 && std::fabs(dt - expect * 3) > 3.0) ok = vsyncMiss > 6;
        }
        if (!ok) return;
        if (haveVsync) finishField();
        haveVsync = true; vsyncT = t_v; vsyncField = field; vsyncMiss = 0; vsyncCount++;
        curField = field; fieldLines = 0;
        vbiPending = true; vbiT = t_v;
    }

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

    bool acquire() {
        const size_t n = acq.size();
        std::vector<float> sub;
        for (size_t k = 0; k < n; k += 5) sub.push_back(acq[k]);
        std::sort(sub.begin(), sub.end());
        const double lo = sub[(size_t)(sub.size() * 0.02)], hi = sub[(size_t)(sub.size() * 0.997)];
        if (hi - lo < 1e-9) return false;
        // the threshold that gives the most pulses of the width of a line sync
        int bestCount = 0;
        std::vector<std::pair<double, int>> counts;
        std::vector<Cr> pl;
        // the search over thresholds runs on a copy with every 4th sample (means of four): widths of 4 us stay clear of the limits, and a failed
        // attempt, which is what a weak or no signal gives every 25 ms, costs a quarter
        std::vector<float> dec(n / 4);
        for (size_t k = 0; k < dec.size(); k++) dec[k] = 0.25f * (acq[4 * k] + acq[4 * k + 1] + acq[4 * k + 2] + acq[4 * k + 3]);
        for (int s = 1; s <= 28; s++) {
            const double f = 0.03 + 0.02 * s;
            const double th = hi - f * (hi - lo);
            scanArr(dec.data(), dec.size(), th, 0.03 * (hi - lo), pl);
            int c = 0;
            for (const auto& p : pl) { const double w = 4 * (p.t1 - p.t0) / spu; if (w >= 3.9 && w <= 5.6) c++; }
            counts.push_back({th, c});
            bestCount = std::max(bestCount, c);
        }
        const double secs = (double)n / fv;
        if (bestCount < 0.4 * secs * 15625 * 0.9) return false;
        // the middle of the thresholds that are nearly as good
        std::vector<double> good;
        for (auto& c : counts) if (c.second >= 0.92 * bestCount) good.push_back(c.first);
        const double th = good[good.size() / 2];
        scan(th, 0.03 * (hi - lo), pl);
        std::vector<Cr> np;
        for (const auto& p : pl) { const double w = (p.t1 - p.t0) / spu; if (w >= 3.9 && w <= 5.6) np.push_back(p); }
        // The line period: the neighbouring pulses give it roughly, then every pulse gets the number of its line (from the spacing to the one before)
        // and a straight line through (number, time) is fitted, throwing away the pulses that are not on it. With noise the edges jitter, but
        // hundreds of pulses fix the period to a few ppm.
        std::vector<double> sp;
        for (size_t k = 1; k < np.size(); k++) { const double d = (np[k].t0 - np[k - 1].t0) / spu; if (d > 62.5 && d < 65.5) sp.push_back(d); }
        if (sp.size() < 40) return false;
        std::sort(sp.begin(), sp.end());
        const double med = sp[sp.size() / 2];
        if (med < 63.2 || med > 64.4) return false;
        std::vector<double> ks(np.size(), 0.0);
        for (size_t k = 1; k < np.size(); k++) ks[k] = ks[k - 1] + std::round((np[k].t0 - np[k - 1].t0) / (med * spu));
        std::vector<char> in(np.size(), 1);
        double Pm = med * spu, T0 = np[0].t0;
        size_t nin = np.size();
        for (int pass = 0; pass < 5; pass++) {
            double sk = 0, st = 0, skk = 0, skt = 0;
            size_t c = 0;
            for (size_t k = 0; k < np.size(); k++) if (in[k]) { sk += ks[k]; st += np[k].t0; skk += ks[k] * ks[k]; skt += ks[k] * np[k].t0; c++; }
            if (c < 30) return false;
            const double den = (double)c * skk - sk * sk;
            if (den < 1e-9) return false;
            Pm = ((double)c * skt - sk * st) / den;
            T0 = (st - Pm * sk) / (double)c;
            const double tol = (pass < 2 ? 2.5 : 1.2) * spu;
            nin = 0;
            for (size_t k = 0; k < np.size(); k++) { in[k] = std::fabs(np[k].t0 - (T0 + ks[k] * Pm)) < tol; if (in[k]) nin++; }
        }
        if ((double)nin < 0.4 * secs * 15625 || Pm / spu < 63.2 || Pm / spu > 64.4) return false;
        size_t lastIn = 0;
        for (size_t k = 0; k < np.size(); k++) if (in[k]) lastIn = k;
        const int lines = (std::fabs(Pm / spu - 64.0) < std::fabs(Pm / spu - 63.5556)) ? 625 : 525;
        // levels
        double tipS = 0;
        int tn = 0;
        for (size_t k = 0; k < np.size(); k++) { if (!in[k]) continue; const size_t c = (size_t)(0.5 * (np[k].t0 + np[k].t1)); if (c < acq.size()) { tipS += acq[c]; tn++; } }
        const double T = tipS / std::max(1, tn), B = 2 * th - T;
        Tr = T; Br = B; thr = th; hyst = 0.18 * (T - B);
        if (!(T > B)) return false;
        P = Pslow = Pm;
        // the next line start after the data we hold
        const double last = (double)acqStart + (T0 + ks[lastIn] * Pm) - dVs;
        double tn0 = last + P;
        while (tn0 < (double)nAbs - 3 * P) tn0 += P;
        while (tn0 - P > (double)nAbs - 3 * P) tn0 -= P;
        tNext = tn0;
        loopRunning = true; levelsValid = true; goodRun = 0; missRun = 0; syncQ = 0.6f;
        lineQ.clear();
        acq.clear();
        pState = 0;
        makeFormat(lines);
        {
            char b[160];
            snprintf(b, sizeof b, "analog TV: line sync found, %d lines, line period %.3f us", lines, P / spu);
            doLog(b);
        }
        return true;
    }

    // ------------------------------------------------------------------------------ lines
    void processLines() {
        // the SECAM decoder looks 5 us past the end of the line (the bell rings)
        const double after = 20 + (colourKind == kAtvSecam ? 5.5 * spu : 0.0);
        while (!lineQ.empty() && lineQ.front().tL + P + after <= (double)nAbs) {
            const LineRec r = lineQ.front();
            lineQ.pop_front();
            if (!fmtKnown) continue;
            processLine(r);
        }
        if (vbiPending && vbiT + 14 * P + 30 <= (double)nAbs && lineGain() > 0) { captureVbi(); vbiPending = false; }
        // a field sync that did not come: after about a field and a half without one the vertical position is lost
        if (haveVsync && loopRunning && fmtKnown && (double)nAbs - vsyncT > (fmt.lines * 0.5 * 1.6) * P) {
            vsyncMiss++; vsyncT += fmt.lines * 0.5 * P; vsyncField ^= 1;     // keep counting: the next field is due
            if (vsyncMiss > 12) haveVsync = false;
        }
    }

    double lineGain() const { return (Tr - Br) / std::max(1e-9, -fmt.syncLevel); }     // raw units for one unit of normalised video

    // mean of the raw video over [a, b] samples (absolute, fractional ends rounded)
    double meanRaw(double a, double b) const {
        const uint64_t ia = (uint64_t)std::max(0.0, std::ceil(a)), ib = (uint64_t)std::max(0.0, std::floor(b));
        if (ib < ia) return rvAt(ia);
        double s = 0;
        for (uint64_t n = ia; n <= ib; n++) s += rvAt(n);
        return s / (double)(ib - ia + 1);
    }

    void processLine(const LineRec& r) {
        const double tL = r.tL;
        curNo = r.no;
        lineCount++;
        const AtvFormat& F = fmt;
        // ---- levels from this line's sync pulse and porches
        double tip = 0, bl = 0;
        if (r.hadPulse) {
            tip = meanRaw(tL + 1.4 * spu, tL + (F.syncUs - 0.7) * spu);
            double bpA = F.lines == 625 ? 8.3 : 8.1, bpB = F.lines == 625 ? 10.0 : 9.1;
            const double b1m = meanRaw(tL + bpA * spu, tL + bpB * spu), b2m = meanRaw(tL - 1.15 * spu, tL - 0.35 * spu);
            const double n1 = (bpB - bpA) * spu, n2 = 0.8 * spu;
            bl = (b1m * n1 + b2m * n2) / (n1 + n2);
            // noise on the porches (variance about the mean)
            // (SECAM has the subcarrier on the back porch: the sync tip serves there)
            if (colourKind == kAtvSecam) { bpA = 1.4; bpB = F.syncUs - 0.7; }
            const double nm = colourKind == kAtvSecam ? tip : b1m;
            double v1 = 0; int c1 = 0;
            for (uint64_t n = (uint64_t)std::ceil(tL + bpA * spu); n <= (uint64_t)std::floor(tL + bpB * spu); n++) { const double d = rvAt(n) - nm; v1 += d * d; c1++; }
            if (c1 > 3) { const double var = v1 / (c1 - 1); noiseVar += (var - noiseVar) * 0.03; if (noiseVar <= 0) noiseVar = var; }
            const double a = 1.0 / 16;
            if (levelsRestart > 0) { Tr = tip; Br = bl; levelsRestart--; }
            else { Tr += (tip - Tr) * a; Br += (bl - Br) * a; }
            hyst = std::max(0.1 * (Tr - Br), 1e-9);
            thr = 0.5 * (Tr + Br);
            if (Bline == 0) Bline = bl; else Bline += (bl - Bline) * 0.4;
            gainValid = Tr > Br;
            // carrier phase at the sync tip, for the radio side
            double si = 0, sq = 0, vi = 0, vq = 0; int cn = 0;
            const uint64_t ta = (uint64_t)std::ceil(tL + 1.4 * spu), tb = (uint64_t)std::floor(tL + (F.syncUs - 0.7) * spu);
            for (uint64_t n = ta; n <= tb; n++) { const float a1 = ri[n & kMask], b1v = rq[n & kMask]; si += a1; sq += b1v; vi += (double)a1 * a1; vq += (double)b1v * b1v; cn++; }
            if (cn > 3 && haveCarrierErr()) {
                si /= cn; sq /= cn;
                const double amp = std::hypot(si, sq);
                const double var = std::max(1e-12, (vi + vq) / cn - si * si - sq * sq);
                const double snr = amp * amp / var;
                const double ph = std::atan2(sq, si);
                carrierAmp += (amp - carrierAmp) * 0.05;
                cnVar = cnVar > 0 ? cnVar + (var - cnVar) * 0.03 : var;
                if (syncQ > 0.5f) doCarrierErr(ph, std::min(1.0, snr / 12.0));
                phErr2 += (ph * ph - phErr2) * 0.03;
                if (phErr2 < 0.12) { if (phGood < 1000) phGood++; } else phGood = 0;
                phaseLockedFlag = phGood > 60;
            }
        }
        // ---- which picture line is this?
        int row = -1, field = 0;
        bool visible = false;
        if (haveVsync) {
            const double h = (tL - vsyncT) * 2.0 / P;
            const long hr = std::lround(h);
            const int off = F.visibleStartH(vsyncField) - F.broadStart[vsyncField];
            if (hr >= off && ((hr - off) & 1) == 0) {
                const long k = (hr - off) / 2;
                if (k < F.fieldRows) { visible = true; row = (int)(2 * k + vsyncField); field = vsyncField; }
            }
        }
        const bool havePicture = visible && r.hadPulse && gainValid;
        // ---- colour: burst and chroma
        const bool wantColour = colourCapable && prm.colourOn;
        bool inVbi = false;                                      // the burst is blanked around the field sync
        if (haveVsync) {
            const double half = F.lines / 2.0;
            double x = std::fmod((tL - vsyncT) / P, half);
            if (x < 0) x += half;
            inVbi = x < 10.5 || x > half - 5.0;
        }
        secamLineOk = false;
        if (F.lines == 625 && wantColour && r.hadPulse && gainValid && haveVsync && (colourKind < 0 || colourKind == kAtvSecam)) secamLine(tL, havePicture);
        if (wantColour && r.hadPulse && gainValid && !inVbi) burstAndColour(tL, havePicture);
        if (havePicture) {
            extractPicture(tL, row, field);
            fieldLines++;
        }
        // ---- scope
        if (r.hadPulse && gainValid && (++scopeSkip >= 50) && haveVsync && visible) { scopeSkip = 0; captureLine(tL); }
        // ---- publish counters
        lineHzMeas = fv / Pslow;
    }

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
    bool measureBurst(double tL, const Lock& L, std::complex<double>& b, double& nb2) const {
        const AtvFormat& F = fmt;
        const double bsUs = F.burstStartUs > 0 ? F.burstStartUs : 5.6;
        const double a = tL + (bsUs + 0.45) * spu, e = tL + (bsUs + 1.95) * spu;      // inside both the 10-cycle PAL and the 9-cycle NTSC burst
        const uint64_t na = (uint64_t)std::ceil(a), nb = (uint64_t)std::floor(e);
        if (nb <= na + 6) return false;
        const double g = lineGain();
        if (g <= 0) return false;
        // the same measurement on the back porch after the burst (nothing there but noise): the size of the noise in the burst measurement
        const double a2 = tL + (bsUs + 2.7) * spu, e2 = tL + (bsUs + 3.9) * spu;
        const uint64_t na2 = (uint64_t)std::ceil(a2), nb2i = (uint64_t)std::floor(e2);
        auto project = [&](uint64_t n0, uint64_t n1) {
            std::complex<double> mean = 0;
            double wsum = 0;
            for (uint64_t n = n0; n <= n1; n++) {
                const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)(n - n0 + 0.5) / (double)(n1 - n0 + 1));
                mean += w * std::complex<double>(ri[n & kMask], rq[n & kMask]);
                wsum += w;
            }
            mean /= wsum;
            std::complex<double> sum = 0;
            for (uint64_t n = n0; n <= n1; n++) {
                const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)(n - n0 + 0.5) / (double)(n1 - n0 + 1));
                const std::complex<double> y = std::complex<double>(ri[n & kMask], rq[n & kMask]) - mean;
                const double ph = L.ph + 2 * kPi * L.fsc * ((double)n - L.tRef) / fv;
                sum += w * y * std::polar(1.0, -ph);
            }
            return sum / wsum;
        };
        // p = (B - r) / G, so the complex amplitude flips sign. The burst sits on the carrier at blanking level, a constant in the complex
        // baseband: project() takes that away first.
        b = -project(na, nb) / g;
        const double n1 = (double)(nb - na + 1), n2 = (double)(nb2i - na2 + 1);
        nb2 = n2 > 6 ? std::norm(project(na2, nb2i) / g) * (n2 / n1) : 0.0;     // a Hann window of n samples has noise power in proportion to 1 / n
        return true;
    }

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
    bool burstShift(double tL, double& dv) const {
        const double bsUs = fmt.burstStartUs > 0 ? fmt.burstStartUs : 5.6;
        const uint64_t na = (uint64_t)std::ceil(tL + (bsUs + 0.45) * spu), nb = (uint64_t)std::floor(tL + (bsUs + 1.95) * spu);
        const double g = lineGain();
        if (nb <= na + 6 || g <= 0) return false;
        double s = 0, ws = 0;
        for (uint64_t n = na; n <= nb; n++) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)(n - na + 0.5) / (double)(nb - na + 1));
            s += w * rvAt(n); ws += w;
        }
        dv = (Bline - s / ws) / g;
        return true;
    }

    // One line of a loop. Returns false when there was no burst to measure.
    bool stepLock(Lock& L, double tL, bool pal, double nominal, double alpha, double beta) {
        advance(L, tL);
        std::complex<double> b;
        double nb2 = 0;
        if (!measureBurst(tL, L, b, nb2)) return false;
        const double amp = std::abs(b);
        L.nz += (nb2 - L.nz) * 0.05;
        const double ampC = std::sqrt(std::max(0.0, amp * amp - L.nz));      // the size without the noise that adds to it
        L.amp += (ampC / nominal - L.amp) * (L.lines < 8 ? 0.4 : 0.1);
        L.last = b;
        if (amp < 0.3 * nominal) return true;               // no burst on this line (or too weak to steer by)
        if (pal) { const int pr = (int)(curNo & 1); L.vacc[pr] += (b.real() - L.vacc[pr]) * 0.2; }
        double err = 0;
        bool have = false;
        if (pal) {
            // the bursts of two lines in a row are at 45 and 135 degrees: their sum points at 90
            if (L.prevValid && L.prevLine + 1 == curNo) {
                const std::complex<double> sm = b + L.prev;
                if (std::abs(sm) > 0.2 * (amp + std::abs(L.prev))) { err = std::arg(sm) - kPi / 2; have = true; }
            }
            L.prev = b; L.prevValid = true; L.prevLine = curNo;
        } else {
            err = std::arg(b) - kPi / 2; have = true;
        }
        if (have) {
            while (err > kPi) err -= 2 * kPi;
            while (err < -kPi) err += 2 * kPi;
            L.ph += alpha * err;
            L.fsc += beta * err * (fmt.lineHz / (2 * kPi));
            L.fsc = std::min(std::max(L.fsc, L.fsc0 - 3000), L.fsc0 + 3000);
            L.err2 += (err * err - L.err2) * (L.lines < 8 ? 0.4 : 0.06);
            L.lines++;
        }
        return true;
    }

    void burstAndColour(double tL, bool picture) {
        (void)picture;
        const AtvFormat& F = fmt;
        if (colourKind < 0) {                      // still deciding what the colour is: every candidate gets a loop of its own
            if (cands.empty()) {
                if (fmt.lines == 625) { cands.resize(2); cands[0].reset(4433618.75); cands[1].reset(3582056.25); }
                else { cands.resize(2); cands[0].reset(455.0 / 2 * (4.5e6 / 286)); cands[1].reset(909.0 / 4 * (4.5e6 / 286)); }
                candLines = 0;
            }
            const bool pal625 = fmt.lines == 625;
            for (size_t k = 0; k < cands.size(); k++) {
                const bool pal = pal625 ? true : (k == 1);
                const double nom = fmt.lines == 625 ? 3.0 / 14 : 0.2;
                const bool fast = cands[k].lines < 40;
                // a candidate that is neither a good tone nor locking is not worth the time: the loops are cheap, so all run
                stepLock(cands[k], tL, pal, nom, fast ? 0.5 : 0.1, fast ? 0.04 : 0.004);
            }
            if (++candLines >= 160) decideColour();
            return;
        }
        if (colourKind == kAtvSecam) return;                  // secamLine() does it
        if (colourKind == kAtvMono) { colourLocked = false; return; }
        if (!mainLock.init) { mainLock.reset(F.fscHz); }
        const bool fast = mainLock.lines < 40;
        if (!stepLock(mainLock, tL, F.pal, F.burstAmp, fast ? 0.5 : 0.1, fast ? 0.04 : 0.004)) return;
        burstEma = mainLock.amp;
        if (colourLocked) {
            // The picture gain follows the burst when the sync is compressed. The burst's fundamental is too small there, because the lower half of
            // each cycle is squeezed like the sync is: with the level shift of the burst the amplitude of the half that is not squeezed is
            // recovered (a cycle with the lower half scaled by k has fundamental (1 + k) / 2 and shift (1 - k) / pi, in units of its peak).
            double dv = 0;
            if (burstShift(tL, dv)) shiftEma += (dv - shiftEma) * 0.02;
            compEma += (burstEma - compEma) * 0.02;
            const double est = compEma + 0.5 * kPi * std::max(0.0, shiftEma) / F.burstAmp;
            compK = est > 1.12 && syncSqueeze() > 0.12 ? est : 1.0;       // a burst that is off on its own (a ghost, a beat) is not a squeezed sync
        }
        // The burst is judged by its phase scatter as well as its size: noise alone gives a burst of about the right size with a random phase.
        // The scatter of one line's burst against the loop is about 0.3 rad at a carrier-to-noise ratio of 20 dB.
        if (burstEma < 0.2 || mainLock.err2 > 1.0) { if (++killCount > 12) killer = true; }
        else if (burstEma > 0.3 && mainLock.err2 < 0.5) { killCount = 0; killer = false; }
        if (F.pal) vsign = mainLock.vacc[curNo & 1] >= 0 ? 1 : -1;            // V as is on the lines whose burst is at 45 degrees; the sign alternates line by line
        chromaErr = std::sqrt(mainLock.err2);
        colourLocked = mainLock.lines > 30 && !killer;
    }
    int candLines = 0;

    void decideColour() {
        const int lines = fmt.lines;
        int best = -1;
        double bestErr = 1e9;
        for (size_t k = 0; k < cands.size(); k++) {
            const Lock& c = cands[k];
            if (c.amp < 0.5 || c.lines < 50 || c.err2 > 0.4) continue;      // 0.4 rad^2: about 36 degrees rms; a loop that follows noise has more than 3
            if (c.err2 < bestErr) { bestErr = c.err2; best = (int)k; }
        }
        int kind = kAtvMono;
        if (best >= 0) kind = lines == 625 ? kAtvPal : kAtvNtsc;
        // SECAM: the subcarrier at the rest frequency of the line on the back porch, alternating line by line, or the identification lines of the
        // field blanking. A signal without either may still have them in a field or two: keep looking for a while before it counts as monochrome.
        bool secamFound = false;
        if (best < 0 && lines == 625 && secamReady) {
            const bool viaLines = sLead >= 30 && sPairs >= 20 && sAlt >= 0.85 * sPairs;
            const bool viaField = sBottle >= 6;
            secamFound = viaLines || viaField;
            if (!secamFound && candLines < 700) return;
            if (secamFound) { kind = kAtvSecam; secamHow = viaLines ? (viaField ? 3 : 1) : 2; }
        }
        // PAL-M is the second candidate of the 525-line systems
        bool palM = best == 1 && lines == 525;
        bool palN = best == 1 && lines == 625;
        if (kind == kAtvNtsc && palM) kind = kAtvPal;
        char b[400];
        {
            std::string d;
            for (size_t k = 0; k < cands.size(); k++) { char t[100]; snprintf(t, sizeof t, " [%.0f Hz: burst %.0f%%, %d lines, phase error %.1f deg]", cands[k].fsc, cands[k].amp * 100, cands[k].lines, std::sqrt(cands[k].err2) * 180 / kPi); d += t; }
            doLog(("analog TV: colour search:" + d).c_str());
        }
        if (secamFound) snprintf(b, sizeof b, "analog TV: colour search: SECAM (%s; lead-in found on %d lines, %d of %d pairs alternate, %d identification line pairs)", secamHow == 3 ? "lead-in on the lines and identification lines" : secamHow == 1 ? "lead-in on the lines" : "identification lines of the field blanking", sLead, sAlt, sPairs, sBottle);
        else snprintf(b, sizeof b, "analog TV: colour search: %s", best < 0 ? "no colour subcarrier (monochrome)" : lines == 625 ? (palN ? "PAL-N burst" : "PAL burst") : (palM ? "PAL-M burst" : "NTSC burst"));
        doLog(b);
        colourKind = prm.forceColour >= 0 ? prm.forceColour : kind;
        // PAL on N means the Argentine system, whose sound is at +4.5 MHz: the subcarrier decides here
        if (palN) soundSpacing = 4.5;
        makeFormat(lines);
        mainLock.reset(fmt.fscHz);
        if (best >= 0 && colourKind == (lines == 625 ? kAtvPal : (palM ? kAtvPal : kAtvNtsc))) { mainLock = cands[(size_t)best]; }
        cands.clear();
        burstEma = 1; killCount = 0; killer = false;
        if (colourKind == kAtvSecam) { colourLocked = true; secAmpEma = 1; secKill = 0; }
    }
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
        const double a = secamOut(tL + 6.6 * spu, sa), b = secamOut(tL + 9.9 * spu, sa);
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
    void secamBottle(double tL, double g, long L) {
        const long sa = secamSpan(tL + 22 * spu, tL + 64 * spu);
        const size_t M = secam.baseband(zsi.data(), zsq.data(), zsi.size(), secBB);
        const double a = secamOut(tL + 30 * spu, sa), b = secamOut(tL + 58 * spu, sa);
        if (M < 10 || a < 0 || b >= (double)M) return;
        double f, am;
        secam.tone(secBB, (size_t)std::ceil(a), (size_t)std::floor(b) + 1, f, am);
        if (am / g < 0.1) return;
        // the trapezoid has reached +350 kHz above f0R (4.756 MHz) or -350 kHz below f0B (3.9 MHz) by now
        const int kind = std::fabs(f - kSecamMaxR) < 100e3 ? 1 : std::fabs(f - kSecamMinB) < 100e3 ? 0 : -1;
        newFieldVote();
        if (kind < 0) { lastBottleKind = -1; return; }
        rVote += (kind == 1) == ((L & 1) == 1) ? 4.0 : -4.0;           // these lines are sure
        if (lastBottleKind >= 0 && lastBottleLine + 1 == L && kind != lastBottleKind) sBottle++;      // the lines alternate
        lastBottleKind = kind; lastBottleLine = L;
    }

    // Everything SECAM that one line needs: field identification lines, the lead-in during the colour search, and the colour of a picture line.
    void secamLine(double tL, bool havePicture) {
        const AtvFormat& F = fmt;
        const double g = lineGain();
        if (!secamReady || g <= 0) return;
        const long hr = std::lround((tL - vsyncT) * 2.0 / P);
        long H = ((long)F.broadStart[vsyncField] + hr) % 1250;
        if (H < 0) H += 1250;
        const long L = (H >> 1) + 1;                                // the line number of the standard
        if ((vsyncField == 0 && L >= 7 && L <= 15) || (vsyncField == 1 && L >= 320 && L <= 328)) { secamBottle(tL, g, L); return; }
        if (!havePicture) return;
        if (colourKind < 0) {                                       // colour search: does the lead-in alternate between the two rest frequencies?
            const long sa = secamSpan(tL + 1 * spu, tL + 14 * spu);
            secam.baseband(zsi.data(), zsq.data(), zsi.size(), secBB);
            double amp;
            const int kind = secamLead(tL, g, secBB, sa, amp);
            if (kind >= 0) {
                sLead++;
                if (lastLeadKind >= 0 && lastLeadNo + 1 == curNo) { sPairs++; if (kind != lastLeadKind) sAlt++; }
                lastLeadKind = kind; lastLeadNo = curNo;
            } else lastLeadKind = -1;
            return;
        }
        const long sa = secamSpan(tL + 1 * spu, tL + 69 * spu);
        const size_t M = secam.baseband(zsi.data(), zsq.data(), zsi.size(), secBB);
        if (M < 20) return;
        double leadAmp = 0, fLead = 0;
        secamLead(tL, g, secBB, sa, leadAmp, &fLead);
        // Which of the two the line is: the lead-in tone says so, softly (it is a measurement of a short stretch and noisy), and the lines of the
        // field vote: the kinds alternate, so every line says something about every other line.
        newFieldVote();
        const bool haveTone = leadAmp >= 0.04 && fLead > 4.15e6 && fLead < 4.52e6;
        if (haveTone) {
            const double e = std::min(1.5, std::max(-1.5, (fLead - 0.5 * (kSecamF0R + kSecamF0B)) / 78e3)) * std::min(1.0, leadAmp / 0.08);
            rVote = rVote * 0.97 + ((L & 1) == 1 ? e : -e);
        }
        bool isR;
        if (std::fabs(rVote) >= 0.6) isR = ((L & 1) == 1) == (rVote > 0);
        else if (haveTone) isR = fLead > 0.5 * (kSecamF0R + kSecamF0B);
        else return;                                                // not known yet: this line stays without colour
        const double pxUs = F.activeUs / W;
        const double aA = std::max(0.0, secamOut(tL + F.blankEndUs * spu, sa)), aB = std::max(0.0, secamOut(tL + (F.blankEndUs + F.activeUs) * spu, sa));
        secam.demod(secBB, isR, secD, (float)(0.3 * kSecamM0 * g), (size_t)aA, (size_t)aB);
        const float chAmp = secam.amp() / (float)(kSecamM0 * g);    // nominal 1: the bell restores M0
        secAmpEma += (chAmp - secAmpEma) * 0.1f;
        burstEma = secAmpEma;
        if (secAmpEma < 0.25f) { if (++secKill > 12) killer = true; }
        else if (secAmpEma > 0.4f) { secKill = 0; killer = false; }
        colourLocked = !killer;
        chromaErr = 0;
        // colour difference at the pixels (cubic through the values of D')
        secRY.resize((size_t)W); secBY.resize((size_t)W); secU.resize((size_t)W); secV.resize((size_t)W);
        std::vector<float>& dst = isR ? secRY : secBY;
        const long nD = (long)secD.size();
        for (int x = 0; x < W; x++) {
            const double u = secamOut(tL + (F.blankEndUs + (x + 0.5) * pxUs) * spu, sa) + 0.5;      // D'[m] lies half an output before output m
            const long i0 = (long)std::floor(u);
            const float fr = (float)(u - (double)i0);
            auto at = [&](long i) { return secD[(size_t)std::min(nD - 1, std::max(0L, i))]; };
            const float t2 = fr * fr, t3 = t2 * fr;
            const float d = (-0.5f * t3 + t2 - 0.5f * fr) * at(i0 - 1) + (1.5f * t3 - 2.5f * t2 + 1.f) * at(i0) + (-1.5f * t3 + 2.f * t2 + 0.5f * fr) * at(i0 + 1) + (0.5f * t3 - 0.5f * t2) * at(i0 + 2);
            dst[(size_t)x] = isR ? -d / 1.902f : d / 1.505f;     // D'R = -1.902 (R - Y), D'B = 1.505 (B - Y)
        }
        (isR ? secRYNo : secBYNo) = curNo;
        // the other colour difference is the one of the line before (the delay line of a SECAM decoder)
        const bool haveRY = secRYNo == curNo || secRYNo + 1 == curNo, haveBY = secBYNo == curNo || secBYNo + 1 == curNo;
        for (int x = 0; x < W; x++) {
            secV[(size_t)x] = haveRY ? 0.877f * secRY[(size_t)x] : 0.f;
            secU[(size_t)x] = haveBY ? 0.493f * secBY[(size_t)x] : 0.f;
        }
        secamLineOk = true;
    }

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
    void designChroma() {
        bpf.clear();
        if (fmt.fscHz <= 0 && !fmt.secam) return;
        const double fsc = fmt.secam ? kSecamBellHz : fmt.fscHz, nyq = 0.5 * fv, wing = fmt.secam ? 0.65e6 : 1.25e6;     // SECAM: the FM subcarrier fills 3.9 to 4.76 MHz
        auto resp = [&](double f) -> std::complex<double> {
            const double af = std::fabs(f);
            const double lo = fsc - wing, hi = fsc + wing, tr = fmt.secam ? 0.5e6 : 0.9e6;
            double r = 0;
            if (af >= lo && af <= std::min(hi, nyq)) r = 1;
            else if (af < lo && af > lo - tr) r = 0.5 + 0.5 * std::cos(kPi * (lo - af) / tr);
            else if (af > hi && af < hi + tr) r = 0.5 + 0.5 * std::cos(kPi * (af - hi) / tr);
            return r;
        };
        const auto t = fromResponse(resp, fv, 25, 5.0);
        bpf.resize(t.size());
        for (size_t k = 0; k < t.size(); k++) bpf[k] = (float)t[k].real();
        if (fmt.secam) return;
        zdec = std::max(1, (int)std::floor(fv / 5e6));
        lpC = lowpass(0.9e6, std::min(2.6e6, 0.9 * fv / 2 - 0.1e6), fv, 38, 31);
    }

    void extractPicture(double tL, int row, int field) {
        (void)field;
        const int W = this->W;                    // a copy: the byte stores below could change the member as far as the compiler knows, which keeps loops from vectorising
        const AtvFormat& F = fmt;
        const double g = lineGain();
        if (g <= 0) return;
        // the span of samples this line's active part needs, with margins for the filters
        const double a0 = tL + (F.blankEndUs - 1.0) * spu, a1 = tL + (F.blankEndUs + F.activeUs + 1.0) * spu;
        const long sa = (long)std::floor(a0) - 24, sb = (long)std::ceil(a1) + 24;
        const size_t len = (size_t)(sb - sa + 1);
        const bool useColour = (colourKind == kAtvPal || colourKind == kAtvNtsc) && !bpf.empty();
        const bool colourNow = useColour && colourLocked && !killer && colourCapable && prm.colourOn;
        const bool secamNow = colourKind == kAtvSecam && secamLineOk && !bpf.empty() && !killer && colourCapable && prm.colourOn;
        std::vector<float>& rawv = yv;
        rawv.resize(len);
        for (size_t k = 0; k < len; k++) rawv[k] = rv[(uint64_t)(sa + (long)k) & kMask];
        const double clampB = Bline;
        // luminance: the video without the colour subcarrier band (or all of it for a picture without colour)
        std::vector<float>& lum = lumBuf;
        lum.assign(rawv.begin(), rawv.end());
        std::vector<float>& zr = zi;
        std::vector<float>& zq_ = zq;
        size_t nz = 0;
        const int Dz = zdec;
        if (colourNow || secamNow) {
            const int nb = (int)bpf.size(), mb = nb / 2;
            bp.resize(len);
            std::fill(bp.begin(), bp.end(), 0.f);
            desamp(rawv.data(), 1, bpf.data(), bp.data() + mb, (int)(len - (size_t)nb + 1), nb);
            for (size_t k = 0; k < len; k++) lum[k] -= bp[k];
        }
        if (colourNow) {
            // chroma: mix the baseband down by the reference, low-pass at every Dz-th sample
            ti.resize(len); tq.resize(len);
            const double dph = 2 * kPi * mainLock.fsc / fv;
            const double ph0 = mainLock.ph + dph * ((double)sa - mainLock.tRef);
            // the I and Q rings of this line in order (the ring may wrap), then four phasors a sample apart, each stepped by four samples,
            // so that the multiplications do not wait for each other (explicit float arithmetic: the complex operators of the library are slow)
            yiL.resize(len); yqL.resize(len);
            {
                const size_t s0 = (size_t)((uint64_t)sa & kMask), first = std::min(len, kRing - s0);
                std::memcpy(yiL.data(), &ri[s0], first * sizeof(float)); std::memcpy(yqL.data(), &rq[s0], first * sizeof(float));
                if (first < len) { std::memcpy(yiL.data() + first, ri.data(), (len - first) * sizeof(float)); std::memcpy(yqL.data() + first, rq.data(), (len - first) * sizeof(float)); }
            }
            const std::complex<double> pd0 = std::polar(1.0, -ph0), rd = std::polar(1.0, -dph);
            float pr[4], pim[4];
            for (int q = 0; q < 4; q++) { const std::complex<float> v(pd0 * std::pow(rd, q)); pr[q] = v.real(); pim[q] = v.imag(); }
            const std::complex<float> r4(std::pow(rd, 4));
            const float r4r = r4.real(), r4i = r4.imag();
            size_t k = 0;
            for (; k + 4 <= len; k += 4) {
                for (int q = 0; q < 4; q++) {
                    const float yi = yiL[k + (size_t)q], yq = yqL[k + (size_t)q];
                    ti[k + (size_t)q] = yi * pr[q] - yq * pim[q];
                    tq[k + (size_t)q] = yi * pim[q] + yq * pr[q];
                    const float nr = pr[q] * r4r - pim[q] * r4i;
                    pim[q] = pr[q] * r4i + pim[q] * r4r;
                    pr[q] = nr;
                }
                if ((k & 127) == 124) for (int q = 0; q < 4; q++) { const float m = 1.f / std::sqrt(pr[q] * pr[q] + pim[q] * pim[q]); pr[q] *= m; pim[q] *= m; }
            }
            for (int q = 0; k + (size_t)q < len; q++) {
                const float yi = yiL[k + (size_t)q], yq = yqL[k + (size_t)q];
                ti[k + (size_t)q] = yi * pr[q] - yq * pim[q];
                tq[k + (size_t)q] = yi * pim[q] + yq * pr[q];
            }
            const int nt = (int)lpC.size(), m = nt / 2;
            // outputs for k = m, m + Dz, ...: z[j] belongs to sample m + j Dz
            nz = (len - (size_t)nt) / (size_t)Dz + 1;
            zr.resize(nz); zq_.resize(nz);
            desamp(ti.data(), Dz, lpC.data(), zr.data(), (int)nz, nt);
            desamp(tq.data(), Dz, lpC.data(), zq_.data(), (int)nz, nt);
            zoff = (double)m;
        }
        // pixels
        const double pxUs = F.activeUs / W;
        const float setup = (float)F.setup, scl = 1.f / (1.f - setup);
        const double advSamp = F.chromaDelayNs * 1e-3 * spu;                      // the transmitter advanced the chrominance: look later
        const float gsat = prm.saturation * scl;
        const float ch = std::cos((float)(prm.hueDeg * kPi / 180)), sh = std::sin((float)(prm.hueDeg * kPi / 180));
        const bool pal = F.pal, hue = prm.hueDeg != 0;
        const float accGain = colourNow ? (float)std::min(3.0, std::max(0.33, 1.0 / std::max(0.05, burstEma))) : 1.f;
        const float invG = (float)(1.0 / g);
        // Sync compression (a transmitter or modulator that squeezes the sync pulses) makes the sync-referenced gain too high. The burst is as
        // high as the sync pulse in every system here (300 mV against 300 mV, 40 IRE against 40 IRE), so a burst that is much larger than that
        // gives the gain instead.
        const float invGY = invG / (float)compK;
        uint8_t* dst = &fbuf[(size_t)row * W * 4];
        float whitePk = 0;
        const bool avgPrev = pal && prevValid && prevLineNo + 1 == curNo;
        // luminance at the pixels: 8-tap windowed sinc, positions stepped along the line
        const float stepY = (float)(pxUs * spu), pos0 = (float)(tL + (F.blankEndUs + 0.5 * pxUs) * spu - (double)sa);
        yRowP.resize((size_t)W);
        {
            const float* kt = kern.data();
            const float* lp = lum.data();
            float* out = yRowP.data();
            float pos = pos0;
            for (int x = 0; x < W; x++, pos += stepY) {
                const int i0 = (int)pos;
                const int ph = (int)((pos - (float)i0) * (float)kInterpPhases + 0.5f);
                const float* k = kt + (size_t)ph * kInterpTaps;
                const float* q = lp + i0 - (kInterpTaps / 2 - 1);
                // four partial sums (the eight taps in two halves), so that the additions do not wait for each other
                const float a0 = k[0] * q[0] + k[4] * q[4], a1 = k[1] * q[1] + k[5] * q[5], a2 = k[2] * q[2] + k[6] * q[6], a3 = k[3] * q[3] + k[7] * q[7];
                out[x] = (a0 + a1) + (a2 + a3);
            }
        }
        // chrominance at every second pixel (cubic), linear in between
        if (colourNow) {
            cuRow.resize((size_t)W + 2); cvRow.resize((size_t)W + 2);
            // every second pixel (and one more at each end): the position in the filtered chrominance and the four cubic weights first
            // (plain arithmetic over arrays, which vectorises), then the sums
            const double advPos = advSamp, invDz = 1.0 / Dz;
            const int nx = (W + 2 + 1) / 2;
            czP.resize((size_t)nx); czW.resize((size_t)nx * 4); czIdx.resize((size_t)nx);
            int* czI = czIdx.data();
            float* czF = czP.data();
            const float pzBase = (float)(((double)pos0 + advPos - zoff) * invDz), pzStep = (float)((double)stepY * invDz);
            for (int j = 0; j < nx; j++) {
                const int xc = std::min(2 * j, W - 1);
                const float pz = pzBase + (float)xc * pzStep;
                const int i0 = (int)pz;
                czF[j] = pz - (float)i0;
                czI[j] = std::min(std::max(i0 - 1, 0), (int)nz - 4);
            }
            float* cw = czW.data();
            for (int j = 0; j < nx; j++) {
                const float fr = czF[j], t2 = fr * fr, t3 = t2 * fr;
                cw[4 * j] = -0.5f * t3 + t2 - 0.5f * fr; cw[4 * j + 1] = 1.5f * t3 - 2.5f * t2 + 1.f;
                cw[4 * j + 2] = -1.5f * t3 + 2.f * t2 + 0.5f * fr; cw[4 * j + 3] = 0.5f * t3 - 0.5f * t2;
            }
            const float* zrp = zr.data(); const float* zqp = zq_.data();
            const float gU = invG * accGain, gV = invG * accGain * (pal ? (float)vsign : 1.f);
            for (int j = 0; j < nx; j++) {
                const int x = 2 * j;
                const float* a = zrp + czI[j]; const float* b = zqp + czI[j];
                const float w0 = cw[4 * j], w1 = cw[4 * j + 1], w2 = cw[4 * j + 2], w3 = cw[4 * j + 3];
                const float re = w0 * a[0] + w1 * a[1] + w2 * a[2] + w3 * a[3], im = w0 * b[0] + w1 * b[1] + w2 * b[2] + w3 * b[3];
                // z in the raw domain, p = -z / G: z_p = sV - jU for a locked reference
                cuRow[(size_t)x] = im * gU;
                cvRow[(size_t)x] = -re * gV;
                if (x + 1 < W + 2) { cuRow[(size_t)x + 1] = 0; cvRow[(size_t)x + 1] = 0; }
            }
            for (int x = 1; x < W; x += 2) { cuRow[(size_t)x] = 0.5f * (cuRow[(size_t)x - 1] + cuRow[(size_t)x + 1]); cvRow[(size_t)x] = 0.5f * (cvRow[(size_t)x - 1] + cvRow[(size_t)x + 1]); }
            for (int x = 0; x < W; x++) { uRow[(size_t)x] = cuRow[(size_t)x]; vRow[(size_t)x] = cvRow[(size_t)x]; }
        } else if (secamNow) {
            cuRow.resize((size_t)W); cvRow.resize((size_t)W);
            for (int x = 0; x < W; x++) { cuRow[(size_t)x] = secU[(size_t)x]; cvRow[(size_t)x] = secV[(size_t)x]; }
        }
        const float* __restrict yp = yRowP.data();
        if (colourNow || secamNow) {
            // one pass over the row: luminance, chrominance (with the previous line on PAL and the hue rotation), the BT.470 matrix (see atvYuvToRgb)
            // and the bytes. Without the previous line the chrominance is averaged with itself, which leaves it as it is.
            const float cY = (float)clampB, ky = invGY * scl, ko = setup * scl;
            const float* __restrict cu = cuRow.data(); const float* __restrict cv = cvRow.data();
            const float* __restrict pu = avgPrev ? prevU.data() : cuRow.data(); const float* __restrict pv = avgPrev ? prevV.data() : cvRow.data();
            const float hs = hue && !secamNow ? sh : 0.f, hc = hue && !secamNow ? ch : 1.f;
            uint8_t* __restrict o = dst;
            // the white peak apart from the pixels (a running maximum would keep the compiler from vectorising the loop), in four lanes
            float m4[4] = {0, 0, 0, 0};
            for (int x = 0; x + 4 <= W; x += 4) for (int l = 0; l < 4; l++) m4[l] = std::max(m4[l], (cY - yp[x + l]) * ky - ko);
            float wp = std::max(std::max(m4[0], m4[1]), std::max(m4[2], m4[3]));
            for (int x = W & ~3; x < W; x++) wp = std::max(wp, (cY - yp[x]) * ky - ko);
            for (int x = 0; x < W; x++) {
                const float Y = (cY - yp[x]) * ky - ko;
                const float u0 = 0.5f * (cu[x] + pu[x]) * gsat, v0 = 0.5f * (cv[x] + pv[x]) * gsat;
                const float U = u0 * hc - v0 * hs, V = u0 * hs + v0 * hc;
                const float r = (Y + 1.1403f * V) * 255.f + 0.5f, g = (Y - 0.3939f * U - 0.5808f * V) * 255.f + 0.5f, b = (Y + 2.0284f * U) * 255.f + 0.5f;
                o[4 * x] = (uint8_t)std::min(255.f, std::max(0.f, r));
                o[4 * x + 1] = (uint8_t)std::min(255.f, std::max(0.f, g));
                o[4 * x + 2] = (uint8_t)std::min(255.f, std::max(0.f, b));
                o[4 * x + 3] = 255;
            }
            whitePk = std::max(whitePk, wp);
        } else {
            uint8_t* __restrict o = dst;
            const float cB = (float)clampB;
            float m4[4] = {whitePk, whitePk, whitePk, whitePk};
            for (int x = 0; x + 4 <= W; x += 4) for (int l = 0; l < 4; l++) m4[l] = std::max(m4[l], ((cB - yp[x + l]) * invGY - setup) * scl);
            float wp = std::max(std::max(m4[0], m4[1]), std::max(m4[2], m4[3]));
            for (int x = W & ~3; x < W; x++) wp = std::max(wp, ((cB - yp[x]) * invGY - setup) * scl);
            for (int x = 0; x < W; x++) {
                float Y = (cB - yp[x]) * invGY;
                Y = (Y - setup) * scl;
                const uint8_t v8 = to8(Y);
                o[4 * x] = v8; o[4 * x + 1] = v8; o[4 * x + 2] = v8; o[4 * x + 3] = 255;
            }
            whitePk = wp;
        }
        if (colourNow && pal) { prevU = uRow; prevV = vRow; prevValid = true; prevLineNo = curNo; }
        else prevValid = false;
        whiteTrack = std::max(whiteTrack * 0.999f, whitePk);
        whiteLast = std::max(whiteLast, whitePk);
        anyPicture = true;
        // (SECAM: a line the decoder could not identify stays without colour, but the picture is a colour picture)
        lastColourNow = colourNow || secamNow || (colourKind == kAtvSecam && colourLocked && !killer && colourCapable && prm.colourOn);
    }
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

    void publish(int field) {
        if (!owner || !owner->frameReady || W <= 0) return;
        auto f = std::make_shared<AtvFrame>();
        f->seq = ++frameSeq;
        f->width = W; f->height = Hh;
        f->fieldRate = fmt.fieldHz;
        f->colour = lastColourNow;
        f->field = field;
        f->rgba = fbuf;
        if (prm.deinterlace == 1) {            // bob: this field's rows, the others made from their neighbours
            for (int r = 0; r < Hh; r++) {
                if ((r & 1) == field) continue;
                const int a = r > 0 ? r - 1 : r + 1, b = r + 1 < Hh ? r + 1 : r - 1;
                for (int x = 0; x < W * 4; x++) f->rgba[(size_t)r * W * 4 + (size_t)x] = (uint8_t)(((int)fbuf[(size_t)a * W * 4 + (size_t)x] + (int)fbuf[(size_t)b * W * 4 + (size_t)x]) / 2);
            }
        }
        doFrame(f);
    }

    // ------------------------------------------------------------------------------ scope
    void captureLine(double tL) {
        const double g = lineGain();
        if (g <= 0) return;
        const int n = 512;
        lineWave.assign((size_t)n, 0.f);
        const double a = tL - 2.0 * spu, span = P;
        for (int k = 0; k < n; k++) {
            const double t = a + span * k / n;
            const long i0 = (long)std::floor(t);
            const double fr = t - (double)i0;
            const double r = rvAt((uint64_t)i0) * (1 - fr) + rvAt((uint64_t)i0 + 1) * fr;
            lineWave[(size_t)k] = (float)((Bline - r) / g);
        }
    }

    void captureVbi() {
        const double g = lineGain();
        const int n = 512;
        vbiWave.assign((size_t)n, 0.f);
        const double a = vsyncT - 3 * P, span = 15 * P;
        if (nAbs < (uint64_t)(a + span) || a < (double)nAbs - (double)kRing + 100) return;
        for (int k = 0; k < n; k++) {
            const double t0 = a + span * k / n, t1 = a + span * (k + 1) / n;
            double s = 0; int c = 0;
            for (uint64_t m = (uint64_t)t0; m < (uint64_t)t1; m++) { s += rvAt(m); c++; }
            vbiWave[(size_t)k] = c ? (float)((Bline - s / c) / g) : 0.f;
        }
    }

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

// ------------------------------------------------------------------------------------------------ the class

AtvVideo::AtvVideo() : p_(std::make_unique<Impl>()) {}
AtvVideo::~AtvVideo() = default;
void AtvVideo::configure(double r, bool c) { p_->configure(r, c); p_->owner = this; }
void AtvVideo::reset() { Impl& s = *p_; std::fill(s.rv.begin(), s.rv.end(), 0.f); s.nAbs = 0; s.fullReset(); }
void AtvVideo::setParams(const AtvVideoParams& p) {
    Impl& s = *p_;
    const bool re = p.forceSys != s.prm.forceSys || p.forceColour != s.prm.forceColour || p.setupMode != s.prm.setupMode || p.chromaDelayNs != s.prm.chromaDelayNs;
    s.prm = p;
    if (re && s.fmtKnown) {
        if (p.forceColour >= 0 || !s.colourCapable) s.colourKind = s.startColour();
        s.makeFormat(s.fmt.lines);
    }
}
void AtvVideo::setSoundSpacing(double mhz) { p_->soundSpacing = mhz; }
void AtvVideo::setSlicerWidth(double us) { p_->setSlicerWidth(us); p_->fullReset(); }
void AtvVideo::setNoiseScale(double s) { p_->noiseScale = s; }
void AtvVideo::process(const float* v, const float* i, const float* q, size_t n) { p_->process(v, i, q, n); }
bool AtvVideo::wantSyncDetector() const { return p_->phaseLockedFlag && p_->loopRunning; }
void AtvVideo::syncDetectorChanged(bool on) {
    Impl& s = *p_;
    if (on == s.syncDetOn) return;
    s.syncDetOn = on;
    s.levelsRestart = 3;                            // the detectors differ in level: take the next lines as they are
}
bool AtvVideo::lineLocked() const { return p_->loopRunning && p_->syncQ > 0.5f; }
bool AtvVideo::fieldLocked() const { return lineLocked() && p_->haveVsync && p_->vsyncMiss < 6; }
double AtvVideo::secsSinceLineLock() const { return 0; }
double AtvVideo::soundSpacingHint() const { return p_->soundSpacing; }
AtvVideo::Info AtvVideo::info() const { return p_->info(); }
AtvFormat AtvVideo::format() const { return p_->fmt; }

} // namespace dect2
