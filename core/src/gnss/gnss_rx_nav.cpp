// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS receiver, navigation data handling per system and the tracker loop.
#include "gnss_rx_impl.h"

namespace dect2 {

// GLONASS strings: the ephemeris from strings 1-4 of one frame, the time corrections and the four-year interval from string 5
void GnssReceiver::Impl::handleGlo(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow) {
    const int slot = t.fcn - kGloMinK;
    if (slot < 0 || slot >= kGloSlots || ev.towStart < 0) return;
    SatNav& s = gloSat[slot];
    const int m = (int)gloField(ev.bits, 84, 4);
    s.towLast = (int)ev.towStart; s.lastSfId = m; s.lastSfTime = signalNow;
    const int64_t frameId = std::llround((ev.towStart - 2.0 * (m - 1)) / 30.0);
    GloEphemeris e;
    const bool done = gloHandleString(gloNav[slot], ev.bits, frameId, &e);
    if (m == 5) {
        const GloEphemeris& pe = gloNav[slot].pend;
        if (!gloTimeValid) {
            char b[120];
            snprintf(b, sizeof b, "GLONASS time: tau_GPS %+.1f ns, tau_c %+.1f ns (from %s)", pe.tauGps * 1e9, pe.tauC * 1e9, gloName(t.prn, t.fcn).c_str());
            logf(b);
        }
        gloTimeValid = true; gloTauGps = pe.tauGps; gloTauC = pe.tauC;
        if (pe.N4 > 0) gloN4 = pe.N4;
    }
    if (!done) return;
    e.k = t.fcn;
    if (!s.hasEph || gloEph[slot].tbIndex != e.tbIndex || gloEph[slot].n != e.n || gloEph[slot].Bn != e.Bn) {
        gloEph[slot] = e;
        s.hasEph = true; s.ephSignalTime = signalNow;
        s.eph = GpsEphemeris();
        s.eph.prn = e.n; s.eph.health = e.healthy() ? 0 : 1; s.eph.iodc = s.eph.iode2 = s.eph.iode3 = e.tbIndex; s.eph.toe = e.tb; s.eph.af0 = -e.tauN;
        s.week = -1;
        t.prn = e.n;
        char b[140];
        snprintf(b, sizeof b, "%s ephemeris tb %02d:%02d, day %d, health %s", gloName(e.n, t.fcn).c_str(), (int)(e.tb / 3600), (int)std::fmod(e.tb / 60, 60.0), e.NT, e.healthy() ? "ok" : "bad");
        logf(b);
    }
}

// BeiDou: the ephemeris from D1 subframes 1-3 of one frame or D2 pages 1-10; BDT - GPS from D1 subframe 5 page 9
void GnssReceiver::Impl::handleBds(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow) {
    BdsNav& n = bnav[t.prn];
    SatNav& s = bsat[t.prn];
    s.towLast = (int)ev.towStart;
    s.lastSfId = (int)bdsFraId(ev.bits); s.lastSfTime = signalNow;
    GpsEphemeris e;
    int wk = 0;
    const bool had = n.gpsValid;
    if (bdsHandleSubframe(n, ev.bits, ev.kind == GnssMsgBdsD2, t.prn, &e, &wk)) {
        if (!s.hasEph || s.eph.iodc != e.iodc || s.eph.toe != e.toe || s.eph.health != e.health) {
            s.eph = e; s.hasEph = true; s.week = wk; s.ephSignalTime = signalNow;
            char b[120];
            snprintf(b, sizeof b, "%s ephemeris AODE %d, BDT week %d, health %s", satName(GnssBeidou, t.prn).c_str(), e.iodc, wk, e.health ? "bad" : "ok");
            logf(b);
        }
    }
    if (n.gpsValid && !had) {
        if (!bdtGpsValid) {
            char b[100];
            snprintf(b, sizeof b, "BDT - GPS time offset %+.1f ns (from %s)", n.a0gps * 1e9, satName(GnssBeidou, t.prn).c_str());
            logf(b);
        }
        bdtGpsValid = true; bdtA0 = n.a0gps; bdtA1 = n.a1gps;
    }
}

// Galileo I/NAV words: the ephemeris from words 1-4 of one IODnav and word 5 (BGD, health, week); the GST-GPS offset from word 10
void GnssReceiver::Impl::handleInav(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow) {
    GalNav& g = gnav[t.prn];
    SatNav& s = gsat[t.prn];
    int tow = -1;
    const int type = inavParseWord(ev.word, g, &tow);
    if (tow >= 0) s.towLast = tow;
    s.lastSfId = type; s.lastSfTime = signalNow;
    {   // SAR return link messages, joined with Cospas-Sarsat: the 22 bit SAR field of every odd page
        GalRlm rlm;
        if (rlmAsm[t.prn].push(ev.sar, rlm)) {
            char b[160];
            snprintf(b, sizeof b, "Galileo SAR return link from %s: beacon %s, %s, parity %s", satName(GnssGalileo, t.prn).c_str(), rlm.hex,
                     rlm.ack ? "acknowledgement" : rlm.test ? "test" : (rlm.longRlm ? "long RLM" : "short RLM"), rlm.parityOk ? "ok" : "BAD");
            logf(b);
        }
    }
    if (ev.osnmaPage == 0 && ev.nma8 != lastNma) {
        lastNma = ev.nma8;
        char b[140];
        snprintf(b, sizeof b, "OSNMA header: service %s, chain %d, %s (from %s; signatures not checked)", osnmaStatusName(ev.nma8 >> 6), (ev.nma8 >> 4) & 3, osnmaChainStatusName((ev.nma8 >> 1) & 7), satName(GnssGalileo, t.prn).c_str());
        logf(b);
    }
    if (type == 10 && g.ggtoValid) {
        if (!ggtoValid) {
            char b[100];
            snprintf(b, sizeof b, "GST - GPS time offset %+.1f ns (from %s)", g.a0g * 1e9, satName(GnssGalileo, t.prn).c_str());
            logf(b);
        }
        ggtoValid = true; ggtoA0 = g.a0g; ggtoA1 = g.a1g; ggtoT0 = g.t0g; ggtoWn0 = g.wn0g; ggtoWn = g.wn >= 0 ? g.wn : ggtoWn0;
    }
    if (g.complete() && g.wn >= 0) {
        GpsEphemeris e = g.eph;
        e.prn = t.prn; e.galileo = true; e.has1 = e.has2 = e.has3 = true;
        e.iode2 = e.iode3 = e.iodc = g.iod[1];
        e.tgd = g.bgdE1E5b;
        e.health = (g.e1bHs != 0 || g.e1bDvs != 0) ? 1 : 0;
        // the GST week counts from August 1999 (GPS week 1024), 12 bits
        const int ref = referenceWeek();
        int wk = g.wn + 1024;
        while (wk + 2048 <= ref) wk += 4096;
        if (!s.hasEph || s.eph.iodc != e.iodc || s.eph.toe != e.toe || s.eph.health != e.health) {
            s.eph = e; s.hasEph = true; s.week = wk; s.ephSignalTime = signalNow;
            char b[120];
            snprintf(b, sizeof b, "%s ephemeris IODnav %d, week %d, health %s", satName(GnssGalileo, t.prn).c_str(), e.iodc, wk, e.health ? "bad" : "ok");
            logf(b);
        }
    }
}

void GnssReceiver::Impl::handleSubframe(GnssTracker& t, const GnssSubframeEvent& ev, double signalNow) {
    if (ev.kind == GnssMsgInav) { handleInav(t, ev, signalNow); return; }
    if (ev.kind == GnssMsgBdsD1 || ev.kind == GnssMsgBdsD2) { handleBds(t, ev, signalNow); return; }
    if (ev.kind == GnssMsgGlo) { handleGlo(t, ev, signalNow); return; }
    SatNav& s = sat[t.prn];
    if (ev.kind == GnssMsgSbas) {
        // SBAS: the message type is reported; the corrections are not applied
        const bool fresh = ev.sbasType < 32 ? !(s.sbasTypes & (1u << ev.sbasType)) : false;
        s.sbasLast = ev.sbasType; s.sbasCount++;
        if (ev.sbasType < 32) s.sbasTypes |= 1u << ev.sbasType;
        sbasTotal++;
        if (sbasCb) sbasCb(t.prn, ev.bits, signalNow);
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

void GnssReceiver::Impl::runTrackers(double signalNow) {
    for (auto& t : trackers) {
        const bool wasLocked = t->carrierLocked(), wasBit = t->bitSynced(), wasFrame = t->frameSynced();
        const uint64_t okBefore = t->framesOk();
        const GnssBand& band = bandOf(*t);
        while (t->step(band)) {}
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
        if (trackers[i]->takePhaseDrop()) logf(satName(trackers[i]->sys, trackers[i]->prn) + ": phase lock dropped (lock indicator low for 0.5 s), pulling in again");
        if (trackers[i]->lost()) {
            okRetired += trackers[i]->framesOk(); badRetired += trackers[i]->framesBad();
            // a lost satellite is found again sooner by a short round than at the end of a long one
            if (trackers[i]->band == 0 && longRound && trackers[i]->lockSeconds() > 0.5) { gpsAcq.endRound(); longRound = false; }
            const std::string why = trackers[i]->lostReason();
            logf(satName(trackers[i]->sys, trackers[i]->prn) + (trackers[i]->wasPullInTimeout() ? " not confirmed" : " lost") + (why.empty() ? "" : ": " + why));
            trackers.erase(trackers.begin() + (long)i);
        } else i++;
    }
}

} // namespace dect2
