// Analog TV picture decoder: colour burst measurement, the colour lock and SECAM colour.
#include "atv_video_impl.h"

namespace dect2 {

// The burst against a reference, in the normalised video domain (p = (B - r) / G, so the complex amplitude flips sign). The burst sits on
// the carrier at blanking level, a constant in the complex baseband: that is taken away first.
bool AtvVideo::Impl::measureBurst(double tL, const Lock& L, std::complex<double>& b, double& nb2) const {
    const AtvFormat& F = fmt;
    const double bsUs = F.burstStartUs > 0 ? F.burstStartUs : 5.6;
    const double a = tL + (bsUs + 0.45) * spu, e = tL + (bsUs + 1.95) * spu;      // inside both the 10-cycle PAL and the 9-cycle NTSC burst
    const uint64_t na = (uint64_t)std::ceil(a), nb = (uint64_t)std::floor(e);
    if (nb <= na + 6) return false;
    const double g = lineGain();
    if (g <= 0) return false;
    // the same measurement on the back porch after the burst (nothing there but noise): the size of the noise in the burst measurement
    const double a2 = tL + (bsUs + 2.7) * spu, e2 = tL + (bsUs + 3.9) * spu;
    const uint64_t na2 = (uint64_t)std::ceil(a2), nb2i = (uint64_t)std::floor(e2);
    auto project = [&](uint64_t n0, uint64_t n1) {
        std::complex<double> mean = 0;
        double wsum = 0;
        for (uint64_t n = n0; n <= n1; n++) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)(n - n0 + 0.5) / (double)(n1 - n0 + 1));
            mean += w * std::complex<double>(ri[n & kMask], rq[n & kMask]);
            wsum += w;
        }
        mean /= wsum;
        std::complex<double> sum = 0;
        for (uint64_t n = n0; n <= n1; n++) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)(n - n0 + 0.5) / (double)(n1 - n0 + 1));
            const std::complex<double> y = std::complex<double>(ri[n & kMask], rq[n & kMask]) - mean;
            const double ph = L.ph + 2 * kPi * L.fsc * ((double)n - L.tRef) / fv;
            sum += w * y * std::polar(1.0, -ph);
        }
        return sum / wsum;
    };
    // p = (B - r) / G, so the complex amplitude flips sign. The burst sits on the carrier at blanking level, a constant in the complex
    // baseband: project() takes that away first.
    b = -project(na, nb) / g;
    const double n1 = (double)(nb - na + 1), n2 = (double)(nb2i - na2 + 1);
    nb2 = n2 > 6 ? std::norm(project(na2, nb2i) / g) * (n2 / n1) : 0.0;     // a Hann window of n samples has noise power in proportion to 1 / n
    return true;
}

// The mean level of the burst window against blanking, in normalised video units (positive = brighter than blanking). A burst is symmetric about
// blanking; a compressor that squeezes what is below blanking (the sync pulses, and with them the lower half of every burst cycle) lifts it.
// Hann weights keep the 4.4 MHz of the burst itself out of the mean.
bool AtvVideo::Impl::burstShift(double tL, double& dv) const {
    const double bsUs = fmt.burstStartUs > 0 ? fmt.burstStartUs : 5.6;
    const uint64_t na = (uint64_t)std::ceil(tL + (bsUs + 0.45) * spu), nb = (uint64_t)std::floor(tL + (bsUs + 1.95) * spu);
    const double g = lineGain();
    if (nb <= na + 6 || g <= 0) return false;
    double s = 0, ws = 0;
    for (uint64_t n = na; n <= nb; n++) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)(n - na + 0.5) / (double)(nb - na + 1));
        s += w * rvAt(n); ws += w;
    }
    dv = (Bline - s / ws) / g;
    return true;
}

// One line of a loop. Returns false when there was no burst to measure.
bool AtvVideo::Impl::stepLock(Lock& L, double tL, bool pal, double nominal, double alpha, double beta) {
    advance(L, tL);
    std::complex<double> b;
    double nb2 = 0;
    if (!measureBurst(tL, L, b, nb2)) return false;
    const double amp = std::abs(b);
    L.nz += (nb2 - L.nz) * 0.05;
    const double ampC = std::sqrt(std::max(0.0, amp * amp - L.nz));      // the size without the noise that adds to it
    L.amp += (ampC / nominal - L.amp) * (L.lines < 8 ? 0.4 : 0.1);
    L.last = b;
    if (amp < 0.3 * nominal) return true;               // no burst on this line (or too weak to steer by)
    if (pal) { const int pr = (int)(curNo & 1); L.vacc[pr] += (b.real() - L.vacc[pr]) * 0.2; }
    double err = 0;
    bool have = false;
    if (pal) {
        // the bursts of two lines in a row are at 45 and 135 degrees: their sum points at 90
        if (L.prevValid && L.prevLine + 1 == curNo) {
            const std::complex<double> sm = b + L.prev;
            if (std::abs(sm) > 0.2 * (amp + std::abs(L.prev))) { err = std::arg(sm) - kPi / 2; have = true; }
        }
        L.prev = b; L.prevValid = true; L.prevLine = curNo;
    } else {
        err = std::arg(b) - kPi / 2; have = true;
    }
    if (have) {
        while (err > kPi) err -= 2 * kPi;
        while (err < -kPi) err += 2 * kPi;
        L.ph += alpha * err;
        L.fsc += beta * err * (fmt.lineHz / (2 * kPi));
        L.fsc = std::min(std::max(L.fsc, L.fsc0 - 3000), L.fsc0 + 3000);
        L.err2 += (err * err - L.err2) * (L.lines < 8 ? 0.4 : 0.06);
        L.lines++;
    }
    return true;
}

void AtvVideo::Impl::burstAndColour(double tL, bool picture) {
    (void)picture;
    const AtvFormat& F = fmt;
    if (colourKind < 0) {                      // still deciding what the colour is: every candidate gets a loop of its own
        if (cands.empty()) {
            // 625 lines: PAL at 4.43 and 3.58 MHz (PAL-N), and NTSC at the same two (NTSC 4.43, NTSC-N; not with positive modulation);
            // 525 lines: NTSC and PAL-M
            candKind.clear();
            if (fmt.lines == 625) {
                const int nc = pol < 0 ? 2 : 4;
                cands.resize((size_t)nc);
                cands[0].reset(4433618.75); cands[1].reset(3582056.25); candKind = {kAtvPal, kAtvPal};
                if (nc == 4) { cands[2].reset(4433618.75); cands[3].reset(3582056.25); candKind.push_back(kAtvNtsc); candKind.push_back(kAtvNtsc); }
            } else { cands.resize(2); cands[0].reset(455.0 / 2 * (4.5e6 / 286)); cands[1].reset(909.0 / 4 * (4.5e6 / 286)); candKind = {kAtvNtsc, kAtvPal}; }
            candLines = 0;
        }
        for (size_t k = 0; k < cands.size(); k++) {
            const bool pal = candKind[k] == kAtvPal;
            const double nom = fmt.lines == 625 ? 3.0 / 14 : 0.2;
            const bool fast = cands[k].lines < 40;
            // a candidate that is neither a good tone nor locking is not worth the time: the loops are cheap, so all run
            stepLock(cands[k], tL, pal, nom, fast ? 0.5 : 0.1, fast ? 0.04 : 0.004);
        }
        if (++candLines >= 160) decideColour();
        return;
    }
    if (colourKind == kAtvSecam) return;                  // secamLine() does it
    if (colourKind == kAtvMono || fmt.fscHz <= 0) { colourLocked = false; return; }
    if (!mainLock.init) { mainLock.reset(F.fscHz); }
    const bool fast = mainLock.lines < 40;
    if (!stepLock(mainLock, tL, F.pal, F.burstAmp, fast ? 0.5 : 0.1, fast ? 0.04 : 0.004)) return;
    burstEma = mainLock.amp;
    if (colourLocked) {
        // The picture gain follows the burst when the sync is compressed. The burst's fundamental is too small there, because the lower half of
        // each cycle is squeezed like the sync is: with the level shift of the burst the amplitude of the half that is not squeezed is
        // recovered (a cycle with the lower half scaled by k has fundamental (1 + k) / 2 and shift (1 - k) / pi, in units of its peak).
        double dv = 0;
        if (burstShift(tL, dv)) shiftEma += (dv - shiftEma) * 0.02;
        compEma += (burstEma - compEma) * 0.02;
        const double est = compEma + 0.5 * kPi * std::max(0.0, shiftEma) / F.burstAmp;
        compK = est > 1.12 && syncSqueeze() > 0.12 ? est : 1.0;       // a burst that is off on its own (a ghost, a beat) is not a squeezed sync
    }
    // The burst is judged by its phase scatter as well as its size: noise alone gives a burst of about the right size with a random phase.
    // The scatter of one line's burst against the loop is about 0.3 rad at a carrier-to-noise ratio of 20 dB.
    if (burstEma < 0.2 || mainLock.err2 > 1.0) { if (++killCount > 12) killer = true; }
    else if (burstEma > 0.3 && mainLock.err2 < 0.5) { killCount = 0; killer = false; }
    if (F.pal) vsign = mainLock.vacc[curNo & 1] >= 0 ? 1 : -1;            // V as is on the lines whose burst is at 45 degrees; the sign alternates line by line
    chromaErr = std::sqrt(mainLock.err2);
    colourLocked = mainLock.lines > 30 && !killer;
}

void AtvVideo::Impl::decideColour() {
    const int lines = fmt.lines;
    int best = -1;
    double bestErr = 1e9;
    for (size_t k = 0; k < cands.size(); k++) {
        const Lock& c = cands[k];
        if (c.amp < 0.5 || c.lines < 50 || c.err2 > 0.4) continue;      // 0.4 rad^2: about 36 degrees rms; a loop that follows noise has more than 3
        // A PAL loop also locks to an NTSC burst (the sum of two bursts at the same phase points anywhere it is turned to): PAL needs the
        // burst to swing to either side of the U axis on alternate lines, by about 0.7 of its size.
        if (candKind[k] == kAtvPal) {
            const double nom = lines == 525 ? 0.2 : 3.0 / 14;
            if (!(c.vacc[0] * c.vacc[1] < 0 && std::min(std::fabs(c.vacc[0]), std::fabs(c.vacc[1])) > 0.3 * nom * std::min(1.0, c.amp))) continue;
        }
        if (c.err2 < bestErr) { bestErr = c.err2; best = (int)k; }
    }
    int kind = kAtvMono;
    if (best >= 0) kind = candKind[(size_t)best];
    // SECAM: the subcarrier at the rest frequency of the line on the back porch, alternating line by line, or the identification lines of the
    // field blanking. A signal without either may still have them in a field or two: keep looking for a while before it counts as monochrome.
    bool secamFound = false;
    if (best < 0 && (lines == 625 || lines == 525) && secamReady) {
        const bool viaLines = sLead >= 30 && sPairs >= 20 && sAlt >= 0.85 * sPairs;
        const bool viaField = sBottle >= 6;
        secamFound = viaLines || viaField;
        if (!secamFound && candLines < 700) return;
        if (secamFound) { kind = kAtvSecam; secamHow = viaLines ? (viaField ? 3 : 1) : 2; }
    }
    // PAL-M is the second candidate of the 525-line systems; PAL-N and NTSC-N use the 3.58 MHz subcarrier on 625 lines
    bool palM = best == 1 && lines == 525;
    bool palN = best == 1 && lines == 625;
    const bool ntscN = best == 3 && lines == 625;
    const bool ntsc443 = best == 2 && lines == 625;
    char b[400];
    {
        std::string d;
        for (size_t k = 0; k < cands.size(); k++) { char t[100]; snprintf(t, sizeof t, " [%.0f Hz: burst %.0f%%, %d lines, phase error %.1f deg]", cands[k].fsc, cands[k].amp * 100, cands[k].lines, std::sqrt(cands[k].err2) * 180 / kPi); d += t; }
        doLog(("analog TV: colour search:" + d).c_str());
    }
    if (secamFound) snprintf(b, sizeof b, "analog TV: colour search: SECAM (%s; lead-in found on %d lines, %d of %d pairs alternate, %d identification line pairs)", secamHow == 3 ? "lead-in on the lines and identification lines" : secamHow == 1 ? "lead-in on the lines" : "identification lines of the field blanking", sLead, sAlt, sPairs, sBottle);
    else snprintf(b, sizeof b, "analog TV: colour search: %s", best < 0 ? "no colour subcarrier (monochrome)" : lines == 625 ? (palN ? "PAL-N burst" : ntscN ? "NTSC-N burst (3.58 MHz)" : ntsc443 ? "NTSC 4.43 burst" : "PAL burst") : (palM ? "PAL-M burst" : "NTSC burst"));
    doLog(b);
    colourKind = prm.forceColour >= 0 ? prm.forceColour : kind;
    // PAL on N means the Argentine system, whose sound is at +4.5 MHz: the subcarrier decides here
    if (palN || ntscN) soundSpacing = 4.5;
    makeFormat(lines);
    mainLock.reset(fmt.fscHz);
    if (best >= 0 && colourKind == candKind[(size_t)best]) { mainLock = cands[(size_t)best]; }
    cands.clear();
    burstEma = 1; killCount = 0; killer = false;
    if (colourKind == kAtvSecam) { colourLocked = true; secAmpEma = 1; secKill = 0; }
}

// One of the identification lines of the field blanking: the subcarrier ends the line at +350 kHz from f0R (a D'R line) or -350 kHz from f0B
void AtvVideo::Impl::secamBottle(double tL, double g, long L) {
    const long sa = secamSpan(tL + 22 * spu, tL + 64 * spu);
    const size_t M = secam.baseband(zsi.data(), zsq.data(), zsi.size(), secBB);
    const double a = secamOut(tL + 30 * spu, sa), b = secamOut(tL + 58 * spu, sa);
    if (M < 10 || a < 0 || b >= (double)M) return;
    double f, am;
    secam.tone(secBB, (size_t)std::ceil(a), (size_t)std::floor(b) + 1, f, am);
    if (am / g < 0.1) return;
    // the trapezoid has reached +350 kHz above f0R (4.756 MHz) or -350 kHz below f0B (3.9 MHz) by now
    const int kind = std::fabs(f - kSecamMaxR) < 100e3 ? 1 : std::fabs(f - kSecamMinB) < 100e3 ? 0 : -1;
    newFieldVote();
    if (kind < 0) { lastBottleKind = -1; return; }
    rVote += (kind == 1) == ((L & 1) == 1) ? 4.0 : -4.0;           // these lines are sure
    if (lastBottleKind >= 0 && lastBottleLine + 1 == L && kind != lastBottleKind) sBottle++;      // the lines alternate
    lastBottleKind = kind; lastBottleLine = L;
}

// Everything SECAM that one line needs: field identification lines, the lead-in during the colour search, and the colour of a picture line.
void AtvVideo::Impl::secamLine(double tL, bool havePicture) {
    const AtvFormat& F = fmt;
    const double g = lineGain();
    if (!secamReady || g <= 0) return;
    const long hr = std::lround(2.0 * (double)curNo - vsyncH);
    long H = ((long)F.broadStart[vsyncField] + hr) % F.halfLines;
    if (H < 0) H += F.halfLines;
    const long L = (H >> 1) + 1;                                // the line number of the standard
    if (F.lines == 625 && ((vsyncField == 0 && L >= 7 && L <= 15) || (vsyncField == 1 && L >= 320 && L <= 328))) { secamBottle(tL, g, L); return; }
    if (!havePicture) return;
    if (colourKind < 0) {                                       // colour search: does the lead-in alternate between the two rest frequencies?
        const long sa = secamSpan(tL + 1 * spu, tL + 14 * spu);
        secam.baseband(zsi.data(), zsq.data(), zsi.size(), secBB);
        double amp;
        const int kind = secamLead(tL, g, secBB, sa, amp);
        if (kind >= 0) {
            sLead++;
            if (lastLeadKind >= 0 && lastLeadNo + 1 == curNo) { sPairs++; if (kind != lastLeadKind) sAlt++; }
            lastLeadKind = kind; lastLeadNo = curNo;
        } else lastLeadKind = -1;
        return;
    }
    const long sa = secamSpan(tL + 1 * spu, tL + 69 * spu);
    const size_t M = secam.baseband(zsi.data(), zsq.data(), zsi.size(), secBB);
    if (M < 20) return;
    double leadAmp = 0, fLead = 0;
    secamLead(tL, g, secBB, sa, leadAmp, &fLead);
    // Which of the two the line is: the lead-in tone says so, softly (it is a measurement of a short stretch and noisy), and the lines of the
    // field vote: the kinds alternate, so every line says something about every other line.
    newFieldVote();
    const bool haveTone = leadAmp >= 0.04 && fLead > 4.15e6 && fLead < 4.52e6;
    if (haveTone) {
        const double e = std::min(1.5, std::max(-1.5, (fLead - 0.5 * (kSecamF0R + kSecamF0B)) / 78e3)) * std::min(1.0, leadAmp / 0.08);
        rVote = rVote * 0.97 + ((L & 1) == 1 ? e : -e);
    }
    bool isR;
    if (std::fabs(rVote) >= 0.6) isR = ((L & 1) == 1) == (rVote > 0);
    else if (haveTone) isR = fLead > 0.5 * (kSecamF0R + kSecamF0B);
    else return;                                                // not known yet: this line stays without colour
    const double pxUs = F.activeUs / W;
    const double aA = std::max(0.0, secamOut(tL + F.blankEndUs * spu, sa)), aB = std::max(0.0, secamOut(tL + (F.blankEndUs + F.activeUs) * spu, sa));
    secam.demod(secBB, isR, secD, (float)(0.3 * kSecamM0 * g), (size_t)aA, (size_t)aB);
    const float chAmp = secam.amp() / (float)(kSecamM0 * g);    // nominal 1: the bell restores M0
    secAmpEma += (chAmp - secAmpEma) * 0.1f;
    burstEma = secAmpEma;
    if (secAmpEma < 0.25f) { if (++secKill > 12) killer = true; }
    else if (secAmpEma > 0.4f) { secKill = 0; killer = false; }
    colourLocked = !killer;
    chromaErr = 0;
    // colour difference at the pixels (cubic through the values of D')
    secRY.resize((size_t)W); secBY.resize((size_t)W); secU.resize((size_t)W); secV.resize((size_t)W);
    std::vector<float>& dst = isR ? secRY : secBY;
    const long nD = (long)secD.size();
    for (int x = 0; x < W; x++) {
        const double u = secamOut(tL + (F.blankEndUs + (x + 0.5) * pxUs) * spu, sa) + 0.5;      // D'[m] lies half an output before output m
        const long i0 = (long)std::floor(u);
        const float fr = (float)(u - (double)i0);
        auto at = [&](long i) { return secD[(size_t)std::min(nD - 1, std::max(0L, i))]; };
        const float t2 = fr * fr, t3 = t2 * fr;
        const float d = (-0.5f * t3 + t2 - 0.5f * fr) * at(i0 - 1) + (1.5f * t3 - 2.5f * t2 + 1.f) * at(i0) + (-1.5f * t3 + 2.f * t2 + 0.5f * fr) * at(i0 + 1) + (0.5f * t3 - 0.5f * t2) * at(i0 + 2);
        dst[(size_t)x] = isR ? -d / 1.902f : d / 1.505f;     // D'R = -1.902 (R - Y), D'B = 1.505 (B - Y)
    }
    (isR ? secRYNo : secBYNo) = curNo;
    // the other colour difference is the one of the line before (the delay line of a SECAM decoder)
    const bool haveRY = secRYNo == curNo || secRYNo + 1 == curNo, haveBY = secBYNo == curNo || secBYNo + 1 == curNo;
    for (int x = 0; x < W; x++) {
        secV[(size_t)x] = haveRY ? 0.877f * secRY[(size_t)x] : 0.f;
        secU[(size_t)x] = haveBY ? 0.493f * secBY[(size_t)x] : 0.f;
    }
    secamLineOk = true;
}

// chroma filters for the standard that was found: a band-pass around the subcarrier (what is taken out of the video to leave the
// luminance) and the low-pass after the synchronous demodulator
void AtvVideo::Impl::designChroma() {
    bpf.clear();
    if (fmt.fscHz <= 0 && !fmt.secam) return;
    const double fsc = fmt.secam ? kSecamBellHz : fmt.fscHz, nyq = 0.5 * fv, wing = fmt.secam ? 0.65e6 : 1.25e6;     // SECAM: the FM subcarrier fills 3.9 to 4.76 MHz
    auto resp = [&](double f) -> std::complex<double> {
        const double af = std::fabs(f);
        const double lo = fsc - wing, hi = fsc + wing, tr = fmt.secam ? 0.5e6 : 0.9e6;
        double r = 0;
        if (af >= lo && af <= std::min(hi, nyq)) r = 1;
        else if (af < lo && af > lo - tr) r = 0.5 + 0.5 * std::cos(kPi * (lo - af) / tr);
        else if (af > hi && af < hi + tr) r = 0.5 + 0.5 * std::cos(kPi * (af - hi) / tr);
        return r;
    };
    const auto t = fromResponse(resp, fv, 25, 5.0);
    bpf.resize(t.size());
    for (size_t k = 0; k < t.size(); k++) bpf[k] = (float)t[k].real();
    if (fmt.secam) return;
    zdec = std::max(1, (int)std::floor(fv / 5e6));
    lpC = lowpass(0.9e6, std::min(2.6e6, 0.9 * fv / 2 - 0.1e6), fv, 38, 31);
}

} // namespace dect2
