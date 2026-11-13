// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS receiver, building the GnssTelemetry published to the UI (trackers, fix, satellites, sky, search, interference).
#include "gnss_rx_impl.h"

namespace dect2 {

// The fix, clock, leap seconds and the level histories
void GnssReceiver::Impl::telFix(GnssTelemetry& t, double signalNow, float best, uint64_t ok, uint64_t bad) {
    t.blocksOk = ok; t.blocksBad = bad;
    t.snrDb = best;
    t.fix = fix;
    t.dataValid = fix.valid;
    t.state = fix.valid ? 2 : (t.nTracked > 0 ? 1 : 0);
    t.cfoHz = cfoEst;
    t.fix.clockDriftMps = driftEst;
    t.ionoValid = iono.valid; t.utcValid = utc.valid; t.leapSeconds = utc.valid ? utc.dtls : -1;
    if (utc.valid && utc.dtlsf != utc.dtls) { t.leapPending = true; t.leapNext = utc.dtlsf; t.leapWn = utc.wnlsf; t.leapDn = utc.dn; }
    t.clockValid = clkValid && fix.valid && clockCompare.load();
    t.clockOffsetMs = clkOff; t.clockOffsetAvgMs = clkAvg;
    if (signalNow >= histNext) {   // one point a second
        histNext = signalNow + 1.0;
        levHist.push_back((float)(20 * std::log10(std::max(levelRms, 1e-6))));
        snrHist.push_back(best);
        offHist.push_back(fix.valid && clkValid ? offNow : NAN);
        for (auto* h : {&levHist, &snrHist, &offHist}) while (h->size() > 120) h->pop_front();
    }
    t.levelHist.assign(levHist.begin(), levHist.end()); t.snrHist.assign(snrHist.begin(), snrHist.end()); t.offsetHist.assign(offHist.begin(), offHist.end());
    for (int i = 0; i < 4; i++) { t.ionoAlpha[i] = (float)iono.alpha[i]; t.ionoBeta[i] = (float)iono.beta[i]; }
    int na = 0;
    for (int p = 1; p <= 32; p++) if (sat[p].alm.valid) na++;
    t.almanacGps = na;
}

// Satellite positions from the ephemerides, in the table of satellites
void GnssReceiver::Impl::telSatellites(GnssTelemetry& t) {
    auto pushSat = [&](int sys, int prn, int fcn, const double pos[3], bool fromEph, int health, bool used, double ephAgeS) {
        if (t.sats.size() >= 96) return;
        double lat, lon, h;
        ecefToLla(pos, &lat, &lon, &h);
        if (!std::isfinite(lat) || !std::isfinite(lon) || h < 100e3) return;
        GnssSatPos s;
        s.sys = sys; s.prn = prn; s.fcn = fcn;
        s.lat = lat; s.lon = lon; s.altKm = h / 1e3;
        s.fromEphemeris = fromEph; s.health = health; s.used = used; s.ephAgeS = fromEph ? ephAgeS : -1;
        for (auto& tr : trackers) {
            const bool match = sys == GnssGlonass ? (tr->sys == GnssGlonass && tr->fcn == fcn)
                                                  : (tr->sys == sys && tr->prn == prn);
            if (!match) continue;
            s.tracked = tr->carrierLocked();
            s.cn0 = s.tracked ? tr->cn0() : 0.f;
        }
        t.sats.push_back(s);
    };
    double gpsTowNow = -1;
    if (fix.valid && fix.gpsWeek >= 0) gpsTowNow = fix.gpsTow;
    else {
        for (int p = 1; p <= 32 && gpsTowNow < 0; p++) if (sat[p].hasEph && sat[p].towLast >= 0) gpsTowNow = sat[p].towLast;
        for (int p = 193; p <= 202 && gpsTowNow < 0; p++) if (sat[p].hasEph && sat[p].towLast >= 0) gpsTowNow = sat[p].towLast;
    }
    if (gpsTowNow >= 0) {
        for (int p = 1; p <= 32; p++) {
            double pos[3];
            bool fromEph = false;
            if (sat[p].hasEph) { gpsEphemerisState(sat[p].eph, gpsTowNow, pos, nullptr); fromEph = true; }
            else if (sat[p].alm.valid) gpsAlmanacPos(sat[p].alm, gpsTowNow, pos);
            else continue;
            const int health = fromEph ? (sat[p].eph.health ? 1 : 0) : (sat[p].alm.health ? 1 : 0);
            pushSat(GnssGps, p, 0, pos, fromEph, health, sol[p].used, fromEph ? std::fabs(gpsWrap(gpsTowNow - sat[p].eph.toe)) : -1);
        }
        for (int p = 193; p <= 202; p++) {
            if (!sat[p].hasEph) continue;
            double pos[3];
            gpsEphemerisState(sat[p].eph, gpsTowNow, pos, nullptr);
            pushSat(GnssQzss, p, 0, pos, true, sat[p].eph.health ? 1 : 0, sol[p].used, std::fabs(gpsWrap(gpsTowNow - sat[p].eph.toe)));
        }
    }
    double galTow = gpsTowNow >= 0 ? gpsTowNow + ggtoAt(gpsTowNow) : -1;
    if (galTow < 0) for (int p = 1; p < kGalPrns; p++) if (gsat[p].hasEph && gsat[p].towLast >= 0) { galTow = gsat[p].towLast; break; }
    if (galTow >= 0) {
        for (int p = 1; p < kGalPrns; p++) {
            if (!gsat[p].hasEph) continue;
            double pos[3];
            gpsEphemerisState(gsat[p].eph, galTow, pos, nullptr);
            pushSat(GnssGalileo, p, 0, pos, true, gsat[p].eph.health ? 1 : 0, gsol[p].used, std::fabs(gpsWrap(galTow - gsat[p].eph.toe)));
        }
    }
    double bdtTow = -1;
    if (gpsTowNow >= 0) {
        bdtTow = gpsTowNow - kBdtGpsSeconds + bdtAt(gpsTowNow - kBdtGpsSeconds);
        if (bdtTow < 0) bdtTow += 604800.0;
    } else {
        for (int p = 1; p <= kBdsMaxPrn; p++) if (bsat[p].hasEph && bsat[p].towLast >= 0) { bdtTow = bsat[p].towLast; break; }
    }
    if (bdtTow >= 0) {
        for (int p = 1; p <= kBdsMaxPrn; p++) {
            if (!bsat[p].hasEph) continue;
            double pos[3];
            gpsEphemerisState(bsat[p].eph, bdtTow, pos, nullptr);
            pushSat(GnssBeidou, p, 0, pos, true, bsat[p].eph.health ? 1 : 0, bsol[p].used, std::fabs(gpsWrap(bdtTow - bsat[p].eph.toe)));
        }
    }
    const double gloTod = gpsTowNow >= 0 ? gpsToGloTod(gpsTowNow) : -1;
    for (int i = 0; i < kGloSlots; i++) {
        if (!gloSat[i].hasEph) continue;
        double tod = gloTod;
        if (tod < 0) tod = gloEph[i].tb;
        double pos[3];
        gloEphemerisState(gloEph[i], tod, pos, nullptr, nullptr, 60.0);
        pushSat(GnssGlonass, gloEph[i].n, i + kGloMinK, pos, true, gloEph[i].healthy() ? 0 : 1, glsol[i].used,
                std::fabs(std::remainder(tod - gloEph[i].tb, 86400.0)));
    }
}

// The sky: satellites above the horizon by the ephemeris or the almanac, from the fix
void GnssReceiver::Impl::telSky(GnssTelemetry& t) {
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
        // Galileo from its ephemeris
        for (int p = 1; p < kGalPrns && t.sky.size() < 64; p++) {
            if (!gsat[p].hasEph) continue;
            double pos[3], az, el;
            gpsEphemerisState(gsat[p].eph, fix.gpsTow + ggtoAt(fix.gpsTow), pos, nullptr);
            azElFromEcef(fix.ecef, pos, &az, &el);
            if (el < 0) continue;
            GnssSky sk;
            sk.sys = GnssGalileo; sk.prn = p; sk.azDeg = (float)az; sk.elDeg = (float)el; sk.fromEphemeris = true;
            for (auto& tr : trackers) if (tr->sys == GnssGalileo && tr->prn == p) { sk.tracked = tr->carrierLocked(); sk.cn0 = sk.tracked ? tr->cn0() : 0.f; }
            sk.used = gsol[p].used;
            sk.health = gsat[p].eph.health ? 1 : 0;
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
        // BeiDou from its ephemeris (BDT)
        for (int p = 1; p <= kBdsMaxPrn && t.sky.size() < 64; p++) {
            if (!bsat[p].hasEph) continue;
            double pos[3], az, el;
            double tb = fix.gpsTow - kBdtGpsSeconds + bdtAt(fix.gpsTow - kBdtGpsSeconds);
            if (tb < 0) tb += 604800.0;
            gpsEphemerisState(bsat[p].eph, tb, pos, nullptr);
            azElFromEcef(fix.ecef, pos, &az, &el);
            if (el < 0) continue;
            GnssSky sk;
            sk.sys = GnssBeidou; sk.prn = p; sk.azDeg = (float)az; sk.elDeg = (float)el; sk.fromEphemeris = true;
            for (auto& tr : trackers) if (tr->sys == GnssBeidou && tr->prn == p) { sk.tracked = tr->carrierLocked(); sk.cn0 = sk.tracked ? tr->cn0() : 0.f; }
            sk.used = bsol[p].used;
            sk.health = bsat[p].eph.health ? 1 : 0;
            t.sky.push_back(sk);
        }
        // GLONASS from its ephemeris (its orbit integrated from tb)
        for (int i = 0; i < kGloSlots && t.sky.size() < 64; i++) {
            if (!gloSat[i].hasEph) continue;
            double pos[3], az, el;
            gloEphemerisState(gloEph[i], gpsToGloTod(fix.gpsTow), pos, nullptr, nullptr, 60.0);
            azElFromEcef(fix.ecef, pos, &az, &el);
            if (el < 0) continue;
            GnssSky sk;
            sk.sys = GnssGlonass; sk.prn = gloEph[i].n; sk.fcn = i + kGloMinK; sk.azDeg = (float)az; sk.elDeg = (float)el; sk.fromEphemeris = true;
            for (auto& tr : trackers) if (tr->sys == GnssGlonass && tr->fcn == sk.fcn) { sk.tracked = tr->carrierLocked(); sk.cn0 = sk.tracked ? tr->cn0() : 0.f; }
            sk.used = glsol[i].used;
            sk.health = gloEph[i].healthy() ? 0 : 1;
            t.sky.push_back(sk);
        }
    }
}

// What the search is doing, the signal level, the status line and the interference monitor
void GnssReceiver::Impl::telSearchAndStatus(GnssTelemetry& t, double signalNow, const std::vector<const GnssTracker*>& order) {
    // search: the L1 one, or the BeiDou one on a tune of its own
    t.searching = gpsActive || bdsActive || gloActive;
    if (gpsActive) {
        t.searchSys = gpsAcq.currentPrn() > 0 ? l1caSystem(gpsAcq.currentPrn()) : GnssGps; t.searchPrn = gpsAcq.currentPrn();
        t.searchProgress = gpsAcq.progress(); t.searchRounds = gpsAcq.rounds();
        t.acqSys = gpsAcq.lastPrn() > 0 ? l1caSystem(gpsAcq.lastPrn()) : GnssGps; t.acqPrn = gpsAcq.lastPrn(); t.acqDopplerHz = (float)gpsAcq.lastDoppler(); t.acqPeakToNoise = gpsAcq.lastRatio();
        t.acqCorr = gpsAcq.lastCorr(); t.acqPeakIndex = gpsAcq.lastPeakIndex();
        t.searchCenterHz = (float)gpsAcq.windowCenterHz(); t.searchHalfHz = (float)gpsAcq.windowHalfHz(); t.searchMs = gpsAcq.blocksNow();
        t.searchStage = t.nTracked > 0 ? 0 : stage;
    } else if (bdsActive) {
        t.searchSys = GnssBeidou; t.searchPrn = bdsAcq.currentPrn();
        t.searchProgress = bdsAcq.progress(); t.searchRounds = bdsAcq.rounds();
        t.acqSys = GnssBeidou; t.acqPrn = bdsAcq.lastPrn(); t.acqDopplerHz = (float)bdsAcq.lastDoppler(); t.acqPeakToNoise = bdsAcq.lastRatio();
        t.acqCorr = bdsAcq.lastCorr(); t.acqPeakIndex = bdsAcq.lastPeakIndex();
        t.searchCenterHz = (float)bdsAcq.windowCenterHz(); t.searchHalfHz = (float)bdsAcq.windowHalfHz(); t.searchMs = bdsAcq.blocksNow();
        t.searchStage = t.nTracked > 0 ? 0 : bdsPlan.stage;
    } else if (gloActive) {
        int i = gloNext;
        for (int n = 0; n < kGloSlots && !gloSlotOn[i]; n++) i = (i + 1) % kGloSlots;
        const GnssAcq& q = gloAcq[i];
        t.searchSys = GnssGlonass; t.searchPrn = 0;
        t.searchProgress = (float)i / (float)kGloSlots; t.searchRounds = q.rounds();
        t.acqSys = GnssGlonass; t.acqPrn = 0; t.acqDopplerHz = (float)q.lastDoppler(); t.acqPeakToNoise = q.lastRatio();
        t.acqCorr = q.lastCorr(); t.acqPeakIndex = q.lastPeakIndex();
        t.searchCenterHz = (float)q.windowCenterHz(); t.searchHalfHz = (float)q.windowHalfHz(); t.searchMs = q.blocksNow();
        t.searchStage = t.nTracked > 0 ? 0 : gloPlan.stage;
    }
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
    snprintf(b, sizeof b, "%d tracked, %d with ephemeris, %s", t.nTracked, (int)(std::count_if(sat + 1, sat + kCaPrns, [](const SatNav& s) { return s.hasEph; }) + std::count_if(gsat + 1, gsat + kGalPrns, [](const SatNav& s) { return s.hasEph; }) + std::count_if(bsat + 1, bsat + kBdsPrns, [](const SatNav& s) { return s.hasEph; }) + std::count_if(gloSat, gloSat + kGloSlots, [](const SatNav& s) { return s.hasEph; })), fix.valid ? fix.type.c_str() : "no fix");
    t.status = b;
    // the interference monitor: the ephemeris of each GPS satellite against its almanac, then the report
    {
        double worst = -1;
        int worstPrn = 0;
        if (fix.valid) {
            for (int p = 1; p <= 32; p++) {
                if (!sat[p].hasEph || !sat[p].alm.valid) continue;
                double pe[3], pa[3], clk;
                gpsEphemerisState(sat[p].eph, fix.gpsTow, pe, &clk);
                gpsAlmanacPos(sat[p].alm, fix.gpsTow, pa);
                const double d = std::sqrt((pe[0] - pa[0]) * (pe[0] - pa[0]) + (pe[1] - pa[1]) * (pe[1] - pa[1]) + (pe[2] - pa[2]) * (pe[2] - pa[2])) / 1e3;
                if (d > worst) { worst = d; worstPrn = p; }
            }
        }
        interf.update(t, worst, worstPrn);
        t.interf = interf.report();
    }
}

// ------------------------------------------------------------------ telemetry
void GnssReceiver::Impl::buildTelemetry(double signalNow) {
    GnssTelemetry t;
    t.seq = ++seq;
    t.inputRate = rate;
    t.centerMhz = centerMhz.load();
    t.systemsMask = sysMask.load() & (gnssSystemBit(GnssGps) | gnssSystemBit(GnssQzss) | gnssSystemBit(GnssSbas) | gnssSystemBit(GnssGalileo) | gnssSystemBit(GnssBeidou) | gnssSystemBit(GnssGlonass));
    t.activeMask = activeMask();
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
        c.sys = tr->sys; c.prn = tr->prn; c.fcn = tr->sys == GnssGlonass ? tr->fcn : 0;
        const int key = keyOf(tr->sys, tr->prn, tr->fcn);
        int st = tr->state();
        const SatNav& s = navOf(tr->sys, key);
        if (st >= GnssChFrameSync && s.hasEph) st = GnssChEphemeris;
        c.state = st;
        c.cn0 = tr->cn0();
        c.dopplerHz = tr->dopplerHz();
        c.codePhase = tr->codePhase() * (tr->sys == GnssGalileo ? 0.5 : 1.0);   // Galileo: counted in half chips
        const SolInfo& si = solOf(tr->sys, key);
        if (fix.valid && s.hasEph) {
            double pos[3], az, el;
            if (tr->sys == GnssGlonass) gloEphemerisState(gloEph[key], gpsToGloTod(fix.gpsTow), pos, nullptr, nullptr);
            else gpsEphemerisState(s.eph, fix.gpsTow - (tr->sys == GnssBeidou ? kBdtGpsSeconds : 0.0), pos, nullptr);
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
        ni.sys = tr->sys; ni.prn = tr->prn; ni.fcn = c.fcn;
        ni.hasEphemeris = s.hasEph;
        if (s.hasEph) {
            ni.iode = s.eph.iode();
            ni.health = s.eph.health ? 1 : 0;
            ni.week = s.week;
            ni.svClockBiasUs = (float)(s.eph.af0 * 1e6);
            if (fix.valid) ni.ephAgeS = tr->sys == GnssGlonass ? (float)gloWrapDay(gpsToGloTod(fix.gpsTow) - s.eph.toe)
                                                               : (float)gpsWrap(fix.gpsTow - (tr->sys == GnssBeidou ? kBdtGpsSeconds : 0.0) - s.eph.toe);
        }
        ni.hasAlmanac = s.alm.valid;
        ni.towS = s.towLast;
        ni.sbasLastType = s.sbasLast; ni.sbasMessages = s.sbasCount; ni.sbasTypesSeen = s.sbasTypes;
        if (tr->sys == GnssGalileo && gnav[tr->prn].ggtoValid) { ni.ggtoValid = true; ni.ggtoNs = (float)(gnav[tr->prn].a0g * 1e9); }
        // subframes 1, 2, 3 come every 30 s, 6 s apart: the time left is until the last missing one has arrived
        if (s.hasEph) { ni.ephParts = 3; ni.ephEtaS = 0; }
        else if (tr->sys == GnssSbas) { ni.ephParts = 0; ni.ephEtaS = -1; }
        else if (tr->sys == GnssBeidou) { ni.ephParts = bnav[tr->prn].parts(); ni.ephEtaS = -1; }
        else if (tr->sys == GnssGlonass) { ni.ephParts = gloNav[key].parts() * 3 / 4; ni.ephEtaS = -1; }
        else if (tr->sys == GnssGalileo) {
            // words 1-4 of one IODnav (shown as parts of 4), then word 5
            const GalNav& g = gnav[tr->prn];
            int parts = 0;
            for (int k = 1; k <= 4; k++) parts += g.iod[k] >= 0 && g.iod[k] == g.iod[1];
            ni.ephParts = parts; ni.ephEtaS = -1;
        }
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
    telFix(t, signalNow, best, ok, bad);
    telSatellites(t);
    telSky(t);
    telSearchAndStatus(t, signalNow, order);
    std::lock_guard<std::mutex> lk(telMu);
    tel = std::move(t);
    telReady = true;
}

} // namespace dect2
