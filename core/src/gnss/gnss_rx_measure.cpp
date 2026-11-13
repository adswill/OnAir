// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS receiver, the measurement epoch: pseudoranges, the position solution, the clock estimate and the sky prediction.
#include "gnss_rx_impl.h"
#include "dect2/common/diag.h"
#include "dect2/automation/engine.h"

namespace dect2 {

// Takes a position solution (or its loss) into the fix, the per-system solution info and the clock estimate
void GnssReceiver::Impl::applyFix(double S, bool solved, GnssSolution& sl, std::vector<GnssObs>& obs, std::vector<GnssMeasurement>& meas, std::vector<size_t>& idx, double tr0) {
    if (solved) {
        fix.valid = true;
        for (int k = 0; k < 3; k++) { fix.ecef[k] = sl.x[k]; prior[k] = sl.x[k]; }
        hasPrior = true;
        ecefToLla(sl.x, &fix.latDeg, &fix.lonDeg, &fix.heightM);
        // the receiver's clock is the signal time plus an offset set at the first fix; its error against GPS time then grows with the oscillator error
        // the clock unknown that stands for GPS time: GPS's own, else that of the first system in the fix (its time moved to the GPS scale)
        int refSys = GnssGps;
        if (!sl.sysUsed[GnssGps]) { static const int order[3] = {GnssGalileo, GnssBeidou, GnssGlonass}; for (int s : order) if (sl.sysUsed[s]) { refSys = s; break; } }
        const double refBias = sl.bias[refSys];
        const double tGpsMeas = tr0 - refBias / kC;
        if (!clockSet) { clockOffset = tGpsMeas - S; clockSet = true; }
        const double gpsBias = kC * ((S + clockOffset) - tGpsMeas);
        for (int s = 0; s < GnssSystems; s++) {
            fix.systemInFix[s] = sl.sysUsed[s];
            fix.clockBiasM[s] = sl.sysUsed[s] ? gpsBias + (sl.bias[s] - refBias) : 0.0;
        }
        fix.nSats = sl.nUsed;
        for (int s = 0; s < GnssSystems; s++) fix.nSatsPerSystem[s] = 0;
        for (size_t k = 0; k < obs.size(); k++) if (sl.used[k]) fix.nSatsPerSystem[meas[idx[k]].sys]++;
        if (fix.nSatsPerSystem[GnssQzss] > 0) { fix.systemInFix[GnssQzss] = true; fix.clockBiasM[GnssQzss] = fix.clockBiasM[GnssGps]; }
        fix.ggtoApplied = false; fix.ggtoNs = ggtoValid ? ggtoAt(tr0) * 1e9 : 0.0;
        if (fix.nSatsPerSystem[GnssGalileo] > 0 && !sl.sysUsed[GnssGalileo]) {
            // the Galileo satellites were moved to GPS time: the receiver clock against GST is the GPS one less the offset
            fix.systemInFix[GnssGalileo] = true; fix.ggtoApplied = true;
            fix.clockBiasM[GnssGalileo] = fix.clockBiasM[GnssGps] - kC * ggtoAt(tr0);
        }
        fix.gloTimeValid = gloTimeValid; fix.gloTauGpsNs = gloTauGps * 1e9; fix.gloTauCNs = gloTauC * 1e9;
        fix.bdtGpsApplied = false; fix.bdtGpsNs = bdtGpsValid ? bdtAt(tr0 - kBdtGpsSeconds) * 1e9 : 0.0;
        if (fix.nSatsPerSystem[GnssBeidou] > 0 && !sl.sysUsed[GnssBeidou]) {
            // BeiDou moved to GPS time: the receiver clock against BDT (less the whole 14 s) is the GPS one less the offset
            fix.systemInFix[GnssBeidou] = true; fix.bdtGpsApplied = true;
            fix.clockBiasM[GnssBeidou] = fix.clockBiasM[GnssGps] - kC * bdtAt(tr0 - kBdtGpsSeconds);
        }
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
        const double tGps = tr0 - refBias / kC;
        int week = 0;
        for (auto& m : meas) if (m.sys == GnssGps && sat[m.prn].hasEph) { week = sat[m.prn].week; break; }
        if (week == 0) for (auto& m : meas) if ((m.sys == GnssGps || m.sys == GnssQzss) && sat[m.prn].hasEph) { week = sat[m.prn].week; break; }
        if (week == 0) for (auto& m : meas) if (m.sys == GnssBeidou && bsat[m.prn].hasEph) { week = bsat[m.prn].week + kBdtGpsWeeks; break; }
        if (week == 0) for (auto& m : meas) if (m.sys == GnssGalileo && gsat[m.prn].hasEph) { week = gsat[m.prn].week; break; }
        if (week == 0) for (auto& m : meas) if (m.sys == GnssGlonass && gloSat[m.fcn - kGloMinK].hasEph) { week = gloGpsWeek(gloEph[m.fcn - kGloMinK], m.txTow); break; }
        double tow = tGps;
        if (tow < 0) { tow += 604800.0; week--; }
        if (tow >= 604800.0) { tow -= 604800.0; week++; }
        fix.gpsWeek = week; fix.gpsTow = tow;
        fix.leapSeconds = utc.valid ? utc.dtls : -1;
        if (week > 0 && clockCompare.load()) {
            const double wall0 = std::min(wallMinCur, wallMinPrev);
            const double towAtS = tow + ((S + clockOffset) - tr0);
            fix.signalS = S;
            fix.unixAtSignal = 315964800.0 + (double)week * 604800.0 + towAtS - (utc.valid ? utc.dtls : 18);
            if (wall0 < 1e299) {
                offNow = (float)((wall0 + S - fix.unixAtSignal) * 1000.0);
                if (std::isfinite(offNow) && std::fabs(offNow) < 1e7) {
                    offWin.push_back(offNow);
                    while (offWin.size() > 60) offWin.pop_front();
                    double sm = 0;
                    for (float o : offWin) sm += o;
                    clkOff = offNow; clkAvg = sm / (double)offWin.size(); clkValid = true;
                } else {
                    DIAG_DEBUG_EVERY(352685, 10.0, "The receiver clock comparison gave an offset of %.0f ms, which is not plausible, so this reading is ignored.", (double)offNow);
                    offNow = NAN;
                }
            }
        }
        fix.timeValid = week > 0;
        if (fix.timeValid) gpsTimeToCalendar(week, tow, utc.valid ? utc.dtls : 18, &fix.year, &fix.month, &fix.day, &fix.hour, &fix.minute, &fix.second);
        fix.fixCount = (uint32_t)(++fixCount);
        if (Feeds::get().wantGnss()) Feeds::get().gnss(fix);
        if (automation::wants(automation::FGnss) && fix.valid && (fix.fixCount % 10) == 1) {   // the automations: about every tenth fix
            automation::Event ev("gnss.fix");
            ev.at(automation::wallNow(), "GNSS", 1575.42e6);
            ev.num("lat", fix.latDeg).num("lon", fix.lonDeg).num("alt_m", fix.heightM).num("sats", fix.nSats);
            automation::emit(std::move(ev));
        }
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
        DIAG(595353, "The position fix was lost because no new solution was computed for over 20 s, so the receiver reports no fix until it solves again.");
    }
}

// ------------------------------------------------------------------ the fix
void GnssReceiver::Impl::measure(double S) {
    std::vector<GnssObs> obs;
    std::vector<GnssMeasurement> meas;
    std::vector<int> prns;
    std::vector<double> towv;
    for (auto& t : trackers) {
        // QZSS keeps GPS time and sends the GPS navigation message: it joins the GPS satellites (IS-QZSS-PNT); SBAS is not used for ranging
        if ((t->sys != GnssGps && t->sys != GnssQzss && t->sys != GnssGalileo && t->sys != GnssBeidou && t->sys != GnssGlonass) || !t->carrierLocked() || !t->timeValid()) continue;
        double tow;
        if (!t->transmitTime(S, &tow)) continue;
        GnssMeasurement m;
        m.sys = t->sys; m.prn = t->prn; m.fcn = t->fcn; m.rxTime = S; m.txTow = tow; m.dopplerHz = t->dopplerHz(); m.cn0 = t->cn0();
        if (m.sys == GnssGlonass) m.txTow = std::fmod(tow, 86400.0);
        meas.push_back(m);
    }
    // satellites with a usable ephemeris for the solution
    double tRef = 0;
    int n = 0;
    // the Galileo times are GST: on the GPS scale with the broadcast offset (a few ns; the reference only needs to be near)
    for (auto& m : meas) {
        const SatNav& s = navOf(m.sys, keyOf(m.sys, m.prn, m.fcn));
        if (!s.hasEph || s.eph.health != 0 || m.cn0 <= 24.0) continue;
        if (m.sys == GnssGlonass) tRef += gloToGps(gloEph[m.fcn - kGloMinK], m.txTow);
        else tRef += m.sys == GnssBeidou ? toGpsTime(m.sys, m.txTow) : m.txTow - (m.sys == GnssGalileo ? ggtoAt(m.txTow) : 0.0);
        n++;
    }
    if (n >= 1) tRef /= n;
    // average in a way that survives the week boundary: the times are within 20 ms of each other, so a plain mean is fine except at the wrap
    const double tr0 = std::round((tRef + 0.075) * 1e6) / 1e6;
    std::vector<size_t> idx;
    for (size_t i = 0; i < meas.size(); i++) {
        const SatNav& s = navOf(meas[i].sys, keyOf(meas[i].sys, meas[i].prn, meas[i].fcn));
        if (!s.hasEph || s.eph.health != 0 || meas[i].cn0 < 24.0) continue;
        if (meas[i].sys == GnssGlonass) {
            // GLONASS: the orbit integrated from tb at GLONASS time (the satellite's time less its clock), the transmit time moved to the GPS scale
            // with the day (NT, N4), the leap seconds and tau_GPS; a clock unknown of its own (FDMA receivers see a bias of their own on it)
            const GloEphemeris& ge = gloEph[meas[i].fcn - kGloMinK];
            double tSys = meas[i].txTow, pos[3], clk = 0;
            for (int k = 0; k < 3; k++) {
                gloEphemerisState(ge, tSys, pos, nullptr, &clk);
                tSys = meas[i].txTow - clk;
            }
            GnssObs o;
            o.sys = GnssGlonass; o.prn = meas[i].fcn - kGloMinK;
            o.pr = kC * (gpsWrap(tr0 - gloToGps(ge, meas[i].txTow)) + clk);

            o.sat[0] = pos[0]; o.sat[1] = pos[1]; o.sat[2] = pos[2];
            o.weight = std::min(2.0, std::max(0.05, std::pow(10.0, (meas[i].cn0 - 45.0) / 10.0)));
            obs.push_back(o);
            idx.push_back(i);
            continue;
        }
        double tTrue = meas[i].txTow, pos[3], dts = 0;
        for (int k = 0; k < 3; k++) {
            gpsEphemerisState(s.eph, tTrue, pos, &dts);
            tTrue = meas[i].txTow - (dts - s.eph.tgd);
        }
        GnssObs o;
        o.sys = GnssGps; o.prn = meas[i].prn;          // QZSS too: one clock with GPS
        double txGps = meas[i].txTow;
        if (meas[i].sys == GnssGalileo) {
            // GST to GPS time with the offset of word type 10 when it is known: then Galileo shares the GPS clock unknown; without it the solver
            // estimates the offset as a clock of its own (one more unknown)
            if (ggtoValid) txGps -= ggtoAt(meas[i].txTow);
            else o.sys = GnssGalileo;
        } else if (meas[i].sys == GnssBeidou) {
            // BDT is 14 s behind GPS time; with the broadcast A0GPS/A1GPS BeiDou shares the GPS clock unknown, without them it has one of its own
            txGps = toGpsTime(GnssBeidou, meas[i].txTow);
            if (!bdtGpsValid) o.sys = GnssBeidou;
        }
        o.pr = kC * (gpsWrap(tr0 - txGps) + (dts - s.eph.tgd));
        o.sat[0] = pos[0]; o.sat[1] = pos[1]; o.sat[2] = pos[2];
        o.weight = std::min(2.0, std::max(0.05, std::pow(10.0, (meas[i].cn0 - 45.0) / 10.0)));
        obs.push_back(o);
        idx.push_back(i);
    }
    for (auto& s : sol) s = SolInfo();
    for (auto& s : gsol) s = SolInfo();
    for (auto& s : bsol) s = SolInfo();
    for (auto& s : glsol) s = SolInfo();
    bool solved = false;
    GnssSolution sl;
    if (obs.size() >= 4) {
        GnssSolveOptions opt;
        opt.hasIono = iono.valid; opt.iono = iono; opt.tow = tr0;
        opt.hasPrior = hasPrior;
        for (int k = 0; k < 3; k++) opt.prior[k] = prior[k];
        opt.maskDeg = elMask.load();
        solved = gnssSolve(obs, opt, sl);
        if (!solved) DIAG_DEBUG_EVERY(674781, 10.0, "The position solver found no solution from %zu satellites with ephemerides, so no fix is updated this second.", obs.size());
        if (solved) {
            double lat, lon, h;
            ecefToLla(sl.x, &lat, &lon, &h);
            // sanity: on or near the earth, residuals not absurd
            if (!(h > -1000 && h < 30000) || sl.rmsResidual > 200.0) {
                DIAG_DEBUG_EVERY(846892, 10.0, "A position solution at %.0f m height with %.0f m residual was refused as implausible, so the previous fix is kept.", h, sl.rmsResidual);
                solved = false;
            }
        }
    }
    for (size_t k = 0; k < obs.size(); k++) {
        if (!solved) break;
        SolInfo& si = solOf(meas[idx[k]].sys, obs[k].prn);
        si.used = sl.used[k] != 0; si.resid = (float)sl.residual[k]; si.az = (float)sl.azDeg[k]; si.el = (float)sl.elDeg[k]; si.has = true;
        meas[idx[k]].inFix = si.used; meas[idx[k]].residualM = sl.residual[k];
    }
    applyFix(S, solved, sl, obs, meas, idx, tr0);
    lastMeas = meas;
    if (measCb) measCb(meas);
}

// The receiver's own frequency error from the Doppler of the satellites: measured minus what the geometry says, and the clock drift
void GnssReceiver::Impl::estimateClock(const GnssSolution& sl, const std::vector<GnssObs>& obs, const std::vector<GnssMeasurement>& meas, const std::vector<size_t>& idx, double tr0) {
    std::vector<double> res;
    for (size_t k = 0; k < obs.size(); k++) {
        if (!sl.used[k]) continue;
        const GnssMeasurement& m = meas[idx[k]];
        const SatNav& s = navOf(m.sys, obs[k].prn);
        double tTx = m.txTow;
        double p1[3], p2[3], c1, c2;
        if (m.sys == GnssGlonass) {
            gloEphemerisState(gloEph[m.fcn - kGloMinK], tTx - 0.25, p1, nullptr, &c1);
            gloEphemerisState(gloEph[m.fcn - kGloMinK], tTx + 0.25, p2, nullptr, &c2);
        } else {
            gpsEphemerisState(s.eph, tTx - 0.25, p1, &c1);
            gpsEphemerisState(s.eph, tTx + 0.25, p2, &c2);
        }
        const double r1 = std::sqrt((p1[0] - sl.x[0]) * (p1[0] - sl.x[0]) + (p1[1] - sl.x[1]) * (p1[1] - sl.x[1]) + (p1[2] - sl.x[2]) * (p1[2] - sl.x[2]));
        const double r2 = std::sqrt((p2[0] - sl.x[0]) * (p2[0] - sl.x[0]) + (p2[1] - sl.x[1]) * (p2[1] - sl.x[1]) + (p2[2] - sl.x[2]) * (p2[2] - sl.x[2]));
        const double rr = (r2 - r1) / 0.5;
        // the satellite clock rate in the transmitted frequency is negligible here (a few mHz)
        // at the signal's own carrier; the radio's error is reported as seen at L1
        const double fc = carrierOf(m.sys, m.fcn);
        const double expected = -rr / kC * fc;
        res.push_back((m.dopplerHz - expected) * (kGpsL1Hz / fc));
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

void GnssReceiver::Impl::predictSky(double tow) {
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
    predictedGal.clear();
    for (int p = 1; p < kGalPrns; p++) {
        if (!gsat[p].hasEph) continue;
        double pos[3], az, el;
        gpsEphemerisState(gsat[p].eph, tow, pos, nullptr);
        azElFromEcef(fix.ecef, pos, &az, &el);
        if (el > -2.0) predictedGal.push_back(p);
    }
    // only the satellites with an ephemeris are known: the prediction just puts them first, the round still holds every other one
    galAcq.setPriority(predictedGal);
    predictedGal.clear();
    if (bdsActive) {
        std::vector<int> pb;
        for (int p = 1; p <= kBdsMaxPrn; p++) {
            if (!bsat[p].hasEph) continue;
            double pos[3], az, el;
            gpsEphemerisState(bsat[p].eph, tow - kBdtGpsSeconds, pos, nullptr);
            azElFromEcef(fix.ecef, pos, &az, &el);
            if (el > -2.0) pb.push_back(p);
        }
        bdsAcq.setPriority(pb);
    }
}

} // namespace dect2
