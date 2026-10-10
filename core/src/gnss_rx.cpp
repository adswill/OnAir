// GNSS receiver: orchestrates the bands, the acquisition, the tracking channels, the navigation data base and the position solution.
#include "dect2/gnss_rx.h"
#include "dect2/gnss_acq.h"
#include "dect2/gnss_codes.h"
#include "dect2/gnss_front.h"
#include "dect2/gnss_nav.h"
#include "dect2/gnss_solve.h"
#include "dect2/gnss_track.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kGpsFsOut = 4.096e6;
constexpr int kGpsN = 4096;
constexpr int kMaxChannels = 24;
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

double cpuNow() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

std::string satName(int sys, int prn) { return gnssSatName(sys, prn); }

} // namespace

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
    uint64_t units = 0;
    double cpuFront = 0, cpuTrack = 0, cpuAcq = 0;

    // front end
    double dcI = 0, dcQ = 0, gain = 1, levelRms = 0;
    bool dcInit = false;
    double clipFrac = 0;
    int64_t inSamples = 0;
    std::vector<cf32> blk;
    std::vector<cf32> pend;          // input waiting for a full block
    // GPS band
    bool gpsActive = false;
    GnssBand gpsBand;
    GnssAcq gpsAcq;
    uint8_t gpsChips[kCaPrns][kGpsCaLen];
    GnssSignalSpec gpsSpec, qzssSpec, sbasSpec;
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
    void replan() {
        planGen = settingsGen.load();
        const double center = centerMhz.load() * 1e6;
        const unsigned mask = sysMask.load();
        bool wantGps = (mask & gnssSystemBit(GnssGps)) != 0;
        const double off = kGpsL1Hz - center;
        // the band must fit: the signal's main lobe (+-1.023 MHz) inside half the input rate, and the input must be fast enough
        const bool fits = rate >= 2.0e6 && std::fabs(off) + 1.0e6 <= 0.5 * rate;
        wantGps = wantGps && fits;
        if (wantGps) {
            if (!gpsActive || std::fabs(gpsBand.offsetHz() - off) > 1.0 || lastRate != rate || mask != gpsMaskNow) {
                gpsBand.init(rate, off, kGpsFsOut, (size_t)1 << 19);
                GnssAcqConfig ac;
                ac.sys = GnssGps; ac.fftLog2 = 12; ac.fsOut = kGpsFsOut; ac.blocks = kShortMs; ac.rfHz = kGpsL1Hz;
                // a false alarm costs little: the hit is looked for again (confirm) and a channel that does not lock gives up
                ac.pfa = 1e-3;
                const int q = (int)std::ceil(dopHalf.load() / 1000.0);
                ac.qMin = -q; ac.qMax = q - 1;
                // GPS first; QZSS and SBAS after it (they are searched once a GPS satellite is locked: see skipPrn)
                for (int p = 1; p <= 32; p++) ac.prns.push_back(p);
                if (mask & gnssSystemBit(GnssQzss)) for (int p = 193; p <= 202; p++) ac.prns.push_back(p);
                if (mask & gnssSystemBit(GnssSbas)) for (int p = 120; p <= 158; p++) ac.prns.push_back(p);
                ac.replica = [](int prn, cf32* out, int n) {
                    uint8_t c[kGpsCaLen];
                    if (!l1caChips(prn, c)) return false;
                    // sampled the way a receiver at 4.096 Msps sees the chips: the chip under the centre of each sample
                    for (int i = 0; i < n; i++) {
                        const int idx = (int)(((int64_t)i * 2 + 1) * kGpsCaLen / (2 * (int64_t)n));
                        out[i] = cf32(c[idx] ? -1.f : 1.f, 0.f);
                    }
                    return true;
                };
                gpsAcq.init(ac);
                stage = 0; stageRounds = 0;
                for (int p = 1; p < kCaPrns; p++) l1caChips(p, gpsChips[p]);
                gpsSpec.sys = GnssGps; gpsSpec.chipRate = kGpsCaChipRate; gpsSpec.codeLen = kGpsCaLen; gpsSpec.rfHz = kGpsL1Hz; gpsSpec.fsOut = kGpsFsOut; gpsSpec.halfSpacing = 2;
                qzssSpec = gpsSpec; qzssSpec.sys = GnssQzss;
                sbasSpec = gpsSpec; sbasSpec.sys = GnssSbas; sbasSpec.msg = GnssMsgSbas;
                trackers.clear();
                acqCredit = 0;
            }
            gpsActive = true;
            gpsMaskNow = mask;
        } else {
            gpsActive = false;
            trackers.clear();
        }
        lastRate = rate;
        tel.activeMask = gpsActive ? gnssSystemBit(GnssGps) : 0;
    }
    double lastRate = 0;
    unsigned gpsMaskNow = 0;
    // the systems searched or tracked now (GnssTelemetry::decodeMask)
    unsigned decodeMask() const {
        if (!gpsActive) return 0;
        return gpsMaskNow & (gnssSystemBit(GnssGps) | gnssSystemBit(GnssQzss) | gnssSystemBit(GnssSbas));
    }

    void resetState() {
        pend.clear();
        dcI = dcQ = 0; gain = 1; dcInit = false; levelRms = 0; clipFrac = 0; inSamples = 0;
        gpsBand.reset();
        gpsAcq.reset();
        trackers.clear();
        for (auto& s : sat) s = SatNav();
        iono = GpsIono(); utc = GpsUtc(); almToa = -1; almWna = -1; almGot = false;
        fix = GnssFix(); clockSet = false; hasPrior = false; nextMeas = 1.0; fixCount = 0; firstFix = -1; lastFixSignal = -1;
        okRetired = badRetired = 0; cfoEst = driftEst = 0; cfoValid = false; sbasTotal = 0;
        stage = 0; stageRounds = 0; firstLock = -1; roundHits = lastRoundHits = 0; longRound = false;
        for (auto& s : sol) s = SolInfo();
        nextReport = 0.25; acqCredit = 0; havePredicted = false;
        tel = GnssTelemetry(); telReady = false;
    }

    // ------------------------------------------------------------------ processing
    bool tracked(int prn) const {
        const int sys = l1caSystem(prn);
        for (auto& t : trackers) if (t->sys == sys && t->prn == prn) return true;
        return false;
    }
    bool gpsLocked() const {
        for (auto& t : trackers) if (t->sys == GnssGps && t->carrierLocked()) return true;
        return false;
    }

    void startFromHit(const GnssAcqHit& found) {
        if (trackers.size() >= (size_t)kMaxChannels || tracked(found.prn)) return;
        // the hit is old: look again in the newest samples
        GnssAcqHit h;
        units += (uint64_t)(6 * std::max(8, found.blocks));
        if (!gpsAcq.confirm(gpsBand, found.prn, found.dopplerHz, found.blocks, &h)) return;
        auto t = std::make_unique<GnssTracker>();
        const double rc = (kGpsCaChipRate + h.dopplerHz * kGpsCaChipRate / kGpsL1Hz) / kGpsFsOut;
        int64_t idx = gpsBand.end() - 3 * kGpsN;
        if (idx < gpsBand.base()) idx = gpsBand.base();
        double ph = std::fmod(((double)idx - ((double)h.segStart + h.codePhase)) * rc, (double)kGpsCaLen);
        if (ph < 0) ph += kGpsCaLen;
        const int sys = l1caSystem(h.prn);
        t->start(sys == GnssSbas ? sbasSpec : sys == GnssQzss ? qzssSpec : gpsSpec, gpsChips[h.prn], h.prn, h.dopplerHz, idx, ph);
        char b[120];
        snprintf(b, sizeof b, "%s found: Doppler %+.0f Hz, peak %.1f times the noise", satName(sys, h.prn).c_str(), h.dopplerHz, h.ratio);
        logf(b);
        trackers.push_back(std::move(t));
    }

    void handleSubframe(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow) {
        SatNav& s = sat[t.prn];
        if (ev.kind == GnssMsgSbas) {
            // SBAS: the message type is reported; the corrections are not applied
            const bool fresh = ev.sbasType < 32 ? !(s.sbasTypes & (1u << ev.sbasType)) : false;
            s.sbasLast = ev.sbasType; s.sbasCount++;
            if (ev.sbasType < 32) s.sbasTypes |= 1u << ev.sbasType;
            sbasTotal++;
            if (fresh) {
                char b[100];
                snprintf(b, sizeof b, "%s SBAS message type %d", satName(t.sys, t.prn).c_str(), ev.sbasType);
                logf(b);
            }
            return;
        }
        const unsigned id = lnavSubframeId(ev.sf);
        s.towLast = (int)ev.towStart;
        s.lastSfId = (int)id; s.lastSfTime = signalNow;
        if (id == 1 || id == 2 || id == 3) {
            if (s.pend.prn != t.prn) { s.pend = GpsEphemeris(); s.pend.prn = t.prn; }
            if (id == 1) { lnavParseSf1(ev.sf, s.pend); s.pendWeek = gpsResolveWeek(s.pend.wn, referenceWeek()); }
            else if (id == 2) lnavParseSf2(ev.sf, s.pend);
            else lnavParseSf3(ev.sf, s.pend);
            if (s.pend.complete() && s.pendWeek > 0 && (!s.hasEph || s.eph.iodc != s.pend.iodc || s.eph.toe != s.pend.toe)) {
                s.eph = s.pend; s.hasEph = true; s.week = s.pendWeek; s.ephSignalTime = signalNow;
                char b[120];
                snprintf(b, sizeof b, "%s ephemeris IODE %d, week %d, health %s", satName(t.sys, t.prn).c_str(), s.eph.iode(), s.week, s.eph.health ? "bad" : "ok");
                logf(b);
            }
        } else if ((id == 4 || id == 5) && t.sys == GnssGps) {     // the QZSS pages of subframes 4 and 5 have their own layout (not decoded)
            GpsAlmanac a; int sv = 0; double toa = 0; int wna = 0;
            GpsIono io; GpsUtc ut;
            const int r = lnavParseAlmanacPage(ev.sf, (int)id, &sv, a, io, ut, &toa, &wna);
            if (r == 1 && a.valid) sat[a.prn].alm = a;
            else if (r == 2) {
                if (!iono.valid) logf("ionosphere and UTC parameters received");
                iono = io; utc = ut;
            } else if (r == 3) { almToa = toa; almWna = wna; almGot = true; }
        }
    }

    void runTrackers(double signalNow) {
        if (!gpsActive) return;
        for (auto& t : trackers) {
            const bool wasLocked = t->carrierLocked(), wasBit = t->bitSynced(), wasFrame = t->frameSynced();
            const uint64_t okBefore = t->framesOk();
            while (t->step(gpsBand)) {}
            if (t->carrierLocked() && !wasLocked) {
                char b[100]; snprintf(b, sizeof b, "%s locked, %.0f dB-Hz", satName(t->sys, t->prn).c_str(), t->cn0()); logf(b);
                if (firstLock < 0) firstLock = signalNow;
            }
            if (t->bitSynced() && !wasBit) logf(satName(t->sys, t->prn) + " bit sync");
            if (t->frameSynced() && !wasFrame) logf(satName(t->sys, t->prn) + " frame sync");
            (void)okBefore;
            for (auto& ev : t->events()) handleSubframe(*t, ev, signalNow);
            t->events().clear();
        }
        for (size_t i = 0; i < trackers.size();) {
            if (trackers[i]->lost()) {
                okRetired += trackers[i]->framesOk(); badRetired += trackers[i]->framesBad();
                // a lost satellite is found again sooner by a short round than at the end of a long one
                if (longRound && trackers[i]->lockSeconds() > 0.5) { gpsAcq.endRound(); longRound = false; }
                logf(satName(trackers[i]->sys, trackers[i]->prn) + (trackers[i]->wasPullInTimeout() ? " not confirmed" : " lost"));
                trackers.erase(trackers.begin() + (long)i);
            } else i++;
        }
    }

    bool skipPrn(int prn) const {
        if (tracked(prn)) return true;
        // QZSS and SBAS once a GPS satellite gives the frequency window; the 39 SBAS codes every other round only
        if (prn >= 193) return !gpsLocked();
        if (prn > 32) return !gpsLocked() || gpsAcq.rounds() % 2 == 0;
        // with a fix and the sky predicted from the almanac, look for the satellites that should be in view; the others now and then
        if (havePredicted && gpsAcq.rounds() % 6 != 5 && std::find(predictedVisible.begin(), predictedVisible.end(), prn) == predictedVisible.end()) return true;
        return false;
    }

    // Where and how long to search next. With satellites locked the radio's error lies within 5.5 kHz of each of their Dopplers, so the others are within
    // 11 kHz of all of them; with a fix it is measured. With nothing locked the window widens round by round (TCXO, HackRF, cheap dongle), then the
    // same with a four times longer integration for weak signals, and starts over.
    void planSearch() {
        double lo = 1e18, hi = -1e18;
        int nLocked = 0, nPull = 0;
        for (auto& t : trackers) {
            if (t->carrierLocked()) { lo = std::min(lo, t->dopplerHz()); hi = std::max(hi, t->dopplerHz()); nLocked++; }
            else nPull++;
        }
        const uint32_t r = gpsAcq.rounds();
        double c = 0, h = dopHalf.load();
        int ms = kShortMs;
        const bool newRound = r != stageRounds;
        if (newRound) { lastRoundHits = roundHits; roundHits = 0; }
        if (nLocked > 0) {
            if (fix.valid && cfoValid) { c = cfoEst; h = kSatDopplerHz + 1000.0; }
            else { c = 0.5 * (lo + hi); h = std::max(kSatDopplerHz, 2 * kSatDopplerHz - 0.5 * (hi - lo)) + 500.0; }
            lastCfo = c; lastCfoValid = true;
            stage = 0; stageRounds = r;
            // a long round for the weak satellites once a short one finds nothing new (a short round finds the strong ones, and the lost ones again, sooner)
            if (newRound) longRound = !longRound && lastRoundHits == 0;
            if (longRound) ms = kLongMs;
        } else {
            longRound = false;
            if (newRound) {
                stageRounds = r;
                if (nPull == 0) {
                    stage = (stage + 1) % 6;
                    static const char* what[6] = {"", "+-45 kHz (a radio without a TCXO, such as a HackRF)", "+-170 kHz (a dongle without a TCXO)",
                                                   "with a 64 ms integration for weak signals", "+-45 kHz with a 64 ms integration", "+-170 kHz with a 64 ms integration"};
                    if (stage > 0) logf(std::string("nothing found yet: searching ") + what[stage]);
                }
            }
            const int st = stage % 3;
            if (st == 0) {
                // the frequency error that worked before (this run, or the one remembered for this radio)
                if (lastCfoValid) c = lastCfo; else if (cfoHintValid.load()) c = cfoHintHz.load();
            } else h = st == 1 ? kMidHz : kWideHz;
            if (stage >= 3) ms = kLongMs;
        }
        gpsAcq.setPlan(c, h, ms);
    }

    void runAcq(double seconds) {
        if (!gpsActive) return;
        planSearch();
        int nLocked = 0;
        for (auto& t : trackers) nLocked += t->carrierLocked();
        // search less when the satellites expected in view are all tracked; harder while nothing is tracked (the channels cost nothing then)
        const double rateNow = (double)acqRate.load() * (nLocked == 0 ? 2.0 : 1.0);
        acqCredit = std::min(acqCredit + seconds * 1000.0 * rateNow, 600.0);
        if (acqCredit < 8) return;
        std::vector<GnssAcqHit> hits;
        const int used = gpsAcq.work(gpsBand, (int)acqCredit, [this](int p) { return skipPrn(p); }, hits);
        acqCredit -= used;
        units += (uint64_t)used;
        roundHits += (int)hits.size();
        for (auto& h : hits) startFromHit(h);
    }

    void trimBands() {
        if (!gpsActive) return;
        int64_t keepFrom = gpsBand.end();
        for (auto& t : trackers) keepFrom = std::min(keepFrom, t->position());
        gpsBand.trimTo(keepFrom - 8192);
    }

    // ------------------------------------------------------------------ the fix
    void measure(double S) {
        std::vector<GnssObs> obs;
        std::vector<GnssMeasurement> meas;
        std::vector<int> prns;
        std::vector<double> towv;
        for (auto& t : trackers) {
            // QZSS keeps GPS time and sends the GPS navigation message: it joins the GPS satellites (IS-QZSS-PNT); SBAS is not used for ranging
            if ((t->sys != GnssGps && t->sys != GnssQzss) || !t->carrierLocked() || !t->timeValid()) continue;
            double tow;
            if (!t->transmitTime(S, &tow)) continue;
            GnssMeasurement m;
            m.sys = t->sys; m.prn = t->prn; m.rxTime = S; m.txTow = tow; m.dopplerHz = t->dopplerHz(); m.cn0 = t->cn0();
            meas.push_back(m);
        }
        // satellites with a usable ephemeris for the solution
        double tRef = 0;
        int n = 0;
        for (auto& m : meas) { const SatNav& s = sat[m.prn]; if (s.hasEph && s.eph.health == 0 && m.cn0 > 24.0) { tRef += m.txTow; n++; } }
        if (n >= 1) tRef /= n;
        // average in a way that survives the week boundary: the times are within 20 ms of each other, so a plain mean is fine except at the wrap
        const double tr0 = std::round((tRef + 0.075) * 1e6) / 1e6;
        std::vector<size_t> idx;
        for (size_t i = 0; i < meas.size(); i++) {
            const SatNav& s = sat[meas[i].prn];
            if (!s.hasEph || s.eph.health != 0 || meas[i].cn0 < 24.0) continue;
            double tTrue = meas[i].txTow, pos[3], dts = 0;
            for (int k = 0; k < 3; k++) {
                gpsEphemerisState(s.eph, tTrue, pos, &dts);
                tTrue = meas[i].txTow - (dts - s.eph.tgd);
            }
            GnssObs o;
            o.sys = GnssGps; o.prn = meas[i].prn;          // QZSS too: one clock with GPS
            o.pr = kC * (gpsWrap(tr0 - meas[i].txTow) + (dts - s.eph.tgd));
            o.sat[0] = pos[0]; o.sat[1] = pos[1]; o.sat[2] = pos[2];
            o.weight = std::min(2.0, std::max(0.05, std::pow(10.0, (meas[i].cn0 - 45.0) / 10.0)));
            obs.push_back(o);
            idx.push_back(i);
        }
        for (auto& s : sol) s = SolInfo();
        bool solved = false;
        GnssSolution sl;
        if (obs.size() >= 4) {
            GnssSolveOptions opt;
            opt.hasIono = iono.valid; opt.iono = iono; opt.tow = tr0;
            opt.hasPrior = hasPrior;
            for (int k = 0; k < 3; k++) opt.prior[k] = prior[k];
            opt.maskDeg = elMask.load();
            solved = gnssSolve(obs, opt, sl);
            if (solved) {
                double lat, lon, h;
                ecefToLla(sl.x, &lat, &lon, &h);
                // sanity: on or near the earth, residuals not absurd
                if (!(h > -1000 && h < 30000) || sl.rmsResidual > 200.0) solved = false;
            }
        }
        for (size_t k = 0; k < obs.size(); k++) {
            if (!solved) break;
            SolInfo& si = sol[obs[k].prn];
            si.used = sl.used[k] != 0; si.resid = (float)sl.residual[k]; si.az = (float)sl.azDeg[k]; si.el = (float)sl.elDeg[k]; si.has = true;
            meas[idx[k]].inFix = si.used; meas[idx[k]].residualM = sl.residual[k];
        }
        if (solved) {
            fix.valid = true;
            for (int k = 0; k < 3; k++) { fix.ecef[k] = sl.x[k]; prior[k] = sl.x[k]; }
            hasPrior = true;
            ecefToLla(sl.x, &fix.latDeg, &fix.lonDeg, &fix.heightM);
            // the receiver's clock is the signal time plus an offset set at the first fix; its error against GPS time then grows with the oscillator error
            const double tGpsMeas = tr0 - sl.bias[GnssGps] / kC;
            if (!clockSet) { clockOffset = tGpsMeas - S; clockSet = true; }
            const double gpsBias = kC * ((S + clockOffset) - tGpsMeas);
            for (int s = 0; s < GnssSystems; s++) {
                fix.systemInFix[s] = sl.sysUsed[s];
                fix.clockBiasM[s] = sl.sysUsed[s] ? gpsBias + (sl.bias[s] - sl.bias[GnssGps]) : 0.0;
            }
            fix.nSats = sl.nUsed;
            for (int s = 0; s < GnssSystems; s++) fix.nSatsPerSystem[s] = 0;
            for (size_t k = 0; k < obs.size(); k++) if (sl.used[k]) fix.nSatsPerSystem[meas[idx[k]].sys]++;
            if (fix.nSatsPerSystem[GnssQzss] > 0) { fix.systemInFix[GnssQzss] = true; fix.clockBiasM[GnssQzss] = fix.clockBiasM[GnssGps]; }
            fix.hdop = sl.hdop; fix.vdop = sl.vdop; fix.pdop = sl.pdop; fix.tdop = sl.tdop;
            fix.hErrM = sl.hErrM;
            fix.vErrM = sl.vErrM;
            fix.residualRmsM = (float)sl.rmsResidual;
            std::string names;
            for (int s = 0; s < GnssSystems; s++) if (fix.nSatsPerSystem[s] > 0) names += std::string(names.empty() ? "" : " + ") + gnssSystemName(s);
            char b[96];
            snprintf(b, sizeof b, "%s %d satellites", names.c_str(), sl.nUsed);
            fix.type = b;
            // GPS time of the measurement: the reference time less the clock error
            const double tGps = tr0 - sl.bias[GnssGps] / kC;
            int week = 0;
            for (auto& m : meas) if (m.sys == GnssGps && sat[m.prn].hasEph) { week = sat[m.prn].week; break; }
            if (week == 0) for (auto& m : meas) if (sat[m.prn].hasEph) { week = sat[m.prn].week; break; }
            double tow = tGps;
            if (tow < 0) { tow += 604800.0; week--; }
            if (tow >= 604800.0) { tow -= 604800.0; week++; }
            fix.gpsWeek = week; fix.gpsTow = tow;
            fix.leapSeconds = utc.valid ? utc.dtls : -1;
            fix.timeValid = week > 0;
            if (fix.timeValid) gpsTimeToCalendar(week, tow, utc.valid ? utc.dtls : 18, &fix.year, &fix.month, &fix.day, &fix.hour, &fix.minute, &fix.second);
            fix.fixCount = (uint32_t)(++fixCount);
            lastFixSignal = S;
            if (firstFix < 0) {
                firstFix = S;
                char m[160];
                snprintf(m, sizeof m, "first fix after %.1f s: %.5f %.5f, %.0f m, %d satellites, HDOP %.1f", S, fix.latDeg, fix.lonDeg, fix.heightM, fix.nSats, fix.hdop);
                logf(m);
            }
            fix.firstFixSecs = firstFix;
            estimateClock(sl, obs, meas, idx, tr0);
            predictSky(tGps);
        } else if (fix.valid && S - lastFixSignal > 20.0) {
            fix.valid = false; fix.type = "no fix";
            logf("position fix lost");
        }
        lastMeas = meas;
        if (measCb) measCb(meas);
    }

    // The receiver's own frequency error from the Doppler of the satellites: measured minus what the geometry says, and the clock drift
    void estimateClock(const GnssSolution& sl, const std::vector<GnssObs>& obs, const std::vector<GnssMeasurement>& meas, const std::vector<size_t>& idx, double tr0) {
        std::vector<double> res;
        for (size_t k = 0; k < obs.size(); k++) {
            if (!sl.used[k]) continue;
            const SatNav& s = sat[obs[k].prn];
            const GnssMeasurement& m = meas[idx[k]];
            double tTx = m.txTow;
            double p1[3], p2[3], c1, c2;
            gpsEphemerisState(s.eph, tTx - 0.25, p1, &c1);
            gpsEphemerisState(s.eph, tTx + 0.25, p2, &c2);
            const double r1 = std::sqrt((p1[0] - sl.x[0]) * (p1[0] - sl.x[0]) + (p1[1] - sl.x[1]) * (p1[1] - sl.x[1]) + (p1[2] - sl.x[2]) * (p1[2] - sl.x[2]));
            const double r2 = std::sqrt((p2[0] - sl.x[0]) * (p2[0] - sl.x[0]) + (p2[1] - sl.x[1]) * (p2[1] - sl.x[1]) + (p2[2] - sl.x[2]) * (p2[2] - sl.x[2]));
            const double rr = (r2 - r1) / 0.5;
            // the satellite clock rate in the transmitted frequency is negligible here (a few mHz)
            const double expected = -rr / kC * kGpsL1Hz;
            res.push_back(m.dopplerHz - expected);
        }
        (void)tr0;
        if (res.size() >= 3) {
            std::sort(res.begin(), res.end());
            const double med = res[res.size() / 2];
            cfoEst = med;
            cfoValid = true;
            driftEst = -med / kGpsL1Hz * kC;
        }
    }

    void predictSky(double tow) {
        predictedVisible.clear();
        havePredicted = true;
        for (int p = 1; p <= 32; p++) {
            double pos[3];
            bool ok = false;
            if (sat[p].hasEph) { gpsEphemerisState(sat[p].eph, tow, pos, nullptr); ok = true; }
            else if (sat[p].alm.valid) { gpsAlmanacPos(sat[p].alm, tow, pos); ok = true; }
            if (!ok) continue;
            double az, el;
            azElFromEcef(fix.ecef, pos, &az, &el);
            if (el > -2.0) predictedVisible.push_back(p);
        }
        gpsAcq.setPriority(predictedVisible);
    }

    // ------------------------------------------------------------------ telemetry
    void buildTelemetry(double signalNow) {
        GnssTelemetry t;
        t.seq = ++seq;
        t.inputRate = rate;
        t.centerMhz = centerMhz.load();
        t.systemsMask = sysMask.load() & (gnssSystemBit(GnssGps) | gnssSystemBit(GnssQzss) | gnssSystemBit(GnssSbas));
        t.activeMask = gpsActive ? 1u : 0u;
        t.decodeMask = decodeMask();
        t.sbasMessages = sbasTotal;
        t.signalSecs = signalNow;
        struct Row { const GnssTracker* t; };
        std::vector<const GnssTracker*> order;
        for (auto& tr : trackers) order.push_back(tr.get());
        std::sort(order.begin(), order.end(), [](const GnssTracker* a, const GnssTracker* b) { return a->cn0() > b->cn0(); });
        float best = 0;
        uint64_t ok = okRetired, bad = badRetired;
        for (auto* tr : order) {
            GnssChannel c;
            c.sys = tr->sys; c.prn = tr->prn;
            int st = tr->state();
            const SatNav& s = sat[tr->prn];
            if (st >= GnssChFrameSync && s.hasEph) st = GnssChEphemeris;
            c.state = st;
            c.cn0 = tr->cn0();
            c.dopplerHz = tr->dopplerHz();
            c.codePhase = tr->codePhase();
            const SolInfo& si = sol[tr->prn];
            if (fix.valid && s.hasEph) {
                double pos[3], az, el;
                gpsEphemerisState(s.eph, fix.gpsTow, pos, nullptr);
                azElFromEcef(fix.ecef, pos, &az, &el);
                c.hasAzEl = true; c.azDeg = (float)az; c.elDeg = (float)el;
            } else if (si.has) { c.hasAzEl = true; c.azDeg = si.az; c.elDeg = si.el; }
            c.used = si.used && fix.valid;
            c.residualM = si.used ? si.resid : 0.f;
            c.health = s.hasEph ? (s.eph.health ? 1 : 0) : (s.alm.valid ? (s.alm.health ? 1 : 0) : -1);
            c.framesOk = tr->framesOk(); c.framesBad = tr->framesBad();
            c.lockSecs = (float)tr->lockSeconds();
            c.cn0Hist = tr->cn0History();
            if (tr->carrierLocked()) best = std::max(best, c.cn0);
            ok += tr->framesOk(); bad += tr->framesBad();
            t.channels.push_back(c);
            if (tr->carrierLocked()) t.nTracked++;
            GnssNavInfo ni;
            ni.sys = tr->sys; ni.prn = tr->prn;
            ni.hasEphemeris = s.hasEph;
            if (s.hasEph) {
                ni.iode = s.eph.iode();
                ni.health = s.eph.health ? 1 : 0;
                ni.week = s.week;
                ni.svClockBiasUs = (float)(s.eph.af0 * 1e6);
                if (fix.valid) ni.ephAgeS = (float)gpsWrap(fix.gpsTow - s.eph.toe);
            }
            ni.hasAlmanac = s.alm.valid;
            ni.towS = s.towLast;
            ni.sbasLastType = s.sbasLast; ni.sbasMessages = s.sbasCount; ni.sbasTypesSeen = s.sbasTypes;
            // subframes 1, 2, 3 come every 30 s, 6 s apart: the time left is until the last missing one has arrived
            if (s.hasEph) { ni.ephParts = 3; ni.ephEtaS = 0; }
            else if (tr->sys == GnssSbas) { ni.ephParts = 0; ni.ephEtaS = -1; }
            else {
                const bool has[4] = {false, s.pend.prn == tr->prn && s.pend.has1, s.pend.prn == tr->prn && s.pend.has2, s.pend.prn == tr->prn && s.pend.has3};
                ni.ephParts = (int)has[1] + (int)has[2] + (int)has[3];
                if (s.lastSfTime >= 0 && s.lastSfId >= 1 && tr->frameSynced()) {
                    double wait = 0;
                    for (int j = 1; j <= 3; j++) if (!has[j]) { int k = (j - s.lastSfId + 5) % 5; if (k == 0) k = 5; wait = std::max(wait, 6.0 * k); }
                    ni.ephEtaS = (float)std::max(0.0, wait - (signalNow - s.lastSfTime));
                }
            }
            t.nav.push_back(ni);
        }
        t.blocksOk = ok; t.blocksBad = bad;
        t.snrDb = best;
        t.fix = fix;
        t.dataValid = fix.valid;
        t.state = fix.valid ? 2 : (t.nTracked > 0 ? 1 : 0);
        t.cfoHz = cfoEst;
        t.fix.clockDriftMps = driftEst;
        t.ionoValid = iono.valid; t.utcValid = utc.valid; t.leapSeconds = utc.valid ? utc.dtls : -1;
        for (int i = 0; i < 4; i++) { t.ionoAlpha[i] = (float)iono.alpha[i]; t.ionoBeta[i] = (float)iono.beta[i]; }
        int na = 0;
        for (int p = 1; p <= 32; p++) if (sat[p].alm.valid) na++;
        t.almanacGps = na;
        // the sky: satellites above the horizon by the ephemeris or the almanac, from the fix
        if (fix.valid) {
            for (int p = 1; p <= 32 && t.sky.size() < 64; p++) {
                double pos[3];
                bool fromEph = false;
                if (sat[p].hasEph) { gpsEphemerisState(sat[p].eph, fix.gpsTow, pos, nullptr); fromEph = true; }
                else if (sat[p].alm.valid) gpsAlmanacPos(sat[p].alm, fix.gpsTow, pos);
                else continue;
                double az, el;
                azElFromEcef(fix.ecef, pos, &az, &el);
                if (el < 0) continue;
                GnssSky sk;
                sk.sys = GnssGps; sk.prn = p; sk.azDeg = (float)az; sk.elDeg = (float)el; sk.fromEphemeris = fromEph;
                for (auto& tr : trackers) if (tr->sys == GnssGps && tr->prn == p) { sk.tracked = tr->carrierLocked(); sk.cn0 = sk.tracked ? tr->cn0() : 0.f; }
                sk.used = sol[p].used;
                sk.health = sat[p].hasEph ? (sat[p].eph.health ? 1 : 0) : (sat[p].alm.health ? 1 : 0);
                t.sky.push_back(sk);
            }
            // QZSS from its ephemeris (SBAS: the geostationary position is in message type 9, not decoded, so it has no place in the sky plot)
            for (int p = 193; p <= 202 && t.sky.size() < 64; p++) {
                if (!sat[p].hasEph) continue;
                double pos[3], az, el;
                gpsEphemerisState(sat[p].eph, fix.gpsTow, pos, nullptr);
                azElFromEcef(fix.ecef, pos, &az, &el);
                if (el < 0) continue;
                GnssSky sk;
                sk.sys = GnssQzss; sk.prn = p; sk.azDeg = (float)az; sk.elDeg = (float)el; sk.fromEphemeris = true;
                for (auto& tr : trackers) if (tr->sys == GnssQzss && tr->prn == p) { sk.tracked = tr->carrierLocked(); sk.cn0 = sk.tracked ? tr->cn0() : 0.f; }
                sk.used = sol[p].used;
                sk.health = sat[p].eph.health ? 1 : 0;
                t.sky.push_back(sk);
            }
        }
        // search
        t.searching = gpsActive;
        t.searchSys = gpsAcq.currentPrn() > 0 ? l1caSystem(gpsAcq.currentPrn()) : GnssGps; t.searchPrn = gpsAcq.currentPrn();
        t.searchProgress = gpsAcq.progress(); t.searchRounds = gpsAcq.rounds();
        t.acqSys = gpsAcq.lastPrn() > 0 ? l1caSystem(gpsAcq.lastPrn()) : GnssGps; t.acqPrn = gpsAcq.lastPrn(); t.acqDopplerHz = (float)gpsAcq.lastDoppler(); t.acqPeakToNoise = gpsAcq.lastRatio();
        t.acqCorr = gpsAcq.lastCorr(); t.acqPeakIndex = gpsAcq.lastPeakIndex();
        t.searchCenterHz = (float)gpsAcq.windowCenterHz(); t.searchHalfHz = (float)gpsAcq.windowHalfHz(); t.searchMs = gpsAcq.blocksNow();
        t.searchStage = t.nTracked > 0 ? 0 : stage;
        for (auto& tr : trackers) t.nPullIn += !tr->carrierLocked();
        t.firstLockSecs = firstLock;
        // scatter of the strongest locked channel
        for (auto* tr : order) {
            if (!tr->carrierLocked() || tr->scatter().size() < 20) continue;
            t.scatterSys = tr->sys; t.scatterPrn = tr->prn;
            float mx = 1e-9f;
            for (auto& pr : tr->scatter()) mx = std::max(mx, std::max(std::fabs(pr.first), std::fabs(pr.second)));
            for (auto& pr : tr->scatter()) { t.scatterI.push_back(pr.first / mx); t.scatterQ.push_back(pr.second / mx); }
            break;
        }
        t.levelDbfs = (float)(20 * std::log10(std::max(levelRms, 1e-6)));
        t.clipPercent = (float)(clipFrac * 100.0);
        t.dcI = (float)dcI; t.dcQ = (float)dcQ;
        char b[120];
        snprintf(b, sizeof b, "%d tracked, %d with ephemeris, %s", t.nTracked, (int)std::count_if(sat + 1, sat + kCaPrns, [](const SatNav& s) { return s.hasEph; }), fix.valid ? fix.type.c_str() : "no fix");
        t.status = b;
        std::lock_guard<std::mutex> lk(telMu);
        tel = std::move(t);
        telReady = true;
    }

    // ------------------------------------------------------------------ one block of input
    void processBlock(const cf32* x, size_t m) {
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
        if (gpsActive && gpsBand.end() > 0) {
            const double tEnd = gpsBand.timeOf((double)gpsBand.end());
            while (tEnd - 0.08 > nextMeas) { measure(nextMeas); nextMeas += 1.0; }
        }
        if (now >= nextReport) { nextReport += 0.25; buildTelemetry(now); }
    }
};

GnssReceiver::GnssReceiver() : p_(std::make_unique<Impl>()) {}
GnssReceiver::~GnssReceiver() = default;

void GnssReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->rate = inputRateHz;
    p_->rateAt = inputRateHz;
    p_->resetState();
    p_->lastRate = 0;
    p_->replan();
}
bool GnssReceiver::ready() const { return p_->rateAt.load() >= gnssTuning().minSampleRate - 1; }

void GnssReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->resetState();
    p_->lastRate = 0;
    if (p_->rate > 0) p_->replan();
}

void GnssReceiver::feed(const cf32* x, size_t n) {
    Impl& p = *p_;
    std::lock_guard<std::mutex> lk(p.procMu);
    if (p.rate <= 0 || n == 0) return;
    if (p.planGen != p.settingsGen.load()) p.replan();
    // The input is cut into blocks of a fixed size whatever the size of the chunks it arrives in: the result does not depend on the chunking
    size_t i = 0;
    if (!p.pend.empty()) {
        const size_t take = std::min(kBlock - p.pend.size(), n);
        p.pend.insert(p.pend.end(), x, x + take);
        i = take;
        if (p.pend.size() == kBlock) { p.processBlock(p.pend.data(), kBlock); p.pend.clear(); }
    }
    while (n - i >= kBlock) { p.processBlock(x + i, kBlock); i += kBlock; }
    if (i < n) p.pend.insert(p.pend.end(), x + i, x + n);
}

bool GnssReceiver::telemetry(GnssTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (!p_->telReady || p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void GnssReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->log = std::move(cb);
}
void GnssReceiver::setMeasurementCallback(std::function<void(const std::vector<GnssMeasurement>&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->measCb = std::move(cb);
}
void GnssReceiver::setCenterMhz(double mhz) { p_->centerMhz = mhz; p_->settingsGen++; }
void GnssReceiver::setSystems(unsigned mask) { p_->sysMask = mask; p_->settingsGen++; }
void GnssReceiver::setWeekReference(int w) { p_->weekRef = w; }
void GnssReceiver::setSearchRange(double hz) { p_->dopHalf = hz; p_->settingsGen++; p_->lastRate = 0; }
void GnssReceiver::setFrequencyHint(double hz, bool valid) { p_->cfoHintHz = hz; p_->cfoHintValid = valid; }
void GnssReceiver::setAcquisitionRate(int u) { p_->acqRate = std::max(1, u); }
void GnssReceiver::setApproxPosition(double lat, double lon, bool valid) { p_->hintLat = lat; p_->hintLon = lon; p_->hintValid = valid; }
void GnssReceiver::setElevationMask(double deg) { p_->elMask = deg; }
bool GnssReceiver::getEphemeris(int prn, GpsEphemeris& e, int* week) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (prn < 1 || prn > 32 || !p_->sat[prn].hasEph) return false;
    e = p_->sat[prn].eph;
    if (week) *week = p_->sat[prn].week;
    return true;
}
bool GnssReceiver::getEphemerisOf(int sys, int prn, GpsEphemeris& e, int* week) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (prn < 1 || prn >= kCaPrns || l1caSystem(prn) != sys || !p_->sat[prn].hasEph) return false;
    e = p_->sat[prn].eph;
    if (week) *week = p_->sat[prn].week;
    return true;
}
bool GnssReceiver::getAlmanac(int prn, GpsAlmanac& a) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (prn < 1 || prn > 32 || !p_->sat[prn].alm.valid) return false;
    a = p_->sat[prn].alm;
    return true;
}
bool GnssReceiver::getIonoUtc(GpsIono& i, GpsUtc& u) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    i = p_->iono; u = p_->utc;
    return p_->iono.valid;
}
uint64_t GnssReceiver::workUnitsUsed() const { return p_->units; }
void GnssReceiver::cpuBreakdown(double* front, double* track, double* acq) const { *front = p_->cpuFront; *track = p_->cpuTrack; *acq = p_->cpuAcq; }

ModeTuning gnssTuning() {
    ModeTuning t;
    t.stdMode = 14; t.id = "gnss"; t.name = "GNSS";
    t.minMhz = 1550; t.maxMhz = 1610; t.defMhz = 1575.42;
    t.sampleRate = 4e6;          // the C/A main lobe is 2.046 MHz wide; 4 Msps is the lowest comfortable rate for GPS alone (BeiDou with it needs 20 Msps near 1568 MHz, GLONASS 10 Msps near 1602 MHz)
    t.basebandHz = 2.5e6;
    t.bandwidthMhz = 2.046;
    t.minSampleRate = 2.046e6;
    return t;
}

} // namespace dect2
