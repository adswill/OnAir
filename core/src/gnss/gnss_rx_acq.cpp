// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS receiver, starting channels from acquisition hits and running the per-system acquisitions.
#include "gnss_rx_impl.h"
#include "dect2/common/diag.h"

namespace dect2 {

void GnssReceiver::Impl::startFromHit(const GnssAcqHit& found) {
    if (trackers.size() >= (size_t)kMaxChannels) {
        DIAG_DEBUG_EVERY(191602, 10.0, "All %d tracking channels are in use, so a new GPS acquisition hit on PRN %d is ignored.", kMaxChannels, found.prn);
        return;
    }
    if (tracked(found.prn)) return;
    // the hit is old: look again in the newest samples
    GnssAcqHit h;
    units += (uint64_t)(6 * std::max(8, found.blocks));
    if (!(found.prn > 32 && !l1Standalone ? auxAcq : gpsAcq).confirm(gpsBand, found.prn, found.dopplerHz, found.blocks, &h)) {
        DIAG_DEBUG_EVERY(595975, 10.0, "A GPS acquisition hit on PRN %d at %+.0f Hz was not confirmed by a second look, so it is dropped as a false alarm.", found.prn, found.dopplerHz);
        return;
    }
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

void GnssReceiver::Impl::startGalFromHit(const GnssAcqHit& found) {
    if (trackers.size() >= (size_t)kMaxChannels) {
        DIAG_DEBUG_EVERY(809077, 10.0, "All %d tracking channels are in use, so a new Galileo acquisition hit on PRN %d is ignored.", kMaxChannels, found.prn);
        return;
    }
    if (trackedGal(found.prn)) return;
    GnssAcqHit h;
    units += (uint64_t)(4 * 6 * std::max(8, found.blocks));
    if (!galAcq.confirm(gpsBand, found.prn, found.dopplerHz, found.blocks, &h)) {
        DIAG_DEBUG_EVERY(607479, 10.0, "A Galileo acquisition hit on PRN %d at %+.0f Hz was not confirmed by a second look, so it is dropped as a false alarm.", found.prn, found.dopplerHz);
        return;
    }
    auto t = std::make_unique<GnssTracker>();
    const double rc = galSpec.chipRate * (1.0 + h.dopplerHz / kGpsL1Hz + codeRateFrac) / kGpsFsOut;
    int64_t idx = gpsBand.end() - 2 * kGalN;
    if (idx < gpsBand.base()) idx = gpsBand.base();
    double ph = std::fmod(((double)idx - ((double)h.segStart + h.codePhase)) * rc, (double)kGalHalfChips);
    if (ph < 0) ph += kGalHalfChips;
    t->start(galSpec, galChips[h.prn].data(), h.prn, h.dopplerHz, idx, ph);
    t->setCodeRateHint(codeRateFrac * galSpec.chipRate);
    char b[120];
    snprintf(b, sizeof b, "%s found: Doppler %+.0f Hz, peak %.1f times the noise", satName(GnssGalileo, h.prn).c_str(), h.dopplerHz, h.ratio);
    logf(b);
    trackers.push_back(std::move(t));
}

void GnssReceiver::Impl::startBdsFromHit(const GnssAcqHit& found) {
    if (trackers.size() >= (size_t)kMaxChannels) {
        DIAG_DEBUG_EVERY(247185, 10.0, "All %d tracking channels are in use, so a new BeiDou acquisition hit on PRN %d is ignored.", kMaxChannels, found.prn);
        return;
    }
    if (trackedSys(GnssBeidou, found.prn)) return;
    GnssAcqHit h;
    units += (uint64_t)(6 * std::max(8, found.blocks));
    if (!bdsAcq.confirm(bdsBand, found.prn, found.dopplerHz, found.blocks, &h)) {
        DIAG_DEBUG_EVERY(403091, 10.0, "A BeiDou acquisition hit on PRN %d at %+.0f Hz was not confirmed by a second look, so it is dropped as a false alarm.", found.prn, found.dopplerHz);
        return;
    }
    auto t = std::make_unique<GnssTracker>();
    // A sample clock that does not follow the oscillator moves the 2.046 Mcps code by 41 chips a second at 20 ppm: more than the code loop
    // pulls in. The hit and its confirmation are two code phase readings far apart: the code periods between them give the code rate.
    double frac = codeRateFrac;
    {
        const double D = ((double)h.segStart + h.codePhase) - ((double)found.segStart + found.codePhase);
        const double aided = 1.0 + h.dopplerHz / kBdsB1Hz;
        if (D > 0.05 * kGpsFsOut) {
            const double m = std::round(D * aided / kBdsN);
            const double f = m * kBdsN / D - aided;
            if (m > 0 && std::fabs(f) < 1e-4) frac = f;
        }
    }
    const double rc = kBdsChipRate * (1.0 + h.dopplerHz / kBdsB1Hz + frac) / kGpsFsOut;
    int64_t idx = bdsBand.end() - 3 * kBdsN;
    if (idx < bdsBand.base()) idx = bdsBand.base();
    double ph = std::fmod(((double)idx - ((double)h.segStart + h.codePhase)) * rc, (double)kBdsLen);
    if (ph < 0) ph += kBdsLen;
    t->start(bdsIsGeo(h.prn) ? bdsD2Spec : bdsD1Spec, bdsChips[h.prn].data(), h.prn, h.dopplerHz, idx, ph);
    t->band = 1;
    if (frac != 0) t->setCodeRateHint(frac * kBdsChipRate);
    char b[120];
    snprintf(b, sizeof b, "%s found: Doppler %+.0f Hz, peak %.1f times the noise", satName(GnssBeidou, h.prn).c_str(), h.dopplerHz, h.ratio);
    logf(b);
    trackers.push_back(std::move(t));
}

void GnssReceiver::Impl::startGloFromHit(int slot, const GnssAcqHit& found) {
    if (trackers.size() >= (size_t)kMaxChannels) {
        DIAG_DEBUG_EVERY(501515, 10.0, "All %d tracking channels are in use, so a new GLONASS acquisition hit on PRN %d is ignored.", kMaxChannels, found.prn);
        return;
    }
    if (trackedGlo(slot)) return;
    GnssAcqHit h;
    units += (uint64_t)(2 * std::max(8, found.blocks));
    GnssBand& band = gloBand[slot];
    if (!gloAcq[slot].confirm(band, found.prn, found.dopplerHz, found.blocks, &h)) {
        DIAG_DEBUG_EVERY(767767, 10.0, "A GLONASS acquisition hit on PRN %d at %+.0f Hz was not confirmed by a second look, so it is dropped as a false alarm.", found.prn, found.dopplerHz);
        return;
    }
    const int k = slot + kGloMinK;
    const double f = gloL1Hz(k);
    // the code rate from the hit and its confirmation (see startBdsFromHit)
    double frac = codeRateFrac;
    {
        const double D = ((double)h.segStart + h.codePhase) - ((double)found.segStart + found.codePhase);
        const double aided = 1.0 + h.dopplerHz / f;
        if (D > 0.05 * kGloFsOut) {
            const double m = std::round(D * aided / kGloN);
            const double fr = m * kGloN / D - aided;
            if (m > 0 && std::fabs(fr) < 1e-4) frac = fr;
        }
    }
    auto t = std::make_unique<GnssTracker>();
    const double rc = kGloChipRate * (1.0 + h.dopplerHz / f + frac) / kGloFsOut;
    int64_t idx = band.end() - 3 * kGloN;
    if (idx < band.base()) idx = band.base();
    double ph = std::fmod(((double)idx - ((double)h.segStart + h.codePhase)) * rc, (double)kGloLen);
    if (ph < 0) ph += kGloLen;
    t->start(gloSpec[slot], gloChips, gloSat[slot].hasEph ? gloEph[slot].n : 0, h.dopplerHz, idx, ph);
    t->fcn = k;
    t->band = 2 + slot;
    if (frac != 0) t->setCodeRateHint(frac * kGloChipRate);
    char b[120];
    snprintf(b, sizeof b, "%s found: Doppler %+.0f Hz, peak %.1f times the noise", gloName(t->prn, k).c_str(), h.dopplerHz, h.ratio);
    logf(b);
    trackers.push_back(std::move(t));
}

// Galileo: searched once GPS has a fix. The receiver's frequency error is then measured, so the window is the satellites' Doppler alone
// (+-5 kHz), and a sky without Galileo costs little (the search sleeps after a round without a find).
void GnssReceiver::Impl::runGalAcq(double seconds, double signalNow) {
    if (!galActive || !fix.valid || !cfoValid) return;
    // A round that finds nothing while no Galileo satellite is tracked (a sky without Galileo, or a receiver under a roof) puts the search to sleep
    // for a while, longer each time (10 s, 20 s, .. 80 s). With Galileo satellites tracked, every third round integrates three times as long for the
    // weak ones.
    const uint32_t r = galAcq.rounds();
    int nGal = 0;
    for (auto& t : trackers) nGal += t->sys == GnssGalileo;
    if (r != galRoundSeen) {
        galRoundSeen = r;
        if (galRoundHits == 0 && nGal == 0) {
            galIdleUntil = signalNow + 10.0 * (1 << std::min(galEmptyRounds, 3));
            galEmptyRounds++;
        } else galEmptyRounds = 0;
        galRoundHits = 0;
        galLong = nGal > 0 && r % 3 == 2;
    }
    if (signalNow < galIdleUntil) return;
    // the sample clock error against the code, from the GPS channels whose code loop has settled (median)
    {
        std::vector<double> e;
        for (auto& t : trackers) if (t->sys == GnssGps && t->carrierLocked() && t->lockSeconds() > 3.0) e.push_back(t->codeRateCorrection() / kGpsCaChipRate);
        // not before a GPS code loop has settled: a sample clock 20 ppm off smears the narrow BOC peak within one search
        if (e.empty()) return;
        std::sort(e.begin(), e.end());
        codeRateFrac = e[e.size() / 2];
        galAcq.setCodeRateError(codeRateFrac);
    }
    galAcq.setPlan(cfoEst, 5000.0, galLong ? 3 * kGalMs : kGalMs);
    // a unit is one FFT of 16384 points here, about five times the GPS one
    galCredit = std::min(galCredit + seconds * 1000.0 * (double)acqRate.load() / 3.0, 300.0);
    if (galCredit < 8) return;
    std::vector<GnssAcqHit> hits;
    const int used = galAcq.work(gpsBand, (int)galCredit, [this](int p) { return trackedGal(p); }, hits);
    galCredit -= used;
    units += (uint64_t)used * 4;
    galRoundHits += (int)hits.size();
    for (auto& h : hits) startGalFromHit(h);
}

// QZSS and SBAS: their own search, once a GPS satellite is locked (in the window the GPS search uses), with half the GPS search's work.
// A round that finds nothing while none of them is tracked puts it to sleep (10 s, 20 s, .. 80 s).
void GnssReceiver::Impl::runAuxAcq(double seconds, double signalNow) {
    if (!gpsLocked() || !(gpsMaskNow & (gnssSystemBit(GnssQzss) | gnssSystemBit(GnssSbas)))) return;
    const uint32_t r = auxAcq.rounds();
    int nAux = 0;
    for (auto& t : trackers) nAux += t->sys == GnssQzss || t->sys == GnssSbas;
    if (r != auxRoundSeen) {
        auxRoundSeen = r;
        if (auxRoundHits == 0 && nAux == 0) { auxIdleUntil = signalNow + 10.0 * (1 << std::min(auxEmptyRounds, 3)); auxEmptyRounds++; }
        else auxEmptyRounds = 0;
        auxRoundHits = 0;
    }
    if (signalNow < auxIdleUntil) return;
    // 8 ms: QZSS and SBAS are as strong as GPS; the weak ones come in the rounds after the first
    auxAcq.setPlan(gpsAcq.windowCenterHz(), gpsAcq.windowHalfHz(), auxEmptyRounds == 0 && r % 2 == 1 ? kShortMs : kShortMs / 2);
    auxCredit = std::min(auxCredit + seconds * 1000.0 * (double)acqRate.load() / 2.0, 300.0);
    if (auxCredit < 8) return;
    std::vector<GnssAcqHit> hits;
    const int used = auxAcq.work(gpsBand, (int)auxCredit, [this](int p) { return tracked(p); }, hits);
    auxCredit -= used;
    units += (uint64_t)used;
    auxRoundHits += (int)hits.size();
    for (auto& h : hits) startFromHit(h);
}

// BeiDou: with L1 in the same tune it is searched once GPS has a fix, in the window of the measured frequency error (as Galileo); on a tune of
// its own it searches like GPS does, the window widening while nothing is found.
void GnssReceiver::Impl::runBdsAcq(double seconds, double signalNow) {
    if (!bdsActive) return;
    int nB = 0, nLocked = 0;
    for (auto& t : trackers) if (t->sys == GnssBeidou) { nB++; nLocked += t->carrierLocked(); }
    double rateNow = (double)acqRate.load();
    if (gpsActive) {
        // once a GPS satellite is locked the radio's error is known to within the window of the GPS search; with a fix it is measured
        if (!gpsLocked()) return;
        const uint32_t r = bdsAcq.rounds();
        if (r != bdsRoundSeen) {
            bdsRoundSeen = r;
            if (bdsRoundHits == 0 && nB == 0) { bdsIdleUntil = signalNow + 10.0 * (1 << std::min(bdsEmptyRounds, 3)); bdsEmptyRounds++; }
            else bdsEmptyRounds = 0;
            bdsRoundHits = 0;
        }
        if (signalNow < bdsIdleUntil) return;
        if (updateCodeRate()) bdsAcq.setCodeRateError(codeRateFrac);
        const double sc = kBdsB1Hz / kGpsL1Hz;
        if (fix.valid && cfoValid) bdsAcq.setPlan(cfoEst * sc, kSatDopplerHz, nB > 0 && r % 3 == 2 ? kLongMs / 2 : kShortMs);
        else bdsAcq.setPlan(gpsAcq.windowCenterHz() * sc, gpsAcq.windowHalfHz(), kShortMs);
        rateNow *= 0.5;
    } else {
        planOwn(bdsAcq, bdsPlan, GnssBeidou, kBdsB1Hz, "BeiDou");
        if (updateCodeRate(GnssBeidou, kBdsChipRate)) bdsAcq.setCodeRateError(codeRateFrac);
        if (nLocked == 0) rateNow *= 2.0;
    }
    bdsCredit = std::min(bdsCredit + seconds * 1000.0 * rateNow, 600.0);
    if (bdsCredit < 8) return;
    std::vector<GnssAcqHit> hits;
    const int used = bdsAcq.work(bdsBand, (int)bdsCredit, [this](int p) { return trackedSys(GnssBeidou, p); }, hits);
    bdsCredit -= used;
    units += (uint64_t)used;
    bdsRoundHits += (int)hits.size();
    bdsPlan.roundHits += (int)hits.size();
    for (auto& h : hits) startBdsFromHit(h);
}

// GLONASS: every frequency channel has its own search of the one code. With L1 in the tune they follow the GPS window (scaled to 1602 MHz)
// once GPS is locked; on a tune of their own they share one widening plan. The work goes round the channels.
void GnssReceiver::Impl::runGloAcq(double seconds, double signalNow) {
    if (!gloActive) return;
    int first = -1, nActive = 0, nG = 0, nLocked = 0;
    for (int i = 0; i < kGloSlots; i++) if (gloSlotOn[i]) { if (first < 0) first = i; nActive++; }
    if (first < 0) return;
    for (auto& t : trackers) if (t->sys == GnssGlonass) { nG++; nLocked += t->carrierLocked(); }
    double rateNow = (double)acqRate.load();
    double c, h;
    int ms;
    const double sc = kGloL1Hz / kGpsL1Hz;
    if (gpsActive) {
        if (!gpsLocked()) return;
        const uint32_t r = gloAcq[first].rounds();
        if (r != gloRoundSeen) {
            gloRoundSeen = r;
            if (gloRoundHits == 0 && nG == 0) { gloIdleUntil = signalNow + 10.0 * (1 << std::min(gloEmptyRounds, 3)); gloEmptyRounds++; }
            else gloEmptyRounds = 0;
            gloRoundHits = 0;
        }
        if (signalNow < gloIdleUntil) return;
        if (updateCodeRate()) for (int i = 0; i < kGloSlots; i++) gloAcq[i].setCodeRateError(codeRateFrac);
        if (fix.valid && cfoValid) { c = cfoEst * sc; h = kSatDopplerHz; } else { c = gpsAcq.windowCenterHz() * sc; h = gpsAcq.windowHalfHz(); }
        ms = kShortMs;
        rateNow *= 0.5;
    } else {
        planOwn(gloAcq[first], gloPlan, GnssGlonass, kGloL1Hz, "GLONASS");
        c = gloAcq[first].windowCenterHz(); h = gloAcq[first].windowHalfHz(); ms = gloAcq[first].blocksNow();
        if (updateCodeRate(GnssGlonass, kGloChipRate)) for (int i = 0; i < kGloSlots; i++) gloAcq[i].setCodeRateError(codeRateFrac);
        if (nLocked == 0) rateNow *= 2.0;
    }
    for (int i = 0; i < kGloSlots; i++) if (gloSlotOn[i] && i != first) gloAcq[i].setPlan(c, h, ms);
    if (gpsActive) gloAcq[first].setPlan(c, h, ms);
    // a unit here is an FFT of 1024 points, a quarter of the GPS one
    gloCredit = std::min(gloCredit + seconds * 1000.0 * rateNow * 4.0, 2400.0);
    if (gloCredit < 8) return;
    const int share = std::max(8, (int)(gloCredit / nActive));
    for (int n = 0; n < kGloSlots && gloCredit >= 8; n++) {
        const int i = (gloNext + n) % kGloSlots;
        if (!gloSlotOn[i]) continue;
        if (trackedGlo(i)) continue;
        std::vector<GnssAcqHit> hits;
        const int used = gloAcq[i].work(gloBand[i], std::min(share, (int)gloCredit), [](int) { return false; }, hits);
        gloCredit -= used;
        units += (uint64_t)(used + 3) / 4;
        gloRoundHits += (int)hits.size();
        gloPlan.roundHits += (int)hits.size();
        for (auto& hh : hits) startGloFromHit(i, hh);
    }
    gloNext = (gloNext + 1) % kGloSlots;
}

void GnssReceiver::Impl::runAcq(double seconds) {
    runBdsAcq(seconds, (double)inSamples / rate);
    runGloAcq(seconds, (double)inSamples / rate);
    if (!gpsActive) return;
    planSearch();
    int nLocked = 0;
    for (auto& t : trackers) nLocked += t->band == 0 && t->carrierLocked();
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
    runAuxAcq(seconds, (double)inSamples / rate);
    runGalAcq(seconds, (double)inSamples / rate);
}

} // namespace dect2
