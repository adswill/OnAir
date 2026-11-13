// e-Czas receiver for the engine (see eczas_live.h).
#include "dect2/eczas/eczas_live.h"
#include "dect2/eczas/eczas_rx.h"
#include "dect2/eczas/eczas_tel.h"
#include "dect2/common/mathconst.h"
#include "eczas_taps.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace dect2 {

namespace {
using dect2::math::kPi;

constexpr double kFs = 8000.0;                 // the working rate after the first decimation
constexpr int kDecim = 16;                     // 8 kHz to the 500 Hz of the bit slicer
constexpr int kBitSamples = 160;               // one bit, 20 ms
constexpr double kRampRad = 36.0 * kPi / 180;  // the phase step of a bit that changes the keying state
constexpr double kLpHz = 45;                   // channel filter, each side of the carrier (4th order): narrower than the 19 ms ramps need, but the phase is
                                               // compared over a whole bit, and 45 Hz gives the most packets on the test signal and on real recordings
constexpr double kWideHz = 200;                // the wide filter of the coarse frequency search: a carrier up to about +-180 Hz off is found
constexpr double kBandwidthRef = 100;          // the SNR is carrier power over noise in 100 Hz
constexpr double kSymbolScale = 16384.0;       // the bit slicer expects +-16384 for a full phase step
constexpr double kPathC = 299792.458 * 0.9997; // km/s, a ground wave is a little slower than light in vacuum
constexpr double kTxLat = 52.9167, kTxLon = 18.2167;   // the transmitter: Solec Kujawski (RCN Solec Kujawski, 225 kHz)
// Where the received time stamp T belongs: the start of the frame (its first sync bit). The GUM description (v1.0) says the receiver enters the time 0.5 s
// after the start of the frame without saying which instant the number itself stands for, so this is an assumption; no real signal with a known clock yet.
constexpr double kStampAfterFrameStartSec = 0.0;
// The streaming front end's own delay between a phase step on air and the symbol peak it makes at the bit slicer; measured with the test signal
// (tests/eczas/test_eczas_live.cpp) and checked against the sample timing of the generator.
constexpr double kFrontEndDelaySec = 0.0094;

double wallNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void lowpass(double fc, double fs, double q) {
        const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
};
struct BiState { double z1 = 0, z2 = 0; };
inline double biRun(const Biquad& b, BiState& s, double x) {
    const double y = b.b0 * x + s.z1;
    s.z1 = b.b1 * x - b.a1 * y + s.z2;
    s.z2 = b.b2 * x - b.a2 * y;
    return y;
}

double haversineKm(double la1, double lo1, double la2, double lo2) {
    const double r = kPi / 180, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
}
} // namespace

struct EczasLiveReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0;
    std::atomic<double> offsetReq{0}, audioTone{1000}, rxLat{0}, rxLon{0}, clockOrigin{0};
    std::atomic<bool> audioIn{false}, rxLocSet{false};
    std::atomic<int> clockMode{0};
    std::atomic<bool> resetReq{true}, clearReq{false};

    // running (analysis thread)
    double rate = 0, offset = 0, tone = 0;
    bool audio = false;
    int64_t nIn = 0;
    double loRe = 1, loIm = 0, stRe = 1, stIm = 0;   // mixer
    int loCount = 0;
    double accRe = 0, accIm = 0;
    int accN = 0;
    int64_t k8 = 0;                  // 8 kHz samples made
    double binEnd = 0;
    double afcHz = 0, resHz = 0;     // frequency loop: applied correction, residual average
    int afcCount = 0;
    Biquad bq[2], wbq[2];            // channel filter (narrow) and the wide one that finds a carrier far from where it should be
    BiState sI[2], sQ[2], wI[2], wQ[2];
    double wPrevRe = 0, wPrevIm = 0, wCorRe = 0, wCorIm = 0, wPow = 0;
    int wCount = 0;
    double prevRe = 0, prevIm = 0;
    bool havePrev = false;
    std::vector<double> dphi;        // the last 160 phase steps (radians, after taking off the average)
    size_t dpPos = 0;
    double dpSum = 0;
    double pAll = 0, pNar = 0;       // slow averages of |z|^2 before and after the channel filter
    double meanRe = 1, meanIm = 0;   // slow average of the filtered carrier, for the phase plot
    int64_t k500 = 0;
    double snrDb = -50, levelDb = -200, noiseDens = 0;
    bool carrier = false;
    eczas::EczasReceiver slicer;
    // 4.1 (docs/SENSITIVITY-GAPS.md): a coherent detector beside the phase-step one. The filtered carrier against a reference that follows the
    // midpoint of the two keying phases (decision-directed, about 0.25 s, after the frequency loop's residual is turned out) gives the keying state
    // itself, so a bit is decided from one phase estimate instead of the difference of two: about 3 dB less noise. Its own slicer; a time packet
    // both find counts once. DECT2_ECZAS_NOCOH=1: off (as 4.0)
    eczas::EczasReceiver cohSlicer;
    bool cohOn = true;
    double refRe = 0, refIm = 0, resPh = 0;
    int64_t recentUtc[4] = {};
    int recentPos = 0;
    EczasTelemetry pub;
    double nextReport = 0;
    double nextSecond = 1;
    std::vector<int16_t> traceQ;
    size_t tracePos = 0, traceCount = 0;
    size_t constelPos = 0;
    int constelCount = 0;
    std::vector<float> cI, cQ;
    // frames
    std::deque<EczasFrameLog> flog;
    std::deque<float> offHist;
    std::deque<float> levelHist, snrHist;
    bool havePkt = false;
    eczas::TimePacket pkt;
    double pktStreamSec = 0;         // signal time at which the last packet was decoded (its frame ended)
    double pktUtcStart = 0;          // the received time of its frame start
    int agree = 0;
    bool candValid = false;
    double candStreamSec = 0, candUtcStart = 0;
    uint64_t okN = 0, badN = 0;
    double wallOrigin = 0, wallWin = 0, wallMinCur = 1e300, wallMinPrev = 1e300;
    double offsetMs = 0;
    bool offsetValid = false;
    std::deque<double> offAvg;

    void resetState() {
        rate = rateSet;
        offset = offsetReq.load();
        audio = audioIn.load();
        tone = audioTone.load();
        nIn = 0; loRe = 1; loIm = 0; loCount = 0;
        accRe = accIm = 0; accN = 0; k8 = 0; binEnd = rate > 0 ? rate / kFs : 0;
        afcHz = resHz = 0; afcCount = 0;
        const double q[2] = {0.5412, 1.3066};   // 4th order Butterworth
        for (int i = 0; i < 2; i++) { bq[i].lowpass(kLpHz, kFs, q[i]); sI[i] = BiState(); sQ[i] = BiState(); wbq[i].lowpass(kWideHz, kFs, q[i]); wI[i] = wQ[i] = BiState(); }
        wPrevRe = wPrevIm = wCorRe = wCorIm = wPow = 0; wCount = 0;
        havePrev = false;
        dphi.assign(kBitSamples, 0.0); dpPos = 0; dpSum = 0;
        pAll = pNar = 0; meanRe = 1; meanIm = 0; k500 = 0;
        snrDb = -50; levelDb = -200; carrier = false;
        slicer.configure(500.0, 0);
        { const char* e = getenv("DECT2_ECZAS_NOCOH"); cohOn = !(e && *e && *e != '0'); }
        cohSlicer.configure(500.0, 0);
        cohSlicer.setCoherent(true);
        refRe = refIm = resPh = 0;
        for (auto& u : recentUtc) u = 0;
        recentPos = 0;
        nextReport = 0; nextSecond = 1;
        traceQ.assign(kEczasTrace, 0); tracePos = 0; traceCount = 0;
        cI.assign(kEczasConstel, 0.f); cQ.assign(kEczasConstel, 0.f); constelPos = 0; constelCount = 0;
        levelHist.clear(); snrHist.clear();
        havePkt = false; agree = 0; candValid = false;
        wallWin = 0; wallMinCur = wallMinPrev = 1e300;
        updateStep();
    }
    void updateStep() {
        const double fc = audio ? tone : offset;   // where the carrier sits in the samples
        const double w = -2 * kPi * (fc + afcHz) / rate;
        stRe = std::cos(w); stIm = std::sin(w);
    }
    double tNow() const { return rate > 0 ? (double)nIn / rate : 0; }

    void sample8(double re, double im) {
        k8++;
        pAll += (re * re + im * im - pAll) * (1.0 / 8000);
        // coarse frequency: the carrier through a wide filter, its offset from the phase advance per sample, every half second
        {
            double wr = re, wi = im;
            for (int i = 0; i < 2; i++) { wr = biRun(wbq[i], wI[i], wr); wi = biRun(wbq[i], wQ[i], wi); }
            wCorRe += wr * wPrevRe + wi * wPrevIm; wCorIm += wi * wPrevRe - wr * wPrevIm;
            wPow += wr * wr + wi * wi;
            wPrevRe = wr; wPrevIm = wi;
            if (++wCount >= 4000) {
                const double strength = std::sqrt(wCorRe * wCorRe + wCorIm * wCorIm) / (wPow + 1e-30);   // 1: a clean tone; modulation and noise lower it
                const double f = std::atan2(wCorIm, wCorRe) * kFs / (2 * kPi);
                if (strength > 0.6 && std::fabs(f) > 12) { afcHz = std::max(-250.0, std::min(250.0, afcHz + f)); updateStep(); havePrev = false; resHz = 0; }
                wCorRe = wCorIm = wPow = 0; wCount = 0;
            }
        }
        // channel filter
        double ar = re, ai = im;
        for (int i = 0; i < 2; i++) { ar = biRun(bq[i], sI[i], ar); ai = biRun(bq[i], sQ[i], ai); }
        pNar += (ar * ar + ai * ai - pNar) * (1.0 / 8000);
        const double mag2 = ar * ar + ai * ai;
        double d = 0;
        if (havePrev) {
            const double cr = ar * prevRe + ai * prevIm, ci = ai * prevRe - ar * prevIm;   // z * conj(prev)
            d = std::atan2(ci, cr);
        }
        prevRe = ar; prevIm = ai; havePrev = true;
        // frequency loop: the residual after the mixer, averaged over about a second
        const double stepHz = d * kFs / (2 * kPi);
        resHz += (std::max(-20.0, std::min(20.0, stepHz)) - resHz) * (1.0 / 4000);
        if (++afcCount >= 800) {
            afcCount = 0;
            if (carrier) { afcHz = std::max(-250.0, std::min(250.0, afcHz + 0.5 * resHz)); updateStep(); resHz *= 0.5; }
        }
        const double dp = d - 2 * kPi * resHz / kFs;
        resPh += 2 * kPi * resHz / kFs;
        if (resPh > kPi) resPh -= 2 * kPi; else if (resPh < -kPi) resPh += 2 * kPi;
        dpSum += dp - dphi[dpPos];
        dphi[dpPos] = dp;
        if (++dpPos >= (size_t)kBitSamples) dpPos = 0;
        // the phase plot: the carrier turned to the real axis by its slow mean
        meanRe += (ar - meanRe) * (1.0 / 16000); meanIm += (ai - meanIm) * (1.0 / 16000);
        if ((k8 & 63) == 0) {   // 125 points a second
            const double mm = std::sqrt(meanRe * meanRe + meanIm * meanIm) + 1e-30;
            const double rr = (ar * meanRe + ai * meanIm) / mm, ri = (ai * meanRe - ar * meanIm) / mm;
            const double sc = 1.0 / (std::sqrt(pNar) + 1e-30);
            cI[constelPos] = (float)(rr * sc); cQ[constelPos] = (float)(ri * sc);
            constelPos = (constelPos + 1) % kEczasConstel; if (constelCount < (int)kEczasConstel) constelCount++;
        }
        (void)mag2;
        if ((k8 % kDecim) == 0) {
            // the phase change over the last bit period in units of a full +-36 degree step
            double s = dpSum / kRampRad * kSymbolScale;
            if (!carrier) s = 0;
            const int16_t v = (int16_t)std::lround(std::max(-32768.0, std::min(32767.0, s)));
            traceQ[tracePos] = v; tracePos = (tracePos + 1) % kEczasTrace; if (traceCount < kEczasTrace) traceCount++;
            slicer.feed(&v, 1);
            if (cohOn) {
                // the carrier turned back by the residual the frequency loop has measured but not yet put into the mixer
                const double c = std::cos(resPh), sn = std::sin(resPh);
                const double yr = ar * c + ai * sn, yi = ai * c - ar * sn;
                const double r2 = refRe * refRe + refIm * refIm;
                double q = 0;
                if (r2 > 1e-30) q = (yi * refRe - yr * refIm) / (r2 * std::sin(kRampRad / 2));   // +-1 at the two keying phases
                if (!carrier) q = 0;
                // the reference: the sample turned by the half step of the state it was decided as, slowly averaged
                const double h = q >= 0 ? -kRampRad / 2 : kRampRad / 2, hc = std::cos(h), hs = std::sin(h);
                refRe += ((yr * hc - yi * hs) - refRe) * (1.0 / 125);
                refIm += ((yr * hs + yi * hc) - refIm) * (1.0 / 125);
                const int16_t cv = (int16_t)std::lround(std::max(-32768.0, std::min(32767.0, q * kSymbolScale)));
                cohSlicer.feed(&cv, 1);
            }
            k500++;
            drainSlicer();
        }
    }

    void measureSnr() {
        // noise density from what is outside the channel filter; the carrier is what is left inside
        const double bn = 2 * 1.026 * kLpHz;
        const double n0 = std::max(1e-30, (pAll - pNar) / (kFs - bn));
        const double c = std::max(1e-30, pNar - n0 * bn);
        noiseDens = n0;
        levelDb = (float)(10 * std::log10(c));
        const double snr = 10 * std::log10(c / (n0 * kBandwidthRef));
        snrDb = std::max(-50.0, std::min(80.0, snr));
        if (!carrier && snrDb > 6) carrier = true;
        else if (carrier && snrDb < 3) carrier = false;
    }

    void addFrameLog(int kind, bool ok, const std::string& text, const uint8_t* f, double startSec) {
        EczasFrameLog l;
        l.streamSec = startSec;
        l.wallTime = (int64_t)wallNow();
        l.kind = kind; l.ok = ok; l.text = text;
        char b[40];
        for (int i = 0; i < eczas::kFrameBytes; i++) { snprintf(b, sizeof b, "%02X", f[i]); l.hex += b; if (i < eczas::kFrameBytes - 1) l.hex += ' '; }
        flog.push_front(std::move(l));
        while (flog.size() > kEczasLogMax) flog.pop_back();
    }

    // a time packet the other slicer already gave
    bool seenUtc(int64_t u) const { for (int64_t v : recentUtc) if (v == u) return true; return false; }
    void drainSlicer() {
        eczas::EczasRxPacket p;
        while (cohSlicer.pop(p)) {   // the coherent slicer adds time packets only: the frame counts and other packets stay the phase-step slicer's
            if (p.time.status != eczas::DecodeStatus::CrcOk || seenUtc(p.time.utcSec)) continue;
            takePacket(p);
        }
        while (slicer.pop(p)) {
            if (p.time.status == eczas::DecodeStatus::CrcOk && seenUtc(p.time.utcSec)) continue;
            takePacket(p);
        }
    }
    void takePacket(const eczas::EczasRxPacket& p) {
        {
            // stream time of the first bit of the frame: the symbol of bit 0 is 964 samples (500 Hz) before the newest one the slicer had, and a symbol is
            // the phase change over the bit that ends there
            const double bit0End = ((double)p.endIdx - 963.0) / 500.0;
            if (p.time.status == eczas::DecodeStatus::CrcOk) {
                okN++;
                recentUtc[recentPos] = p.time.utcSec; recentPos = (recentPos + 1) % 4;
                const double utcStart = (double)p.time.utcSec - kStampAfterFrameStartSec;
                const double startSec = bit0End - 0.02 - kFrontEndDelaySec;
                // a check byte of 8 bits lets about one in 256 damaged frames through: once the time is established (two packets agree with each other and
                // with the elapsed signal time) a packet that disagrees is kept apart until a second one confirms it
                bool take = true;
                char t[112];
                int y, mo, d, h, mi, s;
                eczas::civilFromUnix(p.time.utcSec, y, mo, d, h, mi, s);
                snprintf(t, sizeof t, "%04d-%02d-%02d %02d:%02d:%02d UTC, +%d h", y, mo, d, h, mi, s, p.time.tzHours);
                if (havePkt) {
                    auto agrees = [&](double s0, double u0) { return std::fabs((startSec - s0) - (utcStart - u0)) < 2.0 && utcStart > u0; };
                    if (agrees(pktStreamSec, pktUtcStart)) { agree++; candValid = false; }
                    else if (agree < 2) agree = 0;   // not established yet: the newer one replaces it
                    else if (candValid && agrees(candStreamSec, candUtcStart)) { agree = 1; candValid = false; }
                    else { candValid = true; candStreamSec = startSec; candUtcStart = utcStart; take = false; }
                }
                if (take) {
                    pkt = p.time; havePkt = true;
                    pktStreamSec = startSec; pktUtcStart = utcStart;
                    pub.syncQuality = p.syncQuality;
                    addFrameLog(0, true, t, p.frame, startSec);
                    // the computer clock against the received time
                    const int cm = clockMode.load();
                    double w0 = 0;
                    bool haveW = false;
                    if (cm == 1) { const double m = std::min(wallMinCur, wallMinPrev); if (m < 1e299) { w0 = m; haveW = true; } }
                    else if (cm == 2) { w0 = clockOrigin.load(); haveW = true; }
                    if (haveW) {
                        const double wallStart = w0 + startSec;
                        const double dist = haversineKm(rxLat.load(), rxLon.load(), kTxLat, kTxLon);
                        const double prop = dist / kPathC * 1000.0;
                        offsetMs = (wallStart - utcStart) * 1000.0 - prop;
                        offsetValid = true;
                        offHist.push_back((float)offsetMs);
                        while (offHist.size() > kEczasOffsetHist) offHist.pop_front();
                        offAvg.push_back(offsetMs);
                        while (offAvg.size() > 10) offAvg.pop_front();
                    }
                } else {
                    snprintf(t, sizeof t, "%04d-%02d-%02d %02d:%02d:%02d UTC: disagrees with the time already received", y, mo, d, h, mi, s);
                    addFrameLog(3, false, t, p.frame, startSec);
                }
            } else if (p.time.status == eczas::DecodeStatus::BadCrc) {
                badN++;
                addFrameLog(1, false, "CRC failed", p.frame, bit0End - 0.02);
            } else {
                char t[48];
                snprintf(t, sizeof t, "slot packet 0x%02X (not the time packet)", p.frame[2]);
                addFrameLog(2, false, t, p.frame, bit0End - 0.02);
            }
        }
    }

    void report() {
        measureSnr();
        const double tt = tNow();
        EczasTelemetry& t = pub;
        t.seq++;
        t.cfoHz = afcHz + resHz;
        t.snrDb = (float)snrDb;
        t.levelDb = (float)levelDb;
        t.timeSec = tt; t.inputRate = rate;
        t.audioInput = audio;
        const eczas::EczasRxStats& st = slicer.stats();
        t.syncs = st.syncs; t.frames = st.frames; t.otherType = st.otherType; t.falseSync = st.falseSync;
        t.blocksOk = okN; t.blocksBad = badN;
        t.timeValid = havePkt;
        t.pkt = pkt;
        t.ageSec = havePkt ? tt - pktStreamSec : 0;
        t.utcNow = havePkt ? pktUtcStart + (tt - pktStreamSec) : 0;
        const bool recent = havePkt && t.ageSec < 180;
        t.dataValid = recent;
        t.confirmed = recent && agree >= 1;
        t.state = recent ? 2 : (carrier ? 1 : 0);
        t.clockValid = clockMode.load() != 0 && offsetValid;
        t.clockOffsetMs = offsetMs;
        double sum = 0;
        for (double v : offAvg) sum += v;
        t.clockOffsetAvgMs = offAvg.empty() ? 0 : sum / (double)offAvg.size();
        t.locationSet = rxLocSet.load();
        t.distanceKm = haversineKm(rxLat.load(), rxLon.load(), kTxLat, kTxLon);
        t.propDelayMs = t.distanceKm / kPathC * 1000.0;
        t.offsetHist.assign(offHist.begin(), offHist.end());
        t.levelHist.assign(levelHist.begin(), levelHist.end());
        t.snrHist.assign(snrHist.begin(), snrHist.end());
        t.trace.clear();
        for (size_t i = 0; i < traceCount; i++) t.trace.push_back(traceQ[(tracePos + kEczasTrace - traceCount + i) % kEczasTrace]);
        t.constelI.clear(); t.constelQ.clear();
        const int n = constelCount;
        for (int i = 0; i < n; i++) {
            const size_t k = (constelPos + kEczasConstel - (size_t)n + (size_t)i) % kEczasConstel;
            t.constelI.push_back(cI[k]); t.constelQ.push_back(cQ[k]);
        }
        t.log.assign(flog.begin(), flog.end());
    }
};

EczasLiveReceiver::EczasLiveReceiver() : p_(std::make_unique<Impl>()) {}
EczasLiveReceiver::~EczasLiveReceiver() = default;

void EczasLiveReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = inputRateHz;
    p_->resetReq = true;
}
void EczasLiveReceiver::setSignalOffset(double hz) { if (hz != p_->offsetReq.exchange(hz)) p_->resetReq = true; }
void EczasLiveReceiver::setAudioInput(bool on, double toneHz) {
    const bool o = p_->audioIn.exchange(on);
    const double t = p_->audioTone.exchange(toneHz);
    if (o != on || t != toneHz) p_->resetReq = true;
}
void EczasLiveReceiver::setClockMode(int mode, double originSec) { p_->clockOrigin = originSec; p_->clockMode = mode; }
void EczasLiveReceiver::setReceiverLocation(double lat, double lon, bool userSet) { p_->rxLat = lat; p_->rxLon = lon; p_->rxLocSet = userSet; }
bool EczasLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet >= (p_->audioIn.load() ? 7900.0 : eczasTuning().minSampleRate - 1);
}
void EczasLiveReceiver::reset() { p_->resetReq = true; }
void EczasLiveReceiver::clearResults() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.log.clear(); p_->pub.offsetHist.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}
void EczasLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool EczasLiveReceiver::telemetry(EczasTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void EczasLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.resetState();
    }
    if (m.clearReq.exchange(false)) { m.flog.clear(); m.offHist.clear(); m.offAvg.clear(); m.okN = m.badN = 0; m.havePkt = false; m.offsetValid = false; }
    if (n == 0 || m.rate < (m.audio ? 7900.0 : eczasTuning().minSampleRate - 1)) return;
    const double r = m.rate;
    const double binW = r / kFs;
    // an audio recording is real: the carrier tone is mixed to zero and its image falls outside the channel filter
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        if (m.audio) im = 0;
        // mixer: z * lo
        m.accRe += re * m.loRe - im * m.loIm;
        m.accIm += re * m.loIm + im * m.loRe;
        m.accN++;
        const double nr = m.loRe * m.stRe - m.loIm * m.stIm, ni = m.loRe * m.stIm + m.loIm * m.stRe;
        m.loRe = nr; m.loIm = ni;
        if (++m.loCount >= 4096) { m.loCount = 0; const double a = 1.0 / std::sqrt(m.loRe * m.loRe + m.loIm * m.loIm); m.loRe *= a; m.loIm *= a; }
        m.nIn++;
        if ((double)m.nIn >= m.binEnd) {
            const double inv = 1.0 / (double)m.accN;
            m.sample8(m.accRe * inv, m.accIm * inv);
            m.accRe = m.accIm = 0; m.accN = 0;
            m.binEnd = (double)(m.k8 + 1) * binW;
        }
        if (m.tNow() >= m.nextReport) {
            m.nextReport += 0.25;
            std::lock_guard<std::mutex> lk(m.mu);
            if (m.tNow() >= m.nextSecond) {
                m.nextSecond += 1;
                m.levelHist.push_back((float)m.levelDb); m.snrHist.push_back((float)m.snrDb);
                while (m.levelHist.size() > kEczasHist) { m.levelHist.pop_front(); m.snrHist.pop_front(); }
            }
            m.report();
        }
    }
    if (m.clockMode.load() == 1) {   // the earliest arrival over the last half minute stands for sample 0's time
        const double w = wallNow();
        const double v = w - (double)m.nIn / r;
        if (m.wallWin == 0) m.wallWin = w;
        if (w - m.wallWin > 30) { m.wallMinPrev = m.wallMinCur; m.wallMinCur = 1e300; m.wallWin = w; }
        m.wallMinCur = std::min(m.wallMinCur, v);
    }
}

ModeTuning eczasTuning() {
    ModeTuning t;
    t.stdMode = 87; t.id = "eczas"; t.name = "e-Czas (225 kHz, Poland)";
    t.minMhz = 0.2; t.maxMhz = 0.25; t.defMhz = 0.225;
    t.sampleRate = 256000;                   // a rate most LF / HF radios offer (RTL-SDR direct sampling, Airspy HF+, SDRplay)
    t.basebandHz = 0;
    t.bandwidthMhz = 0.01;
    t.minSampleRate = 48000;
    t.tuneOffsetHz = 12000;                  // the radio sits 12 kHz above the carrier, away from its DC spike
    return t;
}

} // namespace dect2
