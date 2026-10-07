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

struct SatNav {
    GpsEphemeris eph;            // the complete, consistent ephemeris
    bool hasEph = false;
    int week = 0;                // full week of the ephemeris (from subframe 1)
    GpsEphemeris pend;           // being collected
    int pendWeek = 0;
    GpsAlmanac alm;
    double ephSignalTime = 0;    // signal time when it was received
    int towLast = -1;
};

double cpuNow() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

std::string satName(int sys, int prn) {
    char b[16];
    snprintf(b, sizeof b, "%c%02d", gnssSystemLetter(sys), prn);
    return b;
}

} // namespace

struct GnssReceiver::Impl {
    std::mutex procMu, telMu;
    std::atomic<double> centerMhz{1575.42};
    std::atomic<unsigned> sysMask{0xFFFFFFFFu};
    std::atomic<int> weekRef{0};
    std::atomic<double> dopHalf{8000.0};
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
    uint8_t gpsChips[33][kGpsCaLen];
    GnssSignalSpec gpsSpec;
    double acqCredit = 0;
    std::vector<std::unique_ptr<GnssTracker>> trackers;
    // navigation data
    SatNav sat[33];
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
    double cfoEst = 0, driftEst = 0;
    std::vector<GnssMeasurement> lastMeas;
    std::vector<int> predictedVisible;
    bool havePredicted = false;
    // per-satellite last report of the fix
    struct SolInfo { bool used = false; float resid = 0; float az = 0, el = 0; bool has = false; };
    SolInfo sol[33];
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
            if (!gpsActive || std::fabs(gpsBand.offsetHz() - off) > 1.0 || lastRate != rate) {
                gpsBand.init(rate, off, kGpsFsOut, (size_t)1 << 19);
                GnssAcqConfig ac;
                ac.sys = GnssGps; ac.fftLog2 = 12; ac.fsOut = kGpsFsOut; ac.blocks = 16;
                const int q = (int)std::ceil(dopHalf.load() / 1000.0);
                ac.qMin = -q; ac.qMax = q - 1;
                for (int p = 1; p <= 32; p++) ac.prns.push_back(p);
                ac.replica = [](int prn, cf32* out, int n) {
                    uint8_t c[kGpsCaLen];
                    if (!gpsCaChips(prn, c)) return false;
                    // sampled the way a receiver at 4.096 Msps sees the chips: the chip under the centre of each sample
                    for (int i = 0; i < n; i++) {
                        const int idx = (int)(((int64_t)i * 2 + 1) * kGpsCaLen / (2 * (int64_t)n));
                        out[i] = cf32(c[idx] ? -1.f : 1.f, 0.f);
                    }
                    return true;
                };
                gpsAcq.init(ac);
                for (int p = 1; p <= 32; p++) gpsCaChips(p, gpsChips[p]);
                gpsSpec.sys = GnssGps; gpsSpec.chipRate = kGpsCaChipRate; gpsSpec.codeLen = kGpsCaLen; gpsSpec.rfHz = kGpsL1Hz; gpsSpec.fsOut = kGpsFsOut; gpsSpec.halfSpacing = 2;
                trackers.clear();
                acqCredit = 0;
            }
            gpsActive = true;
        } else {
            gpsActive = false;
            trackers.clear();
        }
        lastRate = rate;
        tel.activeMask = gpsActive ? gnssSystemBit(GnssGps) : 0;
    }
    double lastRate = 0;

    void resetState() {
        pend.clear();
        dcI = dcQ = 0; gain = 1; dcInit = false; levelRms = 0; clipFrac = 0; inSamples = 0;
        gpsBand.reset();
        gpsAcq.reset();
        trackers.clear();
        for (auto& s : sat) s = SatNav();
        iono = GpsIono(); utc = GpsUtc(); almToa = -1; almWna = -1; almGot = false;
        fix = GnssFix(); clockSet = false; hasPrior = false; nextMeas = 1.0; fixCount = 0; firstFix = -1; lastFixSignal = -1;
        okRetired = badRetired = 0; cfoEst = driftEst = 0;
        for (auto& s : sol) s = SolInfo();
        nextReport = 0.25; acqCredit = 0; havePredicted = false;
        tel = GnssTelemetry(); telReady = false;
    }

    // ------------------------------------------------------------------ processing
    bool tracked(int prn) const {
        for (auto& t : trackers) if (t->sys == GnssGps && t->prn == prn) return true;
        return false;
    }

    void startFromHit(const GnssAcqHit& found) {
        if (trackers.size() >= (size_t)kMaxChannels || tracked(found.prn)) return;
        // the hit is old: look again in the newest samples
        GnssAcqHit h;
        units += 48;
        if (!gpsAcq.confirm(gpsBand, found.prn, found.dopplerHz, &h)) return;
        auto t = std::make_unique<GnssTracker>();
        const double rc = (kGpsCaChipRate + h.dopplerHz * kGpsCaChipRate / kGpsL1Hz) / kGpsFsOut;
        int64_t idx = gpsBand.end() - 3 * kGpsN;
        if (idx < gpsBand.base()) idx = gpsBand.base();
        double ph = std::fmod(((double)idx - ((double)h.segStart + h.codePhase)) * rc, (double)kGpsCaLen);
        if (ph < 0) ph += kGpsCaLen;
        t->start(gpsSpec, gpsChips[h.prn], h.prn, h.dopplerHz, idx, ph);
        char b[120];
        snprintf(b, sizeof b, "%s found: Doppler %+.0f Hz, peak %.1f times the noise", satName(GnssGps, h.prn).c_str(), h.dopplerHz, h.ratio);
        logf(b);
        trackers.push_back(std::move(t));
    }

    void handleSubframe(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow) {
        SatNav& s = sat[t.prn];
        const unsigned id = lnavSubframeId(ev.sf);
        s.towLast = (int)ev.towStart;
        if (id == 1 || id == 2 || id == 3) {
            if (s.pend.prn != t.prn) { s.pend = GpsEphemeris(); s.pend.prn = t.prn; }
            if (id == 1) { lnavParseSf1(ev.sf, s.pend); s.pendWeek = gpsResolveWeek(s.pend.wn, referenceWeek()); }
            else if (id == 2) lnavParseSf2(ev.sf, s.pend);
            else lnavParseSf3(ev.sf, s.pend);
            if (s.pend.complete() && s.pendWeek > 0 && (!s.hasEph || s.eph.iodc != s.pend.iodc || s.eph.toe != s.pend.toe)) {
                s.eph = s.pend; s.hasEph = true; s.week = s.pendWeek; s.ephSignalTime = signalNow;
                char b[120];
                snprintf(b, sizeof b, "%s ephemeris IODE %d, week %d, health %s", satName(GnssGps, t.prn).c_str(), s.eph.iode(), s.week, s.eph.health ? "bad" : "ok");
                logf(b);
            }
        } else if (id == 4 || id == 5) {
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
            if (t->carrierLocked() && !wasLocked) { char b[100]; snprintf(b, sizeof b, "%s locked, %.0f dB-Hz", satName(t->sys, t->prn).c_str(), t->cn0()); logf(b); }
            if (t->bitSynced() && !wasBit) logf(satName(t->sys, t->prn) + " bit sync");
            if (t->frameSynced() && !wasFrame) logf(satName(t->sys, t->prn) + " frame sync");
            (void)okBefore;
            for (auto& ev : t->events()) handleSubframe(*t, ev, signalNow);
            t->events().clear();
        }
        for (size_t i = 0; i < trackers.size();) {
            if (trackers[i]->lost()) {
                okRetired += trackers[i]->framesOk(); badRetired += trackers[i]->framesBad();
                logf(satName(trackers[i]->sys, trackers[i]->prn) + (trackers[i]->wasPullInTimeout() ? " not confirmed" : " lost"));
                trackers.erase(trackers.begin() + (long)i);
            } else i++;
        }
    }

    bool skipPrn(int prn) const {
        if (tracked(prn)) return true;
        // with a fix and the sky predicted from the almanac, look for the satellites that should be in view; the others now and then
        if (havePredicted && gpsAcq.rounds() % 6 != 5 && std::find(predictedVisible.begin(), predictedVisible.end(), prn) == predictedVisible.end()) return true;
        return false;
    }

    void runAcq(double seconds) {
        if (!gpsActive) return;
        // search less when the satellites expected in view are all tracked
        acqCredit = std::min(acqCredit + seconds * 1000.0 * (double)acqRate.load(), 600.0);
        if (acqCredit < 8) return;
        std::vector<GnssAcqHit> hits;
        const int used = gpsAcq.work(gpsBand, (int)acqCredit, [this](int p) { return skipPrn(p); }, hits);
        acqCredit -= used;
        units += (uint64_t)used;
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
            if (t->sys != GnssGps || !t->carrierLocked() || !t->timeValid()) continue;
            double tow;
            if (!t->transmitTime(S, &tow)) continue;
            GnssMeasurement m;
            m.sys = GnssGps; m.prn = t->prn; m.rxTime = S; m.txTow = tow; m.dopplerHz = t->dopplerHz(); m.cn0 = t->cn0();
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
            o.sys = GnssGps; o.prn = meas[i].prn;
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
            fix.nSatsPerSystem[GnssGps] = sl.nUsed;
            fix.hdop = sl.hdop; fix.vdop = sl.vdop; fix.pdop = sl.pdop; fix.tdop = sl.tdop;
            fix.hErrM = sl.hErrM;
            fix.vErrM = sl.vErrM;
            fix.residualRmsM = (float)sl.rmsResidual;
            char b[64];
            snprintf(b, sizeof b, "GPS %d satellites", sl.nUsed);
            fix.type = b;
            // GPS time of the measurement: the reference time less the clock error
            const double tGps = tr0 - sl.bias[GnssGps] / kC;
            int week = 0;
            for (auto& m : meas) if (sat[m.prn].hasEph) { week = sat[m.prn].week; break; }
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
        t.systemsMask = sysMask.load() & 1u;
        t.activeMask = gpsActive ? 1u : 0u;
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
                for (auto& tr : trackers) if (tr->prn == p) { sk.tracked = tr->carrierLocked(); sk.cn0 = sk.tracked ? tr->cn0() : 0.f; }
                sk.used = sol[p].used;
                sk.health = sat[p].hasEph ? (sat[p].eph.health ? 1 : 0) : (sat[p].alm.health ? 1 : 0);
                t.sky.push_back(sk);
            }
        }
        // search
        t.searching = gpsActive;
        t.searchSys = GnssGps; t.searchPrn = gpsAcq.currentPrn();
        t.searchProgress = gpsAcq.progress(); t.searchRounds = gpsAcq.rounds();
        t.acqSys = GnssGps; t.acqPrn = gpsAcq.lastPrn(); t.acqDopplerHz = (float)gpsAcq.lastDoppler(); t.acqPeakToNoise = gpsAcq.lastRatio();
        t.acqCorr = gpsAcq.lastCorr(); t.acqPeakIndex = gpsAcq.lastPeakIndex();
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
        snprintf(b, sizeof b, "%d tracked, %d with ephemeris, %s", t.nTracked, (int)std::count_if(sat + 1, sat + 33, [](const SatNav& s) { return s.hasEph; }), fix.valid ? fix.type.c_str() : "no fix");
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
