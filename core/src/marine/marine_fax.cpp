// Weather fax (radiofax) on USB audio: decoder and an endless test transmission.
//
// Facts used (checked against fldigi src/wefax/wefax.cxx, which follows HAMFAX and ITU-T T.3 / WMO No. 386):
//  - image width = IOC x pi cut to an integer (ioc_to_width): 576 -> 1809, 288 -> 904
//  - start tone 300 Hz (IOC 576) or 675 Hz (IOC 288), stop tone 450 Hz (5 s), then 10 s of black (tx_apt_stop)
//  - black 1500 Hz, white 2300 Hz, carrier 1900 Hz, deviation +-400 Hz
//  - phasing line: white for the first 2.5 % and the last 2.5 % of the line, black in between (a 5 % white pulse
//    centred on the line boundary), then one white line before the image (tx phasing, ENDPHASING)
// Not covered: inverted phasing (black pulse on white), colour fax, line rates other than 60/90/100/120/180/240.
#include "dect2/marine/marine_fax.h"
#include "dect2/common/window.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kBlackHz = 1500.0, kWhiteHz = 2300.0, kCentreHz = 1900.0;
constexpr double kWideSnrDb = 22.0;   // above this the picture comes through the wide filter (sharper); below, the narrow one (less noise)
constexpr int kLpmChoices[] = {60, 90, 100, 120, 180, 240};
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
using cplx = std::complex<double>;

double bessel0(double x) { return dect2::window::besselI0(x, 40, 1e-12); }

} // namespace

int faxImageWidth(int ioc) { return ioc > 0 ? static_cast<int>(ioc * kPi) : 0; }

// ------------------------------------------------------------------------------------------------------------------
// Decoder
// ------------------------------------------------------------------------------------------------------------------

struct FaxDecoder::State {
    // Settings written by other threads.
    std::atomic<int> lpmSet{0}, iocSet{0}, maxLines{8000};
    std::atomic<double> manualPpm{0};
    std::atomic<bool> autoSlant{true};

    std::mutex procMtx;                          // push / configure / reset
    std::mutex pubMtx;                           // what status() and latestImage() read
    FaxStatus pubStatus;
    FaxImage pubImage;
    uint64_t imgSeq = 0;

    FaxStatus st;                                // working copy of the status

    // Front end: audio -> complex baseband around 1900 Hz -> low pass -> instantaneous frequency f in Hz.
    double fs = 12000;
    bool ready = false;
    std::vector<double> taps, tapsW;             // narrow (sync, and the picture of a weak signal) and wide (the picture of a strong one), same length
    std::vector<cplx> dl;
    size_t dlPos = 0;
    double gd = 0;                               // group delay of the low pass in samples
    cplx osc{1, 0}, rot{1, 0};
    int oscCount = 0;
    double dcX = 0, dcY = 0, dcR = 0.99;
    cplx zPrev{0, 0}, zPrevW{0, 0};
    double avgPow = 0, alphaPow = 0, prevF = kCentreHz, avgPowW = 0, prevFW = kCentreHz;

    // History of f as running sums, so that the mean over any fractional interval costs two look-ups.
    // cum[i] = sum of f over samples [0, base + i); sample n covers [n - 0.5, n + 0.5).
    std::vector<double> cum{0.0}, cum2{0.0};
    std::vector<double> cumW{0.0};               // the same for f through the wide filter (imageLines() above kWideSnrDb)
    int64_t base = 0;
    int64_t nAud = 0;                            // samples seen so far (= number of f values)

    // Pulse (phasing line) detector on a smoothed f.
    int smL = 48;
    std::vector<double> sm;
    int smI = 0;
    double smSum = 0, prevSm = 0, prevPos = 0;
    bool hi = false;
    double upX = kNaN, dnX = kNaN, riseT = kNaN;
    double dHat = 0;                             // mistuning estimate in Hz (black level - 1500)
    double dTone = 0;                            // mistuning measured on the start tone
    bool toneSeen = false;
    struct Pulse { double c, w; };
    std::deque<Pulse> pulses;
    std::vector<Pulse> chain;

    // Tone detector.
    int Nw = 1200, hop = 600;
    std::vector<float> aring;
    int aPos = 0;
    std::vector<float> hann, ctab, stab, xw;
    double hannSum = 0;
    static constexpr int kBins = 61;             // 200 .. 800 Hz in 10 Hz steps
    int toneKind = 0, toneRun = 0, toneMiss = 0, toneFired = 0, toneNone = 0;
    int64_t toneOnset = 0;
    double toneFSum = 0, toneRefF = 0;

    // Line lock and image.
    bool locked = false;
    int curLpm = 120;
    std::vector<double> fitI, fitC;
    double fitA = 0, fitP = 0;
    int phIdx = 0, failCount = 0, firstFail = 0, phasingPass = 0;
    double fbAvg = kBlackHz, fwAvg = kWhiteHz;
    bool levelsSeen = false;
    double snrAvg = 0;
    int imgW = 0;
    double imgStart = 0, Pnom = 0, Puse = 0, estPpm = 0, appliedManual = 0;
    double fbUse = kBlackHz, fwUse = kWhiteHz;
    int rowIdx = 0;

    // Joined mid-picture (no start tone, no phasing): the line period is found by correlating the picture with itself one line
    // later, and refined as the picture comes in by correlating each row with the one kRefK lines above it (the slant).
    static constexpr int kRefK = 8, kRefR = 8;
    int64_t freeFrom = 0, freeNext = 0;          // history usable for the search starts at freeFrom (after a stop tone, a reset)
    int freeHits = 0, freeLpm = 0;
    bool freeRun = false;
    int64_t refLag = 0;
    double refAcc[2 * kRefR + 1] = {};
    int refRows = 0;
    bool periodDirty = false;

    State() { configure(12000); }

    // ---- setup ----
    void configure(double rate) {
        fs = rate < 4000 ? 4000 : rate;
        // Low passes around the 1900 Hz carrier, Kaiser window for 60 dB, both of the narrow one's length so they have the same delay.
        // Narrow: passband to about 1000 Hz, stopband from 1600 Hz (1500 - 2300 Hz and the picture's sidebands, 4 dB less noise into the
        // discriminator, which matters most near its threshold: issue #103). Wide: passband to about 1400 Hz (the sharper picture of a
        // strong signal); the mirror image of the real audio after mixing lies at -3800 Hz +-800.
        const int N = static_cast<int>(std::ceil(3.62 * fs / 600.0)) | 1;
        auto design = [&](double cutHz, std::vector<double>& h) {
            const double fc = cutHz / fs;
            h.assign(N, 0.0);
            const double beta = 5.65;
            double sum = 0;
            for (int i = 0; i < N; i++) {
                const double m = i - (N - 1) / 2.0;
                const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
                const double r = 2.0 * i / (N - 1) - 1.0;
                h[i] = sinc * bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(beta);
                sum += h[i];
            }
            for (double& t : h) t /= sum;
        };
        design(1300.0, taps);
        design(2000.0, tapsW);
        gd = (N - 1) / 2.0;
        dl.assign(N, cplx(0, 0));
        rot = std::polar(1.0, -2 * kPi * kCentreHz / fs);
        alphaPow = 1.0 / (0.05 * fs);
        dcR = 1.0 - 2 * kPi * 20.0 / fs;
        smL = std::max(3, static_cast<int>(std::lround(0.004 * fs)));
        Nw = std::max(64, static_cast<int>(std::lround(0.1 * fs))) & ~1;
        hop = Nw / 2;
        aring.assign(Nw, 0.f);
        hann.resize(Nw);
        hannSum = 0;
        for (int i = 0; i < Nw; i++) {
            hann[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / Nw));
            hannSum += hann[i];
        }
        ctab.resize(static_cast<size_t>(kBins) * Nw);
        stab.resize(static_cast<size_t>(kBins) * Nw);
        for (int b = 0; b < kBins; b++) {
            const double w = 2 * kPi * (200.0 + 10.0 * b) / fs;
            for (int i = 0; i < Nw; i++) {
                ctab[static_cast<size_t>(b) * Nw + i] = static_cast<float>(std::cos(w * i));
                stab[static_cast<size_t>(b) * Nw + i] = static_cast<float>(std::sin(w * i));
            }
        }
        xw.assign(Nw, 0.f);
        ready = true;
        resetAll();
    }

    void resetAll() {
        std::fill(dl.begin(), dl.end(), cplx(0, 0));
        dlPos = 0;
        osc = cplx(1, 0);
        oscCount = 0;
        zPrev = zPrevW = cplx(0, 0);
        dcX = dcY = 0;
        avgPow = avgPowW = 0;
        prevF = prevFW = kCentreHz;
        cum.assign(1, 0.0);
        cum2.assign(1, 0.0);
        cumW.assign(1, 0.0);
        base = 0;
        nAud = 0;
        sm.assign(smL, 0.0);
        smI = 0;
        smSum = 0;
        prevSm = 0;
        prevPos = 0;
        hi = false;
        upX = dnX = riseT = kNaN;
        dHat = 0;
        dTone = 0;
        toneSeen = false;
        pulses.clear();
        chain.clear();
        std::fill(aring.begin(), aring.end(), 0.f);
        aPos = 0;
        toneKind = toneRun = toneMiss = toneFired = toneNone = 0;
        locked = false;
        fitI.clear();
        fitC.clear();
        phIdx = failCount = firstFail = phasingPass = 0;
        levelsSeen = false;
        fbAvg = kBlackHz;
        fwAvg = kWhiteHz;
        snrAvg = 0;
        rowIdx = 0;
        imgW = 0;
        freeFrom = freeNext = 0;
        freeHits = freeLpm = 0;
        freeRun = false;
        refRows = 0;
        periodDirty = false;
        const int ioc = iocSet.load() > 0 ? iocSet.load() : 576;
        st = FaxStatus();
        st.ioc = ioc;
        st.lpm = lpmSet.load() > 0 ? lpmSet.load() : 120;
        curLpm = st.lpm;
        {
            std::lock_guard<std::mutex> lk(pubMtx);
            pubImage = FaxImage();
            imgSeq++;
        }
        publishStatus();
    }

    void publishStatus() {
        std::lock_guard<std::mutex> lk(pubMtx);
        pubStatus = st;
        pubStatus.imageSeq = imgSeq;
    }

    // ---- running sums ----
    double integ(const std::vector<double>& c, double x) const {
        double y = x + 0.5;
        int64_t m = static_cast<int64_t>(std::floor(y));
        double fr = y - static_cast<double>(m);
        if (m < base) { m = base; fr = 0; }
        if (m >= nAud) return c.back();
        const double a = c[static_cast<size_t>(m - base)];
        return a + fr * (c[static_cast<size_t>(m + 1 - base)] - a);
    }
    double meanF(double a, double b) const { return (integ(cum, b) - integ(cum, a)) / (b - a); }
    double meanF2(double a, double b) const { return (integ(cum2, b) - integ(cum2, a)) / (b - a); }

    void trim() {
        const int64_t keep = static_cast<int64_t>(12.0 * fs) + 16;   // the free-running line search looks back 7 lines (7 s at 60 lpm)
        if (static_cast<int64_t>(cum.size()) <= 2 * keep) return;
        const size_t drop = cum.size() - static_cast<size_t>(keep);
        cum.erase(cum.begin(), cum.begin() + static_cast<std::ptrdiff_t>(drop));
        cum2.erase(cum2.begin(), cum2.begin() + static_cast<std::ptrdiff_t>(drop));
        cumW.erase(cumW.begin(), cumW.begin() + static_cast<std::ptrdiff_t>(drop));
        base += static_cast<int64_t>(drop);
        const double o1 = cum[0], o2 = cum2[0], o3 = cumW[0];      // keep the sums small
        for (double& v : cum) v -= o1;
        for (double& v : cum2) v -= o2;
        for (double& v : cumW) v -= o3;
    }

    // FM discriminator; holds the last value in a fade or gap instead of passing noise on.
    double discriminate(cplx y, cplx& zp, double& ap, double& pf) const {
        const double pw = std::norm(y);
        ap += (pw - ap) * alphaPow;
        const cplx d = y * std::conj(zp);
        double f;
        if (std::abs(d) <= 0.002 * ap || pw < 1e-14) f = pf;
        else f = kCentreHz + std::atan2(d.imag(), d.real()) * fs / (2 * kPi);
        zp = y;
        f = std::min(2550.0, std::max(1250.0, f));
        pf = f;
        return f;
    }

    // ---- per sample ----
    void block(const float* a, size_t n) {
        const size_t N = taps.size();
        for (size_t k = 0; k < n; k++) {
            const float xin = std::isfinite(a[k]) ? a[k] : 0.f;     // one NaN would stay in the running sums for good
            // DC blocker (about 20 Hz): radios and sound cards leave an offset that would sit on the carrier after mixing.
            const double xd = xin - dcX + dcR * dcY;
            dcX = xin;
            dcY = xd;
            const float x = static_cast<float>(xd);
            aring[aPos] = x;
            if (++aPos == Nw) aPos = 0;
            // Mix to complex baseband (rotator, renormalised now and then).
            osc *= rot;
            if (++oscCount >= 4096) { oscCount = 0; osc /= std::abs(osc); }
            dl[dlPos] = osc * static_cast<double>(x);
            // Low pass (direct form, circular buffer).
            cplx y(0, 0), yW(0, 0);
            size_t idx = dlPos;
            for (size_t i = 0; i < N; i++) {
                y += taps[i] * dl[idx];
                yW += tapsW[i] * dl[idx];
                idx = idx == 0 ? N - 1 : idx - 1;
            }
            if (++dlPos == N) dlPos = 0;
            const double f = discriminate(y, zPrev, avgPow, prevF);
            cum.push_back(cum.back() + f);
            cum2.push_back(cum2.back() + f * f);
            cumW.push_back(cumW.back() + discriminate(yW, zPrevW, avgPowW, prevFW));
            nAud++;
            edgeStep(f);
        }
        trim();
    }

    // Pulse detector: the white pulse of a phasing line seen as a rise and a fall of the smoothed f through the mid
    // level; the pulse centre is the line boundary. Crossing times are interpolated, so a clock error shows up as a drift.
    void edgeStep(double f) {
        smSum += f - sm[smI];
        sm[smI] = f;
        if (++smI == smL) smI = 0;
        if (nAud < smL) return;
        const double v = smSum / smL;
        const double pos = static_cast<double>(nAud - 1) - (smL - 1) / 2.0;
        const double mid = kCentreHz + dHat, hyst = 120.0;
        if (nAud > smL) {
            if (!hi) {
                if (v > mid && prevSm <= mid) upX = prevPos + (mid - prevSm) / (v - prevSm) * (pos - prevPos);
                if (v > mid + hyst) { hi = true; riseT = upX; }
            } else {
                if (v < mid && prevSm >= mid) dnX = prevPos + (mid - prevSm) / (v - prevSm) * (pos - prevPos);
                if (v < mid - hyst) {
                    hi = false;
                    if (!std::isnan(riseT) && !std::isnan(dnX) && dnX > riseT) onPulse(0.5 * (riseT + dnX), dnX - riseT);
                    riseT = kNaN;
                }
            }
        }
        prevSm = v;
        prevPos = pos;
    }

    bool plausibleSpacing(double sp) const {
        const int set = lpmSet.load();
        if (set > 0) {
            const double pn = fs * 60.0 / set;
            return std::fabs(sp - pn) < 0.03 * pn;
        }
        return sp > fs * 60.0 / 260.0 && sp < fs * 60.0 / 55.0;
    }

    void onPulse(double c, double w) {
        if (st.state == 2) return;
        pulses.push_back({c, w});
        while (pulses.size() > 64) pulses.pop_front();
        if (locked) return;
        // Chain of equally spaced pulses with the right width.
        bool ok = true;
        if (!chain.empty()) {
            const double sp = c - chain.back().c;
            if (chain.size() == 1) ok = plausibleSpacing(sp);
            else {
                const double ref = (chain.back().c - chain.front().c) / static_cast<double>(chain.size() - 1);
                ok = std::fabs(sp - ref) < 0.02 * ref;
            }
            if (ok) { const double fr = w / sp; ok = fr > 0.03 && fr < 0.08; }
        }
        if (ok) chain.push_back({c, w});
        else chain.assign(1, {c, w});
        if (chain.size() >= 6) tryLock();
    }

    void tryLock() {
        const double sp = (chain.back().c - chain.front().c) / static_cast<double>(chain.size() - 1);
        int lpm = lpmSet.load();
        if (lpm <= 0) {
            const double est = 60.0 * fs / sp;
            lpm = 0;
            for (int cand : kLpmChoices)
                if (std::fabs(est / cand - 1.0) < 0.03) lpm = cand;
            if (lpm == 0) { chain.clear(); return; }
        }
        curLpm = lpm;
        st.lpm = lpm;
        Pnom = fs * 60.0 / lpm;
        fitI.clear();
        fitC.clear();
        // The first pulse after a start tone is only the second half of a pulse: leave out pulses that are too narrow.
        std::vector<double> ws;
        for (const Pulse& p : chain) ws.push_back(p.w);
        std::nth_element(ws.begin(), ws.begin() + ws.size() / 2, ws.end());
        const double wMed = ws[ws.size() / 2];
        for (size_t i = 0; i < chain.size(); i++)
            if (chain[i].w > 0.8 * wMed) { fitI.push_back(static_cast<double>(i)); fitC.push_back(chain[i].c); }
        phIdx = static_cast<int>(chain.size());
        failCount = 0;
        phasingPass = static_cast<int>(chain.size());
        levelsSeen = false;
        chain.clear();
        refit();
        locked = true;
        if (st.state != 1) {
            st.state = 1;
            if (iocSet.load() > 0) st.ioc = iocSet.load();
        }
    }

    // Least squares line c = a + i * P through the phasing pulses; P stays at the nominal value when the fit is not credible.
    void refit() {
        const size_t n = fitI.size();
        double sI = 0, sC = 0, sII = 0, sIC = 0;
        for (size_t i = 0; i < n; i++) { sI += fitI[i]; sC += fitC[i]; sII += fitI[i] * fitI[i]; sIC += fitI[i] * fitC[i]; }
        double P = Pnom;
        if (n >= 3) {
            const double den = n * sII - sI * sI;
            if (den > 0) P = (n * sIC - sI * sC) / den;
            if (std::fabs(P / Pnom - 1.0) > 0.01) P = Pnom;
        }
        fitP = P;
        fitA = (sC - P * sI) / static_cast<double>(n);
        estPpm = (P / Pnom - 1.0) * 1e6;
        st.slantPpm = estPpm + manualPpm.load();
    }

    // ---- tone detector ----
    void toneBlock() {
        // Linearise the ring (oldest first) and remove the mean.
        double mean = 0;
        for (int i = 0; i < Nw; i++) {
            const float v = aring[(aPos + i) % Nw];
            xw[i] = v;
            mean += v;
        }
        mean /= Nw;
        double pwr = 0;
        for (int i = 0; i < Nw; i++) {
            const float v = xw[i] - static_cast<float>(mean);
            pwr += static_cast<double>(v) * v;
            xw[i] = v * hann[i];
        }
        pwr /= Nw;
        double mag[kBins];
        int bp = 0;
        for (int b = 0; b < kBins; b++) {
            const float* c = &ctab[static_cast<size_t>(b) * Nw];
            const float* s = &stab[static_cast<size_t>(b) * Nw];
            float re = 0, im = 0;
            for (int i = 0; i < Nw; i++) { re += xw[i] * c[i]; im += xw[i] * s[i]; }
            mag[b] = std::sqrt(static_cast<double>(re) * re + static_cast<double>(im) * im);
            if (mag[b] > mag[bp]) bp = b;
        }
        int kind = 0;
        double fTone = 0, r = 0;
        if (pwr > 1e-12 && mag[bp] > 0) {
            double delta = 0;
            if (bp > 0 && bp < kBins - 1 && mag[bp - 1] > 0 && mag[bp + 1] > 0) {
                const double l = std::log(mag[bp - 1]), c0 = std::log(mag[bp]), rr = std::log(mag[bp + 1]);
                const double den = l - 2 * c0 + rr;
                if (den < 0) delta = std::min(0.5, std::max(-0.5, 0.5 * (l - rr) / den));
            }
            fTone = 200.0 + 10.0 * (bp + delta);
            // Hann window scalloping at a fractional bin offset: sinc(d) / (1 - d^2).
            const double dd = std::fabs(delta);
            const double g = dd < 1e-9 ? 1.0 : (std::sin(kPi * dd) / (kPi * dd)) / (1 - dd * dd);
            const double amp = 2.0 * mag[bp] / (hannSum * g);
            r = std::min(0.999, 0.5 * amp * amp / pwr);
            if (r >= 0.35) {
                if (std::fabs(fTone - 300) <= 55) kind = 1;
                else if (std::fabs(fTone - 675) <= 55) kind = 2;
                else if (std::fabs(fTone - 450) <= 55) kind = 3;
            }
        }
        if (kind) {
            st.toneHz = fTone;
            st.snrDb = 10 * std::log10(r / (1.001 - r));
        }
        const int64_t winStart = nAud - Nw;
        if (kind != 0 && kind == toneKind && std::fabs(fTone - toneRefF) < 8.0) {
            toneRun++;
            toneMiss = 0;
            toneFSum += fTone;
        } else if (kind != 0) {
            toneKind = kind;
            toneRun = 1;
            toneMiss = 0;
            toneOnset = winStart;
            toneFSum = fTone;
            toneRefF = fTone;
            toneNone = 0;
        } else if (toneRun > 0 && toneMiss < 1) {
            toneMiss++;
        } else {
            toneRun = 0;
            toneKind = 0;
            toneMiss = 0;
            toneNone++;
            if (toneNone >= 3) toneFired = 0;
        }
        if (toneKind != 0 && toneFired != toneKind) {
            const int need = toneKind == 3 ? 8 : 14;
            if (toneRun >= need) {
                toneFired = toneKind;
                const double fMean = toneFSum / toneRun;
                if (toneKind == 3) onStopTone(toneOnset);
                else onStartTone(toneKind, toneOnset, fMean - (toneKind == 1 ? 300.0 : 675.0));
            }
        }
    }

    // ---- state machine events ----
    void onStartTone(int kind, int64_t onset, double dMeasured) {
        if (st.state == 2) truncateRows(onset);
        st.state = 1;
        locked = false;
        freeFrom = nAud;
        chain.clear();
        pulses.clear();
        fitI.clear();
        fitC.clear();
        phIdx = failCount = phasingPass = 0;
        levelsSeen = false;
        fbAvg = kBlackHz;
        fwAvg = kWhiteHz;
        dTone = std::min(80.0, std::max(-80.0, dMeasured));
        toneSeen = true;
        dHat = dTone;
        freeRun = false;
        freeHits = 0;
        const int ioc = iocSet.load() > 0 ? iocSet.load() : (kind == 1 ? 576 : 288);
        st.ioc = ioc;
        st.lpm = lpmSet.load() > 0 ? lpmSet.load() : 120;
        st.phasingLines = 0;
    }

    void onStopTone(int64_t onset) {
        if (st.state == 2) truncateRows(onset);
        st.state = 3;
        locked = false;
        chain.clear();
        pulses.clear();
        freeRun = false;
        freeHits = 0;
        freeFrom = nAud;                         // the picture before the stop tone is not the next one
    }

    // A tone (300, 450 or 675 Hz) lies far below the black level, so the discriminator sits at its lower limit for the
    // whole tone. Rows of the last few that spend a good part of their length there are the tone, not the picture.
    void truncateRows(int64_t) {
        int keep = rowIdx;
        const int from = std::max(0, rowIdx - 6);
        for (int r = from; r < rowIdx; r++) {
            const double s = imgStart + r * Puse;
            int low = 0;
            for (int k = 0; k < 20; k++) {
                const double a = s + k * Puse / 20.0;
                if (meanF(a, a + Puse / 20.0) < fbUse - 150.0) low++;
            }
            if (low >= 3) { keep = r; break; }
        }
        if (keep < rowIdx) {
            std::lock_guard<std::mutex> lk(pubMtx);
            pubImage.pix.resize(static_cast<size_t>(keep) * imgW);
            pubImage.height = keep;
            imgSeq++;
            rowIdx = keep;
        }
        st.lines = rowIdx;
    }

    // ---- lines ----
    void lineStep() {
        if (st.state == 2) imageLines();
        else if (st.state == 1 && locked) phasingLines();
        if (st.state == 3 && rowIdx > 0 && imgW > 0) applyManual();   // the slant slider also straightens a finished picture
        if ((st.state == 0 || st.state == 3) && !locked && nAud >= freeNext) freeSearch();
        st.width = imgW;
        st.blackHz = fbAvg;
    }

    void phasingLines() {
        for (;;) {
            const double P = fitP;
            const double b = fitA + phIdx * P;
            if (static_cast<double>(nAud) < b + 0.94 * P + 4) break;
            const double mid = kCentreHz + dHat;
            const double wMean = meanF(b - 0.015 * P, b + 0.015 * P);
            const double bMean = meanF(b + 0.06 * P, b + 0.94 * P);
            // Contrast, not absolute levels: noise pulls the black level up long before the pulse stops standing out.
            // The pulse is 5 % wide: just outside it (3.5 .. 6 % either side of the boundary) the line is black again.
            const double halfLevel = 0.5 * (wMean + bMean);
            const bool narrow = meanF(b - 0.06 * P, b - 0.035 * P) < halfLevel && meanF(b + 0.035 * P, b + 0.06 * P) < halfLevel;
            const bool pass = wMean - bMean > 300 && bMean < mid && narrow;
            if (pass) {
                failCount = 0;
                phasingPass++;
                st.phasingLines = phasingPass;
                if (!levelsSeen) { fbAvg = bMean; fwAvg = wMean; levelsSeen = true; }
                else { fbAvg += 0.15 * (bMean - fbAvg); fwAvg += 0.15 * (wMean - fwAvg); }
                dHat = std::min(100.0, std::max(-100.0, fbAvg - kBlackHz));
                const double m2 = meanF2(b + 0.06 * P, b + 0.94 * P);
                const double sigma = std::sqrt(std::max(1e-6, m2 - bMean * bMean));
                const double snr = std::min(60.0, 20 * std::log10(400.0 / sigma));
                snrAvg = snrAvg == 0 ? snr : snrAvg + 0.1 * (snr - snrAvg);
                st.snrDb = snrAvg;
                // The measured pulse for this line, if the detector saw it near the expected place.
                for (const Pulse& p : pulses) {
                    if (std::fabs(p.c - b) < 0.02 * P && p.w / P > 0.03 && p.w / P < 0.08) {
                        fitI.push_back(phIdx);
                        fitC.push_back(p.c);
                        refit();
                        break;
                    }
                }
            } else {
                if (failCount == 0) firstFail = phIdx;
                failCount++;
            }
            phIdx++;
            while (!pulses.empty() && pulses.front().c < b - 0.02 * P) pulses.pop_front();
            if (failCount >= 3) { startImage(firstFail); return; }   // three in a row: a fade of a line or two does not end the phasing
        }
    }

    void startImage(int kImg) {
        imgW = faxImageWidth(st.ioc);
        if (imgW <= 0) imgW = faxImageWidth(576);
        const double m = manualPpm.load();
        double a;
        if (autoSlant.load()) {
            Puse = fitP * (1.0 + m * 1e-6);
            a = fitA;
            estPpm = (fitP / Pnom - 1.0) * 1e6;
            imgStart = a + kImg * fitP;
        } else {
            Puse = Pnom * (1.0 + m * 1e-6);
            double s = 0;
            for (size_t i = 0; i < fitI.size(); i++) s += fitC[i] - fitI[i] * Puse;
            a = s / static_cast<double>(fitI.size());
            estPpm = 0;
            imgStart = a + kImg * Puse;
        }
        appliedManual = m;
        fbUse = fbAvg;
        fwUse = (fwAvg - fbAvg > 700 && fwAvg - fbAvg < 900) ? fwAvg : fbAvg + (kWhiteHz - kBlackHz);
        if (snrAvg < 16.0) {
            // Noise pulls the measured levels towards each other and the black level up: trust the nominal ones, moved by
            // the mistuning measured on the start tone when there was one.
            // Without a start tone the black of the phasing lines still tells the mistuning (its mean over 88 % of each line is
            // pulled up only a few Hz at 15 dB, where the white pulse is no longer measured well).
            fbUse = kBlackHz + (toneSeen ? dTone : levelsSeen && std::fabs(fbAvg - kBlackHz) <= 130 ? fbAvg - kBlackHz : 0.0);
            fwUse = fbUse + (kWhiteHz - kBlackHz);
        }
        rowIdx = 0;
        freeRun = false;
        st.state = 2;
        st.lines = 0;
        st.slantPpm = (Puse / Pnom - 1.0) * 1e6;
        {
            std::lock_guard<std::mutex> lk(pubMtx);
            pubImage.width = imgW;
            pubImage.height = 0;
            pubImage.pix.clear();
            imgSeq++;
        }
        pulses.clear();
        imageLines();
    }

    // The manual slant changed: move the rows already received (row r shifts by r * dP / samples-per-pixel).
    void applyManual() {
        const double m = manualPpm.load();
        if (std::fabs(m - appliedManual) < 1e-9 && !periodDirty) return;
        appliedManual = m;
        periodDirty = false;
        double ppm = (autoSlant.load() ? estPpm : 0.0) + m;
        const double newP = Pnom * (1.0 + ppm * 1e-6);
        const double dP = newP - Puse;
        const double spp = Puse / imgW;
        Puse = newP;
        st.slantPpm = ppm;
        if (rowIdx < 2 || imgW <= 0) return;
        std::lock_guard<std::mutex> lk(pubMtx);
        std::vector<uint8_t> tmp(imgW);
        for (int r = 1; r < rowIdx; r++) {
            uint8_t* row = &pubImage.pix[static_cast<size_t>(r) * imgW];
            const double sh = r * dP / spp;
            for (int j = 0; j < imgW; j++) {
                double x = std::fmod(j + sh, static_cast<double>(imgW));
                if (x < 0) x += imgW;
                const int i0 = static_cast<int>(x);
                const double fr = x - i0;
                const int i1 = i0 + 1 == imgW ? 0 : i0 + 1;
                tmp[j] = static_cast<uint8_t>(std::lround(row[i0] * (1 - fr) + row[i1] * fr));
            }
            std::memcpy(row, tmp.data(), imgW);
        }
        imgSeq++;
    }

    void imageLines() {
        applyManual();
        const int W = imgW;
        const int cap = maxLines.load();
        std::vector<uint8_t> row(W);
        bool any = false;
        while (static_cast<double>(nAud) >= imgStart + (rowIdx + 1) * Puse + 4) {
            if (rowIdx >= cap) { st.state = 3; break; }
            const double s = imgStart + rowIdx * Puse;
            const double spp = Puse / W;
            // the picture: through the narrow filter while the signal is weak (or not measured), the wide one when it is strong
            const std::vector<double>& cp = snrAvg >= kWideSnrDb ? cumW : cum;
            double prev = integ(cp, s);
            const double span = fwUse - fbUse;
            for (int j = 0; j < W; j++) {
                const double nxt = integ(cp, s + (j + 1) * spp);
                const double f = (nxt - prev) / spp;
                prev = nxt;
                const double v = (f - fbUse) / span;
                row[j] = static_cast<uint8_t>(std::lround(255.0 * std::min(1.0, std::max(0.0, v))));
            }
            {
                std::lock_guard<std::mutex> lk(pubMtx);
                pubImage.pix.insert(pubImage.pix.end(), row.begin(), row.end());
                pubImage.height = rowIdx + 1;
                imgSeq++;
            }
            rowIdx++;
            any = true;
            if (freeRun) refineRow(s);
        }
        if (any) st.lines = rowIdx;
    }

    // f smoothed over smD samples (the discriminator noise is mostly at the top of its band): n .. n + smD - 1 must be in the history
    int smD = 8;
    double fAt(int64_t n) const { return (cum[static_cast<size_t>(n + smD - base)] - cum[static_cast<size_t>(n - base)]) / smD; }

    // Normalised correlation of f over [a, a + len) with f over [a + lag, a + lag + len).
    double corrAt(int64_t a, int64_t len, int64_t lag) const {
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        for (int64_t n = a; n < a + len; n++) {
            const double x = fAt(n), y = fAt(n + lag);
            sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
        }
        const double N = static_cast<double>(len);
        const double vx = sxx - sx * sx / N, vy = syy - sy * sy / N;
        if (vx <= 0 || vy <= 0) return 0;
        return (sxy - sx * sy / N) / std::sqrt(vx * vy);
    }

    // No start tone and no phasing: a picture is a signal that repeats almost exactly one line later. Look for that at each
    // line rate; the shortest period that correlates well is the line (a 120 lpm picture also matches two lines later at 60 lpm).
    void freeSearch() {
        freeNext = nAud + static_cast<int64_t>(fs * 0.5);
        if (chain.size() >= 3) { freeHits = 0; return; }     // a phasing signal is coming in: the pulse chain locks on it
        const int set = lpmSet.load();
        const int64_t from = std::max(freeFrom, base);
        constexpr int kLines = 6;
        struct Cand { int lpm; double r, P; };
        std::vector<Cand> cands;
        double rMax = 0;
        for (int lpm : {240, 120, 90, 60}) {
            if (set > 0 && lpm != set) continue;
            const double Pn = fs * 60.0 / lpm;
            const int64_t len = static_cast<int64_t>(kLines * Pn);
            const int64_t L0 = static_cast<int64_t>(std::lround(Pn));
            const int64_t R = static_cast<int64_t>(std::ceil(Pn * 0.003)) + 1;
            const int64_t a = nAud - len - L0 - R - 1 - smD;
            if (a < from) continue;
            // the picture must have some contrast: a steady carrier, black or a tone is not one
            const double m = meanF(static_cast<double>(a), static_cast<double>(a + len));
            const double sd = std::sqrt(std::max(0.0, meanF2(static_cast<double>(a), static_cast<double>(a + len)) - m * m));
            if (sd < 60) continue;
            // and lie between black and white: noise, FSK (DSC, NAVTEX dot patterns repeat at every lag) and tones mostly do not
            int inBand = 0, total = 0;
            for (int64_t n = a; n + smD <= a + len; n += smD, total++) { const double f = fAt(n); inBand += f > 1350 && f < 2450; }
            if (inBand < 0.6 * total) continue;
            std::vector<double> rr(static_cast<size_t>(2 * R + 1));
            int64_t bi = -R;
            for (int64_t d = -R; d <= R; d++) {
                rr[static_cast<size_t>(d + R)] = corrAt(a, len, L0 + d);
                if (rr[static_cast<size_t>(d + R)] > rr[static_cast<size_t>(bi + R)]) bi = d;
            }
            if (bi <= -R || bi >= R) continue;      // the peak at the edge of the search: not this line rate
            const double l = rr[static_cast<size_t>(bi + R - 1)], c = rr[static_cast<size_t>(bi + R)], h = rr[static_cast<size_t>(bi + R + 1)];
            double P = static_cast<double>(L0 + bi);
            const double den = l - 2 * c + h;
            if (den < 0) P += std::min(0.5, std::max(-0.5, 0.5 * (l - h) / den));
            const double rOff = corrAt(a, len, static_cast<int64_t>(std::lround(0.37 * Pn)));   // no line structure there
            if (c - rOff < 0.15) continue;
            cands.push_back({lpm, c, P});
            rMax = std::max(rMax, c);
        }
        int bestLpm = 0;
        double bestP = 0;
        for (const Cand& c : cands)                 // highest line rate first
            if (c.r >= 0.3 && c.r >= 0.85 * rMax) { bestLpm = c.lpm; bestP = c.P; break; }
        if (bestLpm == 0) { freeHits = 0; freeLpm = 0; return; }
        if (bestLpm != freeLpm) { freeLpm = bestLpm; freeHits = 0; }
        if (++freeHits < 2) return;                 // seen twice, half a second apart
        startFree(bestLpm, bestP, std::max(from, nAud - static_cast<int64_t>((kLines + 1) * bestP)));
    }

    void startFree(int lpm, double P0, int64_t histStart) {
        curLpm = lpm;
        st.lpm = lpm;
        Pnom = fs * 60.0 / lpm;
        if (iocSet.load() > 0) st.ioc = iocSet.load();
        imgW = faxImageWidth(st.ioc);
        if (imgW <= 0) imgW = faxImageWidth(576);
        fitP = P0;
        fitA = static_cast<double>(histStart);
        const double m = manualPpm.load();
        estPpm = autoSlant.load() ? (P0 / Pnom - 1.0) * 1e6 : 0.0;
        Puse = Pnom * (1.0 + (estPpm + m) * 1e-6);
        appliedManual = m;
        // Levels: a chart is mostly white background, so the commonest frequency is the white level; it also gives the mistuning.
        std::vector<int> hist(131, 0);
        const int64_t step = 4;
        for (int64_t n = histStart; n + step < nAud; n += step) {
            const double f = meanF(static_cast<double>(n), static_cast<double>(n + step));
            hist[static_cast<size_t>(std::min(130, std::max(0, static_cast<int>((f - 1250.0) / 10.0))))]++;
        }
        int bk = 0;
        for (int k = 1; k < 131; k++) if (hist[static_cast<size_t>(k)] > hist[static_cast<size_t>(bk)]) bk = k;
        const double mode = 1250.0 + 10.0 * bk + 5.0;
        const double d = std::fabs(mode - kWhiteHz) <= 130 ? mode - kWhiteHz : 0.0;
        dHat = d;
        fbUse = kBlackHz + d;
        fwUse = kWhiteHz + d;
        fbAvg = fbUse;
        imgStart = static_cast<double>(histStart);
        rowIdx = 0;
        st.state = 2;
        st.lines = 0;
        st.phasingLines = 0;
        st.slantPpm = estPpm + m;
        freeRun = true;
        refLag = static_cast<int64_t>(std::lround(kRefK * P0));
        std::fill(std::begin(refAcc), std::end(refAcc), 0.0);
        refRows = 0;
        {
            std::lock_guard<std::mutex> lk(pubMtx);
            pubImage.width = imgW;
            pubImage.height = 0;
            pubImage.pix.clear();
            imgSeq++;
        }
        pulses.clear();
        chain.clear();
        imageLines();
    }

    // Free-running picture: where does a line start? A chart is framed: a white margin with a dark frame line on either side runs
    // down the whole picture at the line boundary. Look for the steady white band flanked by dark columns in the rows so far and
    // roll the picture (and the line start) to put it at the edges. Nothing moves when no such band stands out.
    void alignLines() {
        const int W = imgW, n = std::min(rowIdx, 96);
        if (W < 64 || n < 16) return;
        std::vector<double> mean(W, 0.0), sd(W, 0.0);
        {
            std::lock_guard<std::mutex> lk(pubMtx);
            for (int r = rowIdx - n; r < rowIdx; r++) {
                const uint8_t* row = &pubImage.pix[static_cast<size_t>(r) * W];
                for (int x = 0; x < W; x++) { mean[x] += row[x]; sd[x] += static_cast<double>(row[x]) * row[x]; }
            }
        }
        for (int x = 0; x < W; x++) { mean[x] /= n; sd[x] = std::sqrt(std::max(0.0, sd[x] / n - mean[x] * mean[x])); }
        const int w = std::max(4, W / 113);            // 16 px of 1809: about 1 % of the line
        auto at = [&](const std::vector<double>& v, int x) { x %= W; if (x < 0) x += W; return v[static_cast<size_t>(x)]; };
        double best = -1e9, bestB = 0, bestS = 0;
        int bc = -1;
        for (int c = 0; c < W; c++) {
            double B = 0, S = 0, L = 1e9, R = 1e9;
            for (int k = -w / 2; k < w / 2; k++) { B += at(mean, c + k); S += at(sd, c + k); }
            B /= w; S /= w;
            for (int k = w / 2; k < w; k++) { L = std::min(L, at(mean, c - k - 1)); R = std::min(R, at(mean, c + k)); }
            const double score = B - 0.5 * (L + R) - S;
            if (score > best) { best = score; bc = c; bestB = B; bestS = S; }
        }
        if (bc <= 2 || bc >= W - 2 || best < 60 || bestB < 170 || bestS > 45) return;   // in place already (a pixel either way is noise)
        // roll every row left by bc pixels; later rows start bc pixels later
        {
            std::lock_guard<std::mutex> lk(pubMtx);
            std::vector<uint8_t> tmp(W);
            for (int r = 0; r < rowIdx; r++) {
                uint8_t* row = &pubImage.pix[static_cast<size_t>(r) * W];
                for (int x = 0; x < W; x++) tmp[x] = row[(x + bc) % W];
                std::memcpy(row, tmp.data(), W);
            }
            imgSeq++;
        }
        imgStart += bc * (Puse / W);
    }

    // Free-running picture: correlate the row just decoded with the one kRefK rows above; the lag of the peak, summed over every
    // row so far, is kRefK line periods. The picture is straightened with each new estimate (the rows already received too).
    void refineRow(double s) {
        if (!autoSlant.load()) return;
        const int64_t s0 = static_cast<int64_t>(std::floor(s));
        const int64_t len = static_cast<int64_t>(Pnom);
        if (s0 - refLag - kRefR < base || s0 + len + smD > nAud) return;
        const double mr = meanF(static_cast<double>(s0), static_cast<double>(s0 + len));
        for (int d = -kRefR; d <= kRefR; d++) {
            double acc = 0;
            const int64_t o = refLag + d;
            for (int64_t n = s0; n < s0 + len; n++) acc += (fAt(n) - mr) * (fAt(n - o) - mr);
            refAcc[d + kRefR] += acc;
        }
        ++refRows;
        if (refRows == 24 || refRows == 96) alignLines();
        if (refRows % 4 != 0) return;
        int bi = -kRefR;
        for (int d = -kRefR; d <= kRefR; d++) if (refAcc[d + kRefR] > refAcc[bi + kRefR]) bi = d;
        if (bi <= -kRefR || bi >= kRefR) return;
        const double l = refAcc[bi + kRefR - 1], c = refAcc[bi + kRefR], h = refAcc[bi + kRefR + 1];
        const double den = l - 2 * c + h;
        if (!(den < 0)) return;
        const double lag = static_cast<double>(refLag + bi) + std::min(0.5, std::max(-0.5, 0.5 * (l - h) / den));
        const double P = lag / kRefK;
        if (std::fabs(P / Pnom - 1.0) > 0.003) return;
        estPpm = (P / Pnom - 1.0) * 1e6;
        st.slantPpm = estPpm + manualPpm.load();
        periodDirty = true;
    }

};

FaxDecoder::FaxDecoder() : s_(std::make_unique<State>()) {}
FaxDecoder::~FaxDecoder() = default;

void FaxDecoder::configure(double audioRate) {
    std::lock_guard<std::mutex> lk(s_->procMtx);
    s_->configure(audioRate);
}
void FaxDecoder::setLpm(int lpm) { s_->lpmSet = lpm; }
void FaxDecoder::setIoc(int ioc) { s_->iocSet = ioc; }
void FaxDecoder::setSlantPpm(double ppm) { s_->manualPpm = std::min(2000.0, std::max(-2000.0, ppm)); }
void FaxDecoder::setAutoSlant(bool on) { s_->autoSlant = on; }
void FaxDecoder::setMaxLines(int n) { s_->maxLines = std::max(1, n); }

void FaxDecoder::reset() {
    std::lock_guard<std::mutex> lk(s_->procMtx);
    s_->resetAll();
}

void FaxDecoder::push(const float* audio, size_t n) {
    State& S = *s_;
    std::lock_guard<std::mutex> lk(S.procMtx);
    if (!S.ready) return;
    size_t i = 0;
    while (i < n) {
        // Sub-blocks end on the tone detector's hop boundaries, so the result does not depend on the chunk size.
        const size_t toGo = static_cast<size_t>(S.hop - S.nAud % S.hop);
        const size_t take = std::min(n - i, toGo);
        S.block(audio + i, take);
        i += take;
        if (S.nAud % S.hop == 0 && S.nAud >= S.Nw) S.toneBlock();
        S.lineStep();
    }
    S.publishStatus();
}

FaxStatus FaxDecoder::status() const {
    std::lock_guard<std::mutex> lk(s_->pubMtx);
    return s_->pubStatus;
}

bool FaxDecoder::latestImage(FaxImage& out, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(s_->pubMtx);
    if (s_->imgSeq == seq) return false;
    out = s_->pubImage;
    seq = s_->imgSeq;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------
// Test chart
// ------------------------------------------------------------------------------------------------------------------

namespace {

struct Glyph { char c; uint8_t r[7]; };
// 5 x 7 capital letters, digits and a few signs; rows top to bottom, bit 4 is the left column.
const Glyph kFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {':', {0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
};

void drawChar(FaxImage& im, int x0, int y0, char c, int scale, uint8_t v) {
    for (const Glyph& g : kFont) {
        if (g.c != c) continue;
        for (int ry = 0; ry < 7; ry++)
            for (int rx = 0; rx < 5; rx++) {
                if (!((g.r[ry] >> (4 - rx)) & 1)) continue;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) {
                        const int x = x0 + rx * scale + dx, y = y0 + ry * scale + dy;
                        if (x >= 0 && x < im.width && y >= 0 && y < im.height) im.pix[static_cast<size_t>(y) * im.width + x] = v;
                    }
            }
        return;
    }
}

void drawText(FaxImage& im, int x, int y, const char* s, int scale, uint8_t v) {
    for (; *s; s++, x += 6 * scale) drawChar(im, x, y, *s, scale, v);
}

void fillRect(FaxImage& im, int x0, int y0, int x1, int y1, uint8_t v) {
    for (int y = std::max(0, y0); y < std::min(im.height, y1); y++)
        for (int x = std::max(0, x0); x < std::min(im.width, x1); x++) im.pix[static_cast<size_t>(y) * im.width + x] = v;
}

void frameRect(FaxImage& im, int x0, int y0, int x1, int y1, int t, uint8_t v) {
    fillRect(im, x0, y0, x1, y0 + t, v);
    fillRect(im, x0, y1 - t, x1, y1, v);
    fillRect(im, x0, y0, x0 + t, y1, v);
    fillRect(im, x1 - t, y0, x1, y1, v);
}

} // namespace

FaxImage faxTestChart(int W, int H, uint32_t seed) {
    FaxImage im;
    im.width = W;
    im.height = H;
    if (W <= 0 || H <= 0) { im.width = im.height = 0; return im; }
    im.pix.assign(static_cast<size_t>(W) * H, 255);
    uint32_t rs = seed * 2654435761u + 12345u;
    auto rnd = [&] { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.0; };
    const double ph1 = rnd() * 6.28, ph2 = rnd() * 6.28;
    // Pressure field: a low and a high, isobars every 4 hPa.
    const double lx = 0.30 * W, ly = 0.38 * H, hx = 0.74 * W, hy = 0.72 * H;
    const double sig = 0.20 * W;
    auto pressure = [&](double x, double y) {
        const double d1 = (x - lx) * (x - lx) + (y - ly) * (y - ly);
        const double d2 = (x - hx) * (x - hx) + (y - hy) * (y - hy);
        return 1012.0 - 26.0 * std::exp(-d1 / (2 * sig * sig)) + 18.0 * std::exp(-d2 / (2 * sig * sig))
               + 2.0 * std::sin(x * 0.004 + ph1) * std::cos(y * 0.005 + ph2);
    };
    auto coast = [&](double y) {
        return W * (0.64 + 0.08 * std::sin(y * 0.011 + ph1) + 0.04 * std::sin(y * 0.029 + ph2) + 0.02 * std::sin(y * 0.063));
    };
    // Land, grid, isobars.
    std::vector<int> lev0(W + 2), lev1(W + 2);
    auto levels = [&](int y, std::vector<int>& out) {
        for (int x = 0; x < W + 2; x++) out[x] = static_cast<int>(std::floor((pressure(x - 1, y) - 1012.0) / 4.0));
    };
    levels(0, lev1);
    const int gx = std::max(8, W / 10), gy = gx;
    for (int y = 0; y < H; y++) {
        lev0.swap(lev1);
        levels(y + 1, lev1);
        const double xc = coast(y);
        for (int x = 0; x < W; x++) {
            uint8_t v = 255;
            if (x > xc) v = 224;
            if ((x % gx) == 0 || (y % gy) == 0) v = 170;
            if (std::fabs(x - xc) < 2.0) v = 20;
            const int l = lev0[x + 1];
            if (l != lev0[x] || l != lev0[x + 2] || l != lev1[x + 1]) v = 40;
            im.pix[static_cast<size_t>(y) * W + x] = v;
        }
    }
    // H and L at the centres.
    drawChar(im, static_cast<int>(lx) - 15, static_cast<int>(ly) - 20, 'L', 6, 0);
    drawChar(im, static_cast<int>(hx) - 15, static_cast<int>(hy) - 20, 'H', 6, 0);
    // Frame, with a white margin around the picture (the first rows and columns stay white).
    fillRect(im, 0, 0, W, 8, 255);
    fillRect(im, 0, 0, 8, H, 255);
    fillRect(im, W - 8, 0, W, H, 255);
    fillRect(im, 0, H - 8, W, H, 255);
    frameRect(im, 8, 8, W - 8, H - 8, 3, 0);
    // Title block.
    if (W >= 420 && H >= 110) {
        fillRect(im, 24, 24, 24 + 340, 24 + 76, 255);
        frameRect(im, 24, 24, 24 + 340, 24 + 76, 3, 0);
        drawText(im, 36, 34, "SURFACE ANALYSIS", 3, 0);
        drawText(im, 36, 66, "TEST CHART 12Z 14 OCT", 2, 0);
    }
    // Grey wedge: eight flat steps from black to white, to check the grey scale.
    if (W >= 480 && H >= 200) {
        const int y0 = H - 70, y1 = H - 30;
        frameRect(im, 22, y0 - 2, 22 + 8 * 50 + 4, y1 + 2, 2, 0);
        for (int k = 0; k < 8; k++) fillRect(im, 24 + k * 50, y0, 24 + (k + 1) * 50, y1, static_cast<uint8_t>(std::lround(255.0 * k / 7)));
    }
    return im;
}

// ------------------------------------------------------------------------------------------------------------------
// Test transmission
// ------------------------------------------------------------------------------------------------------------------

struct FaxAudioSource::State {
    double fs;
    int ioc, lpm, lines;
    uint32_t seed;
    int W;
    FaxImage chart;
    double tStart = 5, tPhase = 30, tStop = 5, tBlack = 10;
    double secLen[6] = {0, 0, 0, 0, 0, 0};
    double spl = 0;
    int sec = 0;
    double pos = 0, phase = 0;
    bool planned = false;

    void plan() {
        spl = fs * 60.0 / lpm;
        const int phaseLines = std::max(1, static_cast<int>(std::lround(tPhase * lpm / 60.0)));
        secLen[0] = tStart * fs;
        secLen[1] = phaseLines * spl;
        secLen[2] = spl;                         // one white line between phasing and picture
        secLen[3] = lines * spl;
        secLen[4] = tStop * fs;
        secLen[5] = tBlack * fs;
        planned = true;
    }

    double freqNow() const {
        switch (sec) {
        case 0: return ioc == 288 ? 675.0 : 300.0;
        case 1: {
            const double lp = pos / spl;
            const double fr = lp - std::floor(lp);
            return (fr < 0.025 || fr >= 0.975) ? kWhiteHz : kBlackHz;
        }
        case 2: return kWhiteHz;
        case 3: {
            const double lp = pos / spl;
            const int row = std::min(lines - 1, static_cast<int>(lp));
            const double fr = lp - std::floor(lp);
            const int col = std::min(W - 1, static_cast<int>(fr * W));
            return kBlackHz + (kWhiteHz - kBlackHz) * chart.pix[static_cast<size_t>(row) * W + col] / 255.0;
        }
        case 4: return 450.0;
        default: return kBlackHz;
        }
    }
};

FaxAudioSource::FaxAudioSource(double audioRate, int ioc, int lpm, int lines, uint32_t seed) : s_(std::make_unique<State>()) {
    s_->fs = audioRate;
    s_->ioc = ioc == 288 ? 288 : 576;
    s_->lpm = lpm > 0 ? lpm : 120;
    s_->lines = std::max(0, lines);
    s_->seed = seed;
    s_->W = faxImageWidth(s_->ioc);
    s_->chart = faxTestChart(s_->W, std::max(1, s_->lines), seed);
}
FaxAudioSource::~FaxAudioSource() = default;

void FaxAudioSource::setTiming(double startToneS, double phasingS, double stopToneS, double blackS) {
    s_->tStart = std::max(0.0, startToneS);
    s_->tPhase = std::max(0.0, phasingS);
    s_->tStop = std::max(0.0, stopToneS);
    s_->tBlack = std::max(0.0, blackS);
    s_->planned = false;
}

void FaxAudioSource::generate(float* audio, size_t n) {
    State& S = *s_;
    if (!S.planned) S.plan();
    const double w = 2 * kPi / S.fs;
    for (size_t i = 0; i < n; i++) {
        int guard = 0;
        while (S.pos >= S.secLen[S.sec] && guard++ < 7) {
            S.pos -= S.secLen[S.sec];
            S.sec = (S.sec + 1) % 6;
        }
        if (guard >= 7) S.pos = 0;               // every section empty: stay on the black signal
        S.phase += w * S.freqNow();
        if (S.phase > 2 * kPi) S.phase -= 2 * kPi;
        audio[i] = static_cast<float>(0.5 * std::sin(S.phase));
        S.pos += 1.0;
    }
}

} // namespace dect2
