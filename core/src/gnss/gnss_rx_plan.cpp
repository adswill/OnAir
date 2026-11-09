// GNSS receiver, planning the channels and the search: replan for the tuning and rate, reset, and the acquisition plans.
#include "gnss_rx_impl.h"

namespace dect2 {

// The GPS L1 C/A channel plan for the current tuning and rate
void GnssReceiver::Impl::replanGps(double center, unsigned mask) {
    // SBAS and QZSS without GPS in the mask (the SBAS mode): the one search that GPS normally has runs over their PRNs, the auxiliary one stays empty
    const unsigned auxBits = gnssSystemBit(GnssQzss) | gnssSystemBit(GnssSbas);
    const bool l1Only = !(mask & gnssSystemBit(GnssGps)) && (mask & auxBits);
    bool wantGps = (mask & (gnssSystemBit(GnssGps) | auxBits)) != 0;
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
            if (l1Only) {
                if (mask & gnssSystemBit(GnssSbas)) for (int p = 120; p <= 158; p++) ac.prns.push_back(p);
                if (mask & gnssSystemBit(GnssQzss)) for (int p = 193; p <= 202; p++) ac.prns.push_back(p);
            } else {
                for (int p = 1; p <= 32; p++) ac.prns.push_back(p);
            }
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
            // QZSS and SBAS have a search of their own (runAuxAcq), so the GPS search runs exactly as without them
            ac.prns.clear();
            if (!l1Only) {
                if (mask & gnssSystemBit(GnssQzss)) for (int p = 193; p <= 202; p++) ac.prns.push_back(p);
                if (mask & gnssSystemBit(GnssSbas)) for (int p = 120; p <= 158; p++) ac.prns.push_back(p);
            }
            auxAcq.init(ac);
            auxCredit = 0;
            stage = 0; stageRounds = 0;
            for (int p = 1; p < kCaPrns; p++) l1caChips(p, gpsChips[p]);
            gpsSpec.sys = GnssGps; gpsSpec.chipRate = kGpsCaChipRate; gpsSpec.codeLen = kGpsCaLen; gpsSpec.rfHz = kGpsL1Hz; gpsSpec.fsOut = kGpsFsOut; gpsSpec.halfSpacing = 2;
            qzssSpec = gpsSpec; qzssSpec.sys = GnssQzss;
            sbasSpec = gpsSpec; sbasSpec.sys = GnssSbas; sbasSpec.msg = GnssMsgSbas;
            trackers.clear();
            acqCredit = 0;
            galRebuild = true;
        }
        gpsActive = true;
        gpsMaskNow = mask;
        l1Standalone = l1Only;
    } else {
        gpsActive = false;
        dropBand(0);
    }
}

// Galileo E1-B shares the GPS band; its channels follow
void GnssReceiver::Impl::replanGal(double center, unsigned mask) {
    const double off = kGpsL1Hz - center;
    // Galileo E1-B shares the band; its BOC(1,1) lobes reach 2 MHz from the carrier, so the input must be 3.5 Msps or more
    const bool wantGal = gpsActive && (mask & gnssSystemBit(GnssGalileo)) && rate >= 3.5e6;
    if (wantGal) {
        if (!galActive || galRebuild) {
            GnssAcqConfig ac;
            ac.sys = GnssGalileo; ac.fftLog2 = 14; ac.fsOut = kGpsFsOut; ac.blocks = kGalMs; ac.rfHz = kGpsL1Hz;
            ac.pfa = 1e-3; ac.coherentPairs = false; ac.offsets = 2;
            ac.qMin = -24; ac.qMax = 23;
            for (int p = 1; p < kGalPrns; p++) ac.prns.push_back(p);
            ac.replica = [](int prn, cf32* out, int n) {
                uint8_t c[kGalE1Len];
                if (!galE1bChips(prn, c)) return false;
                for (int i = 0; i < n; i++) {
                    const int h = (int)(((int64_t)i * 2 + 1) * kGalHalfChips / (2 * (int64_t)n));
                    out[i] = cf32((c[h / 2] ^ (h & 1)) ? -1.f : 1.f, 0.f);
                }
                return true;
            };
            galAcq.init(ac);
            for (int p = 1; p < kGalPrns; p++) {
                uint8_t c[kGalE1Len];
                galChips[p].assign((size_t)kGalHalfChips, 0);
                if (galE1bChips(p, c)) for (int h = 0; h < kGalHalfChips; h++) galChips[p][(size_t)h] = (uint8_t)(c[h / 2] ^ (h & 1));
            }
            galSpec.sys = GnssGalileo; galSpec.chipRate = 2 * kGalE1ChipRate; galSpec.codeLen = kGalHalfChips; galSpec.rfHz = kGpsL1Hz; galSpec.fsOut = kGpsFsOut;
            galSpec.halfSpacing = 1;           // a quarter chip: inside the narrow main peak of BOC(1,1)
            galSpec.msg = GnssMsgInav; galSpec.epochS = 4e-3;
            galCredit = 0;
        }
        galActive = true;
    } else {
        if (galActive) trackers.erase(std::remove_if(trackers.begin(), trackers.end(), [](const std::unique_ptr<GnssTracker>& t) { return t->sys == GnssGalileo; }), trackers.end());
        galActive = false;
    }
    galRebuild = false;
}

// BeiDou B1I on its own carrier
void GnssReceiver::Impl::replanBds(double center, unsigned mask) {
    // BeiDou B1I, 1561.098 MHz: the main lobe (+-2.046 MHz) mostly inside the band; 4 Msps on its own carrier is enough
    const double offB = kBdsB1Hz - center;
    const bool wantBds = (mask & gnssSystemBit(GnssBeidou)) && rate >= 3.9e6 && std::fabs(offB) + 1.0e6 <= 0.5 * rate;
    if (wantBds) {
        if (!bdsActive || std::fabs(bdsBand.offsetHz() - offB) > 1.0 || lastRate != rate) {
            dropBand(1);
            bdsBand.init(rate, offB, kGpsFsOut, (size_t)1 << 19);
            GnssAcqConfig ac;
            ac.sys = GnssBeidou; ac.fftLog2 = 12; ac.fsOut = kGpsFsOut; ac.blocks = kShortMs; ac.rfHz = kBdsB1Hz;
            ac.pfa = 1e-3;
            ac.coherentPairs = false;          // the NH code turns the D1 symbols over from one code period to the next
            const int q = (int)std::ceil(dopHalf.load() / 1000.0);
            ac.qMin = -q; ac.qMax = q - 1;
            for (int p = 1; p <= kBdsMaxPrn; p++) ac.prns.push_back(p);
            ac.replica = [](int prn, cf32* out, int n) {
                uint8_t c[kBdsLen];
                if (!bdsB1iChips(prn, c)) return false;
                for (int i = 0; i < n; i++) {
                    const int idx = (int)(((int64_t)i * 2 + 1) * kBdsLen / (2 * (int64_t)n));
                    out[i] = cf32(c[idx] ? -1.f : 1.f, 0.f);
                }
                return true;
            };
            bdsAcq.init(ac);
            for (int p = 1; p <= kBdsMaxPrn; p++) { bdsChips[p].assign((size_t)kBdsLen, 0); bdsB1iChips(p, bdsChips[p].data()); }
            bdsD1Spec.sys = GnssBeidou; bdsD1Spec.chipRate = kBdsChipRate; bdsD1Spec.codeLen = kBdsLen; bdsD1Spec.rfHz = kBdsB1Hz; bdsD1Spec.fsOut = kGpsFsOut;
            bdsD1Spec.halfSpacing = 1;          // half a chip at two samples a chip
            bdsD1Spec.msg = GnssMsgBdsD1; bdsD1Spec.epochS = 1e-3;
            bdsD2Spec = bdsD1Spec; bdsD2Spec.msg = GnssMsgBdsD2;
            bdsCredit = 0; bdsPlan = SearchPlan();
        }
        bdsActive = true;
    } else {
        if (bdsActive) dropBand(1);
        bdsActive = false;
    }
}

// GLONASS L1OF: the frequency channels whose lobe lies in the band
void GnssReceiver::Impl::replanGlo(double center, unsigned mask) {
    // GLONASS L1OF: the frequency channels whose code main lobe (+-0.511 MHz) lies in the band; a radio too slow for all fourteen gets the middle ones
    {
        bool fitK[kGloSlots] = {};
        bool any = false;
        for (int i = 0; i < kGloSlots; i++) {
            const double off = gloL1Hz(i + kGloMinK) - center;
            fitK[i] = (mask & gnssSystemBit(GnssGlonass)) && rate >= 1.9e6 && std::fabs(off) + 0.55e6 <= 0.5 * rate;
            any = any || fitK[i];
        }
        bool same = gloActive && lastRate == rate && std::fabs(gloCenterNow - center) < 1.0;
        for (int i = 0; i < kGloSlots && same; i++) same = gloSlotOn[i] == fitK[i];
        if (any && !same) {
            for (int b = 2; b < 2 + kGloSlots; b++) dropBand(b);
            // above 16 Msps a wide band around the channels is cut first: fourteen narrow bands from it cost less than from the input
            gloUseWide = rate > 16e6;
            double lo = 1e18, hi = -1e18;
            for (int i = 0; i < kGloSlots; i++) if (fitK[i]) { const double off = gloL1Hz(i + kGloMinK) - center; lo = std::min(lo, off); hi = std::max(hi, off); }
            const double wideC = 0.5 * (lo + hi), wideFs = 1.024e6 * std::ceil((hi - lo + 1.4e6) / 1.024e6);
            if (gloUseWide) { gloWide.init(rate, wideC, wideFs, (size_t)1 << 16); gloWideFed = 0; }
            gloCaChips(gloChips);
            for (int i = 0; i < kGloSlots; i++) {
                gloSlotOn[i] = fitK[i];
                if (!fitK[i]) continue;
                const int k = i + kGloMinK;
                const double off = gloL1Hz(k) - center;
                if (gloUseWide) gloBand[i].init(wideFs, off - wideC, kGloFsOut, (size_t)1 << 17);
                else gloBand[i].init(rate, off, kGloFsOut, (size_t)1 << 17);
                gloBand[i].setExcision(false);
                GnssAcqConfig ac;
                ac.sys = GnssGlonass; ac.fftLog2 = 10; ac.fsOut = kGloFsOut; ac.blocks = kShortMs; ac.rfHz = gloL1Hz(k);
                ac.pfa = 1e-3;
                ac.coherentPairs = false;      // the meander turns the symbols over every 10 ms
                const int q = (int)std::ceil(dopHalf.load() / 1000.0);
                ac.qMin = -q; ac.qMax = q - 1;
                ac.prns.push_back(i + 1);
                ac.replica = [](int, cf32* out, int n) {
                    uint8_t c[kGloLen];
                    gloCaChips(c);
                    for (int j = 0; j < n; j++) {
                        const int idx = (int)(((int64_t)j * 2 + 1) * kGloLen / (2 * (int64_t)n));
                        out[j] = cf32(c[idx] ? -1.f : 1.f, 0.f);
                    }
                    return true;
                };
                gloAcq[i].init(ac);
                GnssSignalSpec& sp = gloSpec[i];
                sp.sys = GnssGlonass; sp.chipRate = kGloChipRate; sp.codeLen = kGloLen; sp.rfHz = gloL1Hz(k); sp.fsOut = kGloFsOut;
                sp.halfSpacing = 1;          // half a chip at two samples a chip
                sp.msg = GnssMsgGlo; sp.epochS = 1e-3;
            }
            gloCredit = 0; gloPlan = SearchPlan(); gloCenterNow = center;
        }
        if (!any && gloActive) for (int b = 2; b < 2 + kGloSlots; b++) dropBand(b);
        if (!any) for (auto& f : gloSlotOn) f = false;
        gloActive = any;
    }
}

// ------------------------------------------------------------------ set-up
void GnssReceiver::Impl::replan() {
    planGen = settingsGen.load();
    const double center = centerMhz.load() * 1e6;
    const unsigned mask = sysMask.load();
    replanGps(center, mask);
    replanGal(center, mask);
    replanBds(center, mask);
    replanGlo(center, mask);
    lastRate = rate;
    tel.activeMask = activeMask();
}

void GnssReceiver::Impl::resetState() {
    pend.clear();
    dcI = dcQ = 0; gain = 1; dcInit = false; levelRms = 0; clipFrac = 0; inSamples = 0;
    wallMinCur = wallMinPrev = 1e300; wallWin = 0; offWin.clear(); offHist.clear(); levHist.clear(); snrHist.clear(); offNow = NAN; clkValid = false; histNext = 0;
    interf.configure(rate);
    interf.setLogCallback([this](const std::string& s) { logf(s); });
    gpsBand.reset();
    gpsAcq.reset();
    auxAcq.reset();
    auxRoundSeen = 0; auxEmptyRounds = 0; auxIdleUntil = 0; auxRoundHits = 0;
    galAcq.reset();
    trackers.clear();
    for (auto& s : sat) s = SatNav();
    for (auto& s : gsat) s = SatNav();
    for (auto& g : gnav) g = GalNav();
    for (auto& a : rlmAsm) a.reset();
    for (auto& s : gsol) s = SolInfo();
    ggtoValid = false; galCredit = 0; predictedGal.clear();
    galRoundSeen = 0; galLong = false; galRoundHits = 0; galEmptyRounds = 0; galIdleUntil = 0; codeRateFrac = 0;
    bdsBand.reset(); bdsAcq.reset();
    for (auto& s : bsat) s = SatNav();
    for (auto& n : bnav) n = BdsNav();
    for (auto& s : bsol) s = SolInfo();
    bdtGpsValid = false; bdsCredit = 0; bdsPlan = SearchPlan(); bdsRoundSeen = 0; bdsRoundHits = 0; bdsEmptyRounds = 0; bdsIdleUntil = 0;
    gloWide.reset(); gloWideFed = 0;
    for (int i = 0; i < kGloSlots; i++) { gloBand[i].reset(); gloAcq[i].reset(); gloSat[i] = SatNav(); gloNav[i] = GloNav(); gloEph[i] = GloEphemeris(); glsol[i] = SolInfo(); }
    gloTimeValid = false; gloTauGps = gloTauC = 0; gloN4 = 0; gloCredit = 0; gloPlan = SearchPlan(); gloNext = 0;
    gloRoundSeen = 0; gloRoundHits = 0; gloEmptyRounds = 0; gloIdleUntil = 0;
    iono = GpsIono(); utc = GpsUtc(); almToa = -1; almWna = -1; almGot = false;
    fix = GnssFix(); clockSet = false; hasPrior = false; nextMeas = 1.0; fixCount = 0; firstFix = -1; lastFixSignal = -1;
    okRetired = badRetired = 0; cfoEst = driftEst = 0; cfoValid = false; sbasTotal = 0;
    stage = 0; stageRounds = 0; firstLock = -1; roundHits = lastRoundHits = 0; longRound = false;
    for (auto& s : sol) s = SolInfo();
    nextReport = 0.25; acqCredit = 0; havePredicted = false;
    tel = GnssTelemetry(); telReady = false;
}

// Where and how long to search next. With satellites locked the radio's error lies within 5.5 kHz of each of their Dopplers, so the others are within
// 11 kHz of all of them; with a fix it is measured. With nothing locked the window widens round by round (TCXO, HackRF, cheap dongle), then the
// same with a four times longer integration for weak signals, and starts over.
void GnssReceiver::Impl::planSearch() {
    double lo = 1e18, hi = -1e18;
    int nLocked = 0, nPull = 0;
    for (auto& t : trackers) {
        if (t->band != 0) continue;
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

// The same plan for a band of its own (BeiDou or GLONASS tuned without L1): its own channels say where the radio's error lies, else the
// window widens round by round. Frequencies scale with the carrier: the remembered error is at L1.
void GnssReceiver::Impl::planOwn(GnssAcq& acq, SearchPlan& pl, int sys, double rf, const char* name) {
    double lo = 1e18, hi = -1e18;
    int nLocked = 0, nPull = 0;
    for (auto& t : trackers) {
        if (t->sys != sys) continue;
        if (t->carrierLocked()) { lo = std::min(lo, t->dopplerHz()); hi = std::max(hi, t->dopplerHz()); nLocked++; }
        else nPull++;
    }
    const uint32_t r = acq.rounds();
    double c = 0, h = dopHalf.load();
    int ms = kShortMs;
    const bool newRound = r != pl.stageRounds;
    if (newRound) { pl.lastRoundHits = pl.roundHits; pl.roundHits = 0; }
    if (nLocked > 0) {
        c = 0.5 * (lo + hi); h = std::max(kSatDopplerHz, 2 * kSatDopplerHz - 0.5 * (hi - lo)) + 500.0;
        pl.lastCfo = c; pl.lastCfoValid = true;
        pl.stage = 0; pl.stageRounds = r;
        if (newRound) pl.longRound = !pl.longRound && pl.lastRoundHits == 0;
        if (pl.longRound) ms = kLongMs;
    } else {
        pl.longRound = false;
        if (newRound) {
            pl.stageRounds = r;
            if (nPull == 0) {
                pl.stage = (pl.stage + 1) % 6;
                static const char* what[6] = {"", "+-45 kHz", "+-170 kHz", "with a 64 ms integration", "+-45 kHz with a 64 ms integration", "+-170 kHz with a 64 ms integration"};
                if (pl.stage > 0) logf(std::string(name) + ": nothing found yet, searching " + what[pl.stage]);
            }
        }
        const int st = pl.stage % 3;
        if (st == 0) {
            if (pl.lastCfoValid) c = pl.lastCfo; else if (cfoHintValid.load()) c = cfoHintHz.load() * rf / kGpsL1Hz;
        } else h = st == 1 ? kMidHz : kWideHz;
        if (pl.stage >= 3) ms = kLongMs;
    }
    acq.setPlan(c, h, ms);
}

// The sample clock error against the code, from the GPS channels whose code loop has settled (median); false while there is none
bool GnssReceiver::Impl::updateCodeRate(int sys, double chipRate) {
    std::vector<double> e;
    for (auto& t : trackers) if (t->sys == sys && t->carrierLocked() && t->lockSeconds() > 3.0) e.push_back(t->codeRateCorrection() / chipRate);
    if (e.empty()) return false;
    std::sort(e.begin(), e.end());
    codeRateFrac = e[e.size() / 2];
    return true;
}

void GnssReceiver::Impl::trimBands() {
    if (gpsActive) {
        int64_t keepFrom = gpsBand.end();
        for (auto& t : trackers) if (t->band == 0) keepFrom = std::min(keepFrom, t->position());
        gpsBand.trimTo(keepFrom - 8192);
    }
    if (bdsActive) {
        int64_t keepFrom = bdsBand.end();
        for (auto& t : trackers) if (t->band == 1) keepFrom = std::min(keepFrom, t->position());
        bdsBand.trimTo(keepFrom - 8192);
    }
    for (int i = 0; i < kGloSlots && gloActive; i++) {
        if (!gloSlotOn[i]) continue;
        int64_t keepFrom = gloBand[i].end();
        for (auto& t : trackers) if (t->band == 2 + i) keepFrom = std::min(keepFrom, t->position());
        gloBand[i].trimTo(keepFrom - 2048);
    }
}

} // namespace dect2
