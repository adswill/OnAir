// The GNSS receiver's private state (Impl) and its constants, shared by gnss_rx.cpp and the gnss_rx_*.cpp files that hold the bigger
// member functions. Not part of the API.
#pragma once
#include "dect2/gnss/gnss_rx.h"
#include "dect2/gnss/gnss_interf.h"
#include "dect2/gnss/gnss_acq.h"
#include "dect2/gnss/gnss_bds.h"
#include "dect2/gnss/gnss_codes.h"
#include "dect2/gnss/gnss_front.h"
#include "dect2/gnss/gnss_glo.h"
#include "dect2/gnss/gnss_msg.h"
#include "dect2/gnss/gnss_nav.h"
#include "dect2/gnss/gnss_solve.h"
#include "dect2/gnss/gnss_track.h"
#include "dect2/net/feeds.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>

namespace dect2 {

namespace gnss_rx_detail {
constexpr double kGpsFsOut = 4.096e6;
constexpr int kGpsN = 4096;
constexpr int kMaxChannels = 40;
constexpr size_t kBlock = 16384;
// The search: a static receiver sees the satellites within +-5 kHz of the radio's own frequency error; that error is up to 2 kHz with a TCXO,
// 31 kHz on a HackRF (20 ppm) and 160 kHz on a dongle without one (100 ppm). The windows widen in that order while nothing is found.
constexpr double kSatDopplerHz = 5500.0;
constexpr double kMidHz = 45000.0, kWideHz = 170000.0;
constexpr int kShortMs = 16, kLongMs = 64;

struct SatNav {
    GpsEphemeris eph;            // the complete, consistent ephemeris
    bool hasEph = false;
    int week = 0;                // full week of the ephemeris (from subframe 1)
    GpsEphemeris pend;           // being collected
    int pendWeek = 0;
    GpsAlmanac alm;
    double ephSignalTime = 0;    // signal time when it was received
    int towLast = -1;
    int lastSfId = 0;            // the last subframe received and when (for the time left until the ephemeris is complete)
    double lastSfTime = -1;
    // SBAS
    int sbasLast = -1;
    uint32_t sbasCount = 0, sbasTypes = 0;
};
// the L1 C/A PRNs share one numbering: GPS 1..32, SBAS 120..158, QZSS 193..202
constexpr int kCaPrns = 203;
// Galileo E1-B: 4 ms codes of 4092 chips with the BOC(1,1) subcarrier, counted in half chips at 2.046 MHz; one code period is 16384 samples of the band
constexpr int kGalPrns = 37;                 // 1..36
constexpr int kGalHalfChips = 2 * kGalE1Len;
constexpr int kGalN = 16384;
constexpr int kGalMs = 4;                    // code periods integrated per search (16 ms, without coherent pairs: a symbol every period)
// BeiDou B1I: 1 ms codes of 2046 chips at 2.046 Mcps, 4096 samples of its own band; ranging codes 1..37
constexpr int kBdsN = 4096;
constexpr int kBdsPrns = 64;
// GLONASS L1OF: 1 ms codes of 511 chips at 0.511 Mcps, 1024 samples of each frequency channel's own band
constexpr double kGloFsOut = 1.024e6;
constexpr int kGloN = 1024;

// The search of a band without GPS to lean on: the window widens while nothing is found, as for GPS (planSearch)
struct SearchPlan {
    int stage = 0;
    uint32_t stageRounds = 0;
    int roundHits = 0, lastRoundHits = 0;
    bool longRound = false;
    double lastCfo = 0;
    bool lastCfoValid = false;
};

inline double cpuNow() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

inline std::string satName(int sys, int prn) { return gnssSatName(sys, prn); }

} // namespace gnss_rx_detail
using namespace gnss_rx_detail;

struct GnssReceiver::Impl {
    std::mutex procMu, telMu;
    std::atomic<double> centerMhz{1575.42};
    std::atomic<unsigned> sysMask{0xFFFFFFFFu};
    std::atomic<int> weekRef{0};
    std::atomic<double> dopHalf{10000.0};
    std::atomic<double> cfoHintHz{0};
    std::atomic<bool> cfoHintValid{false};
    std::atomic<int> acqRate{10};
    std::atomic<double> elMask{5.0};
    std::atomic<double> hintLat{0}, hintLon{0};
    std::atomic<bool> hintValid{false};
    std::atomic<uint32_t> settingsGen{1};
    std::atomic<double> rateAt{0};
    uint32_t planGen = 0;
    double rate = 0;
    std::function<void(const std::string&)> log;
    std::function<void(const std::vector<GnssMeasurement>&)> measCb;
    std::function<void(int, const uint8_t*, double)> sbasCb;     // SBAS messages with a good CRC (setSbasCallback)
    uint64_t units = 0;
    double cpuFront = 0, cpuTrack = 0, cpuAcq = 0;

    // front end
    double dcI = 0, dcQ = 0, gain = 1, levelRms = 0;
    bool dcInit = false;
    double clipFrac = 0;
    int64_t inSamples = 0;
    GnssInterfMonitor interf;        // interference and spoofing monitor on the raw input
    std::vector<cf32> blk;
    std::vector<cf32> pend;          // input waiting for a full block
    // GPS band
    bool gpsActive = false;
    GnssBand gpsBand;
    GnssAcq gpsAcq;
    uint8_t gpsChips[kCaPrns][kGpsCaLen];
    // Galileo E1-B on the same band
    bool galActive = false;
    GnssAcq galAcq;
    GnssSignalSpec galSpec;
    std::vector<uint8_t> galChips[kGalPrns];     // half chips: the code chip xor the subcarrier half (logic 0 = +1)
    double galCredit = 0;
    SatNav gsat[kGalPrns];
    GalNav gnav[kGalPrns];
    GalRlmAssembler rlmAsm[kGalPrns];
    uint8_t lastNma = 0xFF;
    bool ggtoValid = false;                      // GST - GPS from word type 10
    double ggtoA0 = 0, ggtoA1 = 0, ggtoT0 = 0;
    int ggtoWn0 = 0, ggtoWn = 0;
    std::vector<int> predictedGal;
    GnssAcq auxAcq;
    double auxCredit = 0;
    uint32_t auxRoundSeen = 0;
    int auxRoundHits = 0, auxEmptyRounds = 0;
    double auxIdleUntil = 0;
    uint32_t galRoundSeen = 0;
    bool galLong = false;
    double codeRateFrac = 0;
    int galRoundHits = 0, galEmptyRounds = 0;
    double galIdleUntil = 0;
    GnssSignalSpec gpsSpec, qzssSpec, sbasSpec;
    // BeiDou B1I band (its own tune, or with L1 at 20 Msps and more)
    bool bdsActive = false;
    GnssBand bdsBand;
    GnssAcq bdsAcq;
    GnssSignalSpec bdsD1Spec, bdsD2Spec;
    std::vector<uint8_t> bdsChips[kBdsPrns];
    SatNav bsat[kBdsPrns];
    BdsNav bnav[kBdsPrns];
    bool bdtGpsValid = false;                    // BDT - GPS from D1 subframe 5 page 9
    double bdtA0 = 0, bdtA1 = 0;
    double bdsCredit = 0;
    SearchPlan bdsPlan;
    uint32_t bdsRoundSeen = 0;
    int bdsRoundHits = 0, bdsEmptyRounds = 0;
    double bdsIdleUntil = 0;
    // GLONASS: a band per frequency channel k = -7..6 (index k + 7), cut from the input or from a wide band around them all
    bool gloActive = false;
    bool gloSlotOn[kGloSlots] = {};
    bool gloUseWide = false;
    GnssBand gloWide;
    int64_t gloWideFed = 0;
    double gloCenterNow = 0;
    GnssBand gloBand[kGloSlots];
    GnssAcq gloAcq[kGloSlots];
    GnssSignalSpec gloSpec[kGloSlots];
    uint8_t gloChips[kGloLen];
    SatNav gloSat[kGloSlots];
    GloNav gloNav[kGloSlots];
    GloEphemeris gloEph[kGloSlots];
    bool gloTimeValid = false;                   // string 5: tau_c, tau_GPS, N4
    double gloTauGps = 0, gloTauC = 0;
    int gloN4 = 0;
    double gloCredit = 0;
    SearchPlan gloPlan;
    int gloNext = 0;
    uint32_t gloRoundSeen = 0;
    int gloRoundHits = 0, gloEmptyRounds = 0;
    double gloIdleUntil = 0;
    double acqCredit = 0;
    std::vector<std::unique_ptr<GnssTracker>> trackers;
    // navigation data
    SatNav sat[kCaPrns];
    GpsIono iono;
    GpsUtc utc;
    double almToa = -1;
    int almWna = -1;
    bool almGot = false;
    // fix
    GnssFix fix;
    bool hasPrior = false;
    double prior[3] = {0, 0, 0};
    double nextMeas = 1.0;
    uint64_t okRetired = 0, badRetired = 0;
    uint64_t fixCount = 0;
    double firstFix = -1;
    double lastFixSignal = -1;
    bool clockSet = false;
    double clockOffset = 0;
    // the computer's clock against the fix: the earliest arrival over two half minutes stands for sample 0's time
    std::atomic<bool> clockCompare{false};
    double wallMinCur = 1e300, wallMinPrev = 1e300, wallWin = 0;
    std::deque<float> offWin, offHist, levHist, snrHist;
    float offNow = NAN;
    bool clkValid = false;
    double clkOff = 0, clkAvg = 0, histNext = 0;
    uint64_t sbasTotal = 0;
    double cfoEst = 0, driftEst = 0;
    bool cfoValid = false;
    // the search plan: the stage of widening while nothing is found, and the last frequency error that worked
    int stage = 0;
    uint32_t stageRounds = 0;
    int roundHits = 0, lastRoundHits = 0;   // finds in the round in progress and in the last one
    bool longRound = false;
    double lastCfo = 0;
    bool lastCfoValid = false;
    double firstLock = -1;
    std::vector<GnssMeasurement> lastMeas;
    std::vector<int> predictedVisible;
    bool havePredicted = false;
    // per-satellite last report of the fix
    struct SolInfo { bool used = false; float resid = 0; float az = 0, el = 0; bool has = false; };
    SolInfo sol[kCaPrns];
    SolInfo gsol[kGalPrns];
    SolInfo bsol[kBdsPrns];
    SolInfo glsol[kGloSlots];
    // a satellite's place in the tables: its PRN, for GLONASS its frequency channel k + 7 (see keyOf)
    SatNav& navOf(int sys, int key) { return sys == GnssGalileo ? gsat[key] : sys == GnssBeidou ? bsat[key] : sys == GnssGlonass ? gloSat[key] : sat[key]; }
    const SatNav& navOf(int sys, int key) const { return sys == GnssGalileo ? gsat[key] : sys == GnssBeidou ? bsat[key] : sys == GnssGlonass ? gloSat[key] : sat[key]; }
    SolInfo& solOf(int sys, int key) { return sys == GnssGalileo ? gsol[key] : sys == GnssBeidou ? bsol[key] : sys == GnssGlonass ? glsol[key] : sol[key]; }
    const SolInfo& solOf(int sys, int key) const { return sys == GnssGalileo ? gsol[key] : sys == GnssBeidou ? bsol[key] : sys == GnssGlonass ? glsol[key] : sol[key]; }
    static int keyOf(int sys, int prn, int fcn) { return sys == GnssGlonass ? fcn - kGloMinK : prn; }
    int leapNow() const { return utc.valid ? utc.dtls : 18; }
    // GLONASS: the satellite's time of day (near the day of its ephemeris) on the GPS time scale, seconds of the GPS week
    double gloToGps(const GloEphemeris& e, double tod) const {
        int n4 = gloN4;
        if (n4 <= 0) n4 = (int)(((int64_t)referenceWeek() * 7 - kGloDaysTo1996) / 1461) + 1;
        // the day of tod: the ephemeris day, or the one before or after when tod lies across midnight from tb
        const double rel = gloWrapDay(tod - e.tb);
        const double dayShift = std::round(((e.tb + rel) - tod) / 86400.0);
        // the day of the week in whole numbers first: seconds since 1980 do not keep nanoseconds in a double
        const int64_t days = (int64_t)kGloDaysTo1996 + (int64_t)(n4 - 1) * 1461 + (e.NT - 1) + (int64_t)dayShift;
        double t = (double)(days % 7) * 86400.0 + tod - 10800.0 + (double)leapNow() + (gloTimeValid ? gloTauGps : 0.0);
        if (t < 0) t += 604800.0;
        if (t >= 604800.0) t -= 604800.0;
        return t;
    }
    int gloGpsWeek(const GloEphemeris& e, double tod) const {
        int n4 = gloN4;
        if (n4 <= 0) n4 = (int)(((int64_t)referenceWeek() * 7 - kGloDaysTo1996) / 1461) + 1;
        const double days = (double)kGloDaysTo1996 + (double)(n4 - 1) * 1461.0 + (double)(e.NT - 1);
        return (int)std::floor((days * 86400.0 + tod - 10800.0 + (double)leapNow()) / 604800.0);
    }
    // the GLONASS time of day at GPS time of week t
    double gpsToGloTod(double t) const { double v = std::fmod(t + 10800.0 - (double)leapNow() - (gloTimeValid ? gloTauGps : 0.0), 86400.0); return v < 0 ? v + 86400.0 : v; }
    // BDT - GPS time less the whole 14 s at BDT t (A0GPS + A1GPS t, ICD 5.2.4.18)
    double bdtAt(double t) const { return bdtGpsValid ? bdtA0 + bdtA1 * t : 0.0; }
    // a satellite's own time (GPS, QZSS: GPS time; Galileo: GST; BeiDou: BDT) on the GPS time scale
    double toGpsTime(int sys, double t) const {
        if (sys == GnssGalileo) return t - ggtoAt(t);
        if (sys == GnssBeidou) { double g = t + kBdtGpsSeconds - bdtAt(t); if (g >= 604800.0) g -= 604800.0; return g; }
        return t;
    }
    static double carrierOf(int sys, int fcn = 0) { return sys == GnssBeidou ? kBdsB1Hz : sys == GnssGlonass ? gloL1Hz(fcn) : kGpsL1Hz; }
    GnssBand& bandOf(const GnssTracker& t) { return t.band == 1 ? bdsBand : t.band >= 2 ? gloBand[t.band - 2] : gpsBand; }
    // GST - GPS time at GPS time of week t (word type 10: A0G + A1G (t - t0G + 604800 (WN - WN0G)), the 6 bit week difference within +-31)
    double ggtoAt(double t) const {
        if (!ggtoValid) return 0;
        int dw = ((ggtoWn - ggtoWn0) % 64 + 64) % 64;
        if (dw > 31) dw -= 64;
        return ggtoA0 + ggtoA1 * (t - ggtoT0 + 604800.0 * dw);
    }
    // telemetry
    uint64_t seq = 0;
    double nextReport = 0.25;
    GnssTelemetry tel;
    bool telReady = false;
    std::vector<float> acqCorr;

    void logf(const std::string& s) { if (log) log(s); }

    int referenceWeek() const {
        int w = weekRef.load();
        if (w > 0) return w;
        const time_t now = time(nullptr);
        if (now > 1700000000) return (int)((now - 315964800 + 18) / 604800);
        return 2400;
    }

    // ------------------------------------------------------------------ set-up
    void replan();
    // the bands that the tuning and the rate hold, by the system that owns each (L1: GPS)
    unsigned activeMask() const {
        return (gpsActive ? gnssSystemBit(GnssGps) : 0u) | (bdsActive ? gnssSystemBit(GnssBeidou) : 0u) | (gloActive ? gnssSystemBit(GnssGlonass) : 0u);
    }
    void dropBand(int b) {
        trackers.erase(std::remove_if(trackers.begin(), trackers.end(), [b](const std::unique_ptr<GnssTracker>& t) { return t->band == b; }), trackers.end());
    }
    bool galRebuild = false;
    double lastRate = 0;
    unsigned gpsMaskNow = 0;
    bool l1Standalone = false;     // SBAS / QZSS without GPS: gpsAcq searches their PRNs
    // the systems searched or tracked now (GnssTelemetry::decodeMask)
    unsigned decodeMask() const {
        const unsigned b = (bdsActive ? gnssSystemBit(GnssBeidou) : 0u) | (gloActive ? gnssSystemBit(GnssGlonass) : 0u);
        if (!gpsActive) return b;
        return (gpsMaskNow & (gnssSystemBit(GnssGps) | gnssSystemBit(GnssQzss) | gnssSystemBit(GnssSbas))) | (galActive ? gnssSystemBit(GnssGalileo) : 0u) | b;
    }

    void resetState();

    // ------------------------------------------------------------------ processing
    bool tracked(int prn) const {
        const int sys = l1caSystem(prn);
        for (auto& t : trackers) if (t->sys == sys && t->prn == prn) return true;
        return false;
    }
    bool trackedGal(int prn) const {
        for (auto& t : trackers) if (t->sys == GnssGalileo && t->prn == prn) return true;
        return false;
    }
    bool trackedSys(int sys, int prn) const {
        for (auto& t : trackers) if (t->sys == sys && t->prn == prn) return true;
        return false;
    }
    bool gpsLocked() const {
        for (auto& t : trackers) if (t->sys == GnssGps && t->carrierLocked()) return true;
        return false;
    }

    void startFromHit(const GnssAcqHit& found);

    void startGalFromHit(const GnssAcqHit& found);

    void startBdsFromHit(const GnssAcqHit& found);

    bool trackedGlo(int slot) const {
        for (auto& t : trackers) if (t->sys == GnssGlonass && t->fcn == slot + kGloMinK) return true;
        return false;
    }
    static std::string gloName(int prn, int fcn) {
        char b[24];
        if (prn > 0) snprintf(b, sizeof b, "R%02d (k%+d)", prn, fcn); else snprintf(b, sizeof b, "R k%+d", fcn);
        return b;
    }

    void startGloFromHit(int slot, const GnssAcqHit& found);

    // GLONASS strings: the ephemeris from strings 1-4 of one frame, the time corrections and the four-year interval from string 5
    void handleGlo(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow);

    // BeiDou: the ephemeris from D1 subframes 1-3 of one frame or D2 pages 1-10; BDT - GPS from D1 subframe 5 page 9
    void handleBds(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow);

    // Galileo I/NAV words: the ephemeris from words 1-4 of one IODnav and word 5 (BGD, health, week); the GST-GPS offset from word 10
    void handleInav(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow);

    void handleSubframe(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow);

    void runTrackers(double signalNow);

    bool skipPrn(int prn) const {
        if (tracked(prn)) return true;
        // with a fix and the sky predicted from the almanac, look for the satellites that should be in view; the others now and then
        if (havePredicted && gpsAcq.rounds() % 6 != 5 && std::find(predictedVisible.begin(), predictedVisible.end(), prn) == predictedVisible.end()) return true;
        return false;
    }

    // Where and how long to search next. With satellites locked the radio's error lies within 5.5 kHz of each of their Dopplers, so the others are within
    // 11 kHz of all of them; with a fix it is measured. With nothing locked the window widens round by round (TCXO, HackRF, cheap dongle), then the
    // same with a four times longer integration for weak signals, and starts over.
    void planSearch();

    // Galileo: searched once GPS has a fix. The receiver's frequency error is then measured, so the window is the satellites' Doppler alone
    // (+-5 kHz), and a sky without Galileo costs little (the search sleeps after a round without a find).
    void runGalAcq(double seconds, double signalNow);

    // QZSS and SBAS: their own search, once a GPS satellite is locked (in the window the GPS search uses), with half the GPS search's work.
    // A round that finds nothing while none of them is tracked puts it to sleep (10 s, 20 s, .. 80 s).
    void runAuxAcq(double seconds, double signalNow);

    // The same plan for a band of its own (BeiDou or GLONASS tuned without L1): its own channels say where the radio's error lies, else the
    // window widens round by round. Frequencies scale with the carrier: the remembered error is at L1.
    void planOwn(GnssAcq& acq, SearchPlan& pl, int sys, double rf, const char* name);

    // The sample clock error against the code, from the GPS channels whose code loop has settled (median); false while there is none
    bool updateCodeRate(int sys = GnssGps, double chipRate = kGpsCaChipRate);

    // BeiDou: with L1 in the same tune it is searched once GPS has a fix, in the window of the measured frequency error (as Galileo); on a tune of
    // its own it searches like GPS does, the window widening while nothing is found.
    void runBdsAcq(double seconds, double signalNow);

    // GLONASS: every frequency channel has its own search of the one code. With L1 in the tune they follow the GPS window (scaled to 1602 MHz)
    // once GPS is locked; on a tune of their own they share one widening plan. The work goes round the channels.
    void runGloAcq(double seconds, double signalNow);

    void runAcq(double seconds);

    void trimBands();

    // ------------------------------------------------------------------ the fix
    void measure(double S);

    // The receiver's own frequency error from the Doppler of the satellites: measured minus what the geometry says, and the clock drift
    void estimateClock(const GnssSolution& sl, const std::vector<GnssObs>& obs, const std::vector<GnssMeasurement>& meas, const std::vector<size_t>& idx, double tr0);

    void predictSky(double tow);

    // ------------------------------------------------------------------ telemetry
    void buildTelemetry(double signalNow);

    // ------------------------------------------------------------------ one block of input
    void processBlock(const cf32* x, size_t m) {
        interf.process(x, m, dcI, dcQ);
        // One pass: remove the DC and scale with the estimates of the blocks before, and measure this block for the next one
        blk.resize(m);
        const float dr = (float)dcI, di = (float)dcQ, gf = (float)gain;
        const float* xf = reinterpret_cast<const float*>(x);
        float* of = reinterpret_cast<float*>(blk.data());
        float sr[4] = {}, si[4] = {}, sq[4] = {};
        size_t clip = 0;
        size_t i = 0;
        for (; i + 4 <= m; i += 4) {
            for (int l = 0; l < 4; l++) {
                const float a = xf[2 * (i + l)], b = xf[2 * (i + l) + 1];
                sr[l] += a; si[l] += b; sq[l] += a * a + b * b;
                of[2 * (i + l)] = (a - dr) * gf;
                of[2 * (i + l) + 1] = (b - di) * gf;
                clip += (std::fabs(a) > 0.98f) | (std::fabs(b) > 0.98f);
            }
        }
        for (; i < m; i++) {
            const float a = xf[2 * i], b = xf[2 * i + 1];
            sr[0] += a; si[0] += b; sq[0] += a * a + b * b;
            of[2 * i] = (a - dr) * gf; of[2 * i + 1] = (b - di) * gf;
            clip += (std::fabs(a) > 0.98f) | (std::fabs(b) > 0.98f);
        }
        const double mr = ((double)sr[0] + sr[1] + sr[2] + sr[3]) / (double)m, mi = ((double)si[0] + si[1] + si[2] + si[3]) / (double)m;
        const double pw = ((double)sq[0] + sq[1] + sq[2] + sq[3]) / (double)m - mr * mr - mi * mi;
        const double dt = (double)m / rate;
        if (!dcInit) { dcI = mr; dcQ = mi; dcInit = true; }
        else { const double a = std::min(1.0, dt / 0.3); dcI += (mr - dcI) * a; dcQ += (mi - dcQ) * a; }
        const double rms = std::sqrt(std::max(pw, 0.0) / 2.0);
        levelRms = levelRms == 0 ? rms : levelRms + (rms - levelRms) * std::min(1.0, dt / 0.5);
        clipFrac += ((double)clip / (double)m - clipFrac) * std::min(1.0, dt / 1.0);
        const double g = 1.0 / std::max(levelRms, 1e-6);
        gain = (inSamples == 0) ? g : gain + (g - gain) * std::min(1.0, dt / 0.5);
        inSamples += (int64_t)m;
        const double c0 = cpuNow();
        if (gpsActive) gpsBand.process(blk.data(), m);
        if (bdsActive) bdsBand.process(blk.data(), m);
        if (gloActive) {
            if (gloUseWide) {
                gloWide.process(blk.data(), m);
                const int64_t from = std::max(gloWideFed, gloWide.base()), to = gloWide.end();
                if (to > from) for (int i = 0; i < kGloSlots; i++) if (gloSlotOn[i]) gloBand[i].process(gloWide.at(from), (size_t)(to - from));
                gloWideFed = to;
                gloWide.trimTo(to);
            } else {
                for (int i = 0; i < kGloSlots; i++) if (gloSlotOn[i]) gloBand[i].process(blk.data(), m);
            }
        }
        const double now = (double)inSamples / rate;
        const double c1 = cpuNow();
        runTrackers(now);
        const double c2 = cpuNow();
        runAcq(dt);
        const double c3 = cpuNow();
        cpuFront += (c1 - c0);
        cpuTrack += (c2 - c1);
        cpuAcq += (c3 - c2);
        trimBands();
        // measurements: a second apart, once every channel has run past the instant
        {
            double tEnd = 1e18;
            bool any = false;
            if (gpsActive && gpsBand.end() > 0) { tEnd = std::min(tEnd, gpsBand.timeOf((double)gpsBand.end())); any = true; }
            if (bdsActive && bdsBand.end() > 0) { tEnd = std::min(tEnd, bdsBand.timeOf((double)bdsBand.end())); any = true; }
            for (int i = 0; i < kGloSlots && gloActive; i++)
                if (gloSlotOn[i] && gloBand[i].end() > 0) { tEnd = std::min(tEnd, gloBand[i].timeOf((double)gloBand[i].end())); any = true; }
            if (any) while (tEnd - 0.08 > nextMeas) { measure(nextMeas); nextMeas += 1.0; }
        }
        if (now >= nextReport) { nextReport += 0.25; buildTelemetry(now); }
    }
    // the steps of the larger methods above (each defined in the file named in its comment)
    void replanGps(double center, unsigned mask);   // The GPS L1 C/A channel plan for the current tuning and rate
    void replanGal(double center, unsigned mask);   // Galileo E1-B shares the GPS band; its channels follow
    void replanBds(double center, unsigned mask);   // BeiDou B1I on its own carrier
    void replanGlo(double center, unsigned mask);   // GLONASS L1OF: the frequency channels whose lobe lies in the band
    void applyFix(double S, bool solved, GnssSolution& sl, std::vector<GnssObs>& obs, std::vector<GnssMeasurement>& meas, std::vector<size_t>& idx, double tr0);   // Takes a position solution (or its loss) into the fix, the per-system solution info and the clock estimate
    void telFix(GnssTelemetry& t, double signalNow, float best, uint64_t ok, uint64_t bad);   // The fix, clock, leap seconds and the level histories
    void telSatellites(GnssTelemetry& t);   // Satellite positions from the ephemerides, in the table of satellites
    void telSky(GnssTelemetry& t);   // The sky: satellites above the horizon by the ephemeris or the almanac, from the fix
    void telSearchAndStatus(GnssTelemetry& t, double signalNow, const std::vector<const GnssTracker*>& order);   // What the search is doing, the signal level, the status line and the interference monitor
};

} // namespace dect2
