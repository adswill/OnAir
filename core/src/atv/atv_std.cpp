// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV systems: see atv_std.h. Source of the numbers: ITU-R BT.470-6 (1998), Tables 1, 1-1, 1-2, 2, 3 and Figs 1, 2, 4, 5.
#include "dect2/atv/atv_std.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace dect2 {

bool atvMakeFormat(int sys, int colour, AtvFormat& f) {
    if (sys < 0 || sys >= kAtvSysCount || colour < kAtvMono || colour > kAtvSecam) {
        DIAG_DEBUG(820977, "Analog TV system number %d with colour code %d is not a known combination, so no video format is built.", sys, colour);
        return false;
    }
    f = AtvFormat();
    f.sys = sys; f.colour = colour;
    const bool ntscLines = sys == kAtvM;
    const bool l405 = sys == kAtvA, l819 = sys == kAtvE;
    const bool positive = atvSysPositive(sys);
    if ((l405 || l819) && colour != kAtvMono) { DIAG_DEBUG(778107, "The 405-line and 819-line analog TV systems were monochrome only, so colour code %d is refused.", colour); return false; }    // the 405 and 819-line services were monochrome
    if (colour == kAtvNtsc && positive) { DIAG_DEBUG(532053, "NTSC colour was never sent with positive modulation, so analog TV system %d with NTSC is refused.", sys); return false; }          // NTSC was never sent with positive modulation
    switch (sys) {
    case kAtvB:  f.sysName = "B/G"; f.chanMhz = 7; f.soundSpacingMhz = 5.5; f.videoBwMhz = 5.0; f.vestigialMhz = 0.75; break;
    case kAtvG:  f.sysName = "B/G"; f.chanMhz = 8; f.soundSpacingMhz = 5.5; f.videoBwMhz = 5.0; f.vestigialMhz = 0.75; break;
    case kAtvI:  f.sysName = "I";   f.chanMhz = 8; f.soundSpacingMhz = 5.9996; f.videoBwMhz = 5.5; f.vestigialMhz = 1.25; f.whitePct = 20; f.soundRelDb = -10; break;
    case kAtvDK: f.sysName = "D/K"; f.chanMhz = 8; f.soundSpacingMhz = 6.5; f.videoBwMhz = 6.0; f.vestigialMhz = 0.75; break;
    case kAtvM:  f.sysName = "M";   f.chanMhz = 6; f.soundSpacingMhz = 4.5; f.videoBwMhz = 4.2; f.vestigialMhz = 0.75; f.soundDevKhz = 25; f.preEmphUs = 75; f.soundRelDb = -10; break;
    case kAtvN:  f.sysName = "N";   f.chanMhz = 6; f.soundSpacingMhz = 4.5; f.videoBwMhz = 4.2; f.vestigialMhz = 0.75; f.soundDevKhz = 25; f.preEmphUs = 75; f.soundRelDb = -10; break;
    // BT.470-6 Table 1 (columns A, C, E, H, K', L) and Table 3 (items 1 to 5)
    case kAtvA:  f.sysName = "A";   f.chanMhz = 5; f.soundSpacingMhz = -3.5; f.videoBwMhz = 3.0; f.vestigialMhz = 0.75; f.visionOffMhz = 3.75; f.mirrored = true; break;
    case kAtvC:  f.sysName = "C";   f.chanMhz = 7; f.soundSpacingMhz = 5.5; f.videoBwMhz = 5.0; f.vestigialMhz = 0.75; break;
    case kAtvE:  f.sysName = "E";   f.chanMhz = 14; f.soundSpacingMhz = 11.15; f.videoBwMhz = 10.0; f.vestigialMhz = 2.0; f.visionOffMhz = 2.25; break;
    case kAtvH:  f.sysName = "H";   f.chanMhz = 8; f.soundSpacingMhz = 5.5; f.videoBwMhz = 5.0; f.vestigialMhz = 1.25; break;
    case kAtvKp: f.sysName = "K'";  f.chanMhz = 8; f.soundSpacingMhz = 6.5; f.videoBwMhz = 6.0; f.vestigialMhz = 1.25; break;
    case kAtvL:  f.sysName = "L";   f.chanMhz = 8; f.soundSpacingMhz = 6.5; f.videoBwMhz = 6.0; f.vestigialMhz = 1.25; break;
    case kAtvLp: f.sysName = "L'";  f.chanMhz = 8; f.soundSpacingMhz = -6.5; f.videoBwMhz = 6.0; f.vestigialMhz = 1.25; f.visionOffMhz = 6.75; f.mirrored = true; break;
    }
    if (positive) {
        // positive modulation, AM sound without pre-emphasis; the sound carrier 10 dB below the peak vision carrier (Table 3 item 4: 10/1 in L)
        f.positive = true; f.whitePct = 100; f.syncPct = 0; f.blankPct = l819 ? 25 : 30;
        f.amSound = true; f.preEmphUs = 0; f.soundDevKhz = 0; f.soundRelDb = -10;
    }
    if (ntscLines) {
        f.lines = 525; f.lineHz = 4.5e6 / 286; f.fieldHz = 2 * f.lineHz / 525;
        f.lineUs = 1e6 / f.lineHz; f.halfLines = 1050;
        f.frontPorchUs = 1.5; f.blankEndUs = 9.4;                   // blanking 10.9 +- 0.2 us (Table 1-1 symbol a), back edge 9.2 to 10.3
        f.activeUs = f.lineUs - (f.frontPorchUs + f.blankEndUs);    // 52.6556
        f.syncUs = 4.7; f.syncRiseUs = 0.14; f.blankRiseUs = 0.14;  // RS-170A typical; Table 1-1 only gives the limits (0.25)
        f.eqUs = 2.3; f.broadUs = 27.1; f.eqPulses = 6; f.broadPulses = 6;
        f.syncLevel = -0.4; f.setup = 0.075;
        f.broadStart[0] = 6; f.broadStart[1] = 531;                 // first broad pulse: 3 H after the first equalising pulse
        f.picW = 640; f.picH = 480; f.fieldRows = 240;
        f.firstLine[0] = 22; f.firstLine[1] = 285;
    } else if (l405) {
        // 405 lines (Table 1 column A): line 98.77 us, blanking 18 us with a front porch of 1.75 us, line sync 9 us; the field sync is a block of
        // 8 broad pulses of 40 us half a line apart with no equalising pulses; sync : picture = 30 : 70 (blanking at 30 % of the carrier)
        f.lines = 405; f.lineHz = 10125; f.fieldHz = 50; f.lineUs = 1e6 / f.lineHz; f.halfLines = 810;
        f.frontPorchUs = 1.75; f.blankEndUs = 16.25; f.activeUs = f.lineUs - 18.0;
        f.syncUs = 9.0; f.syncRiseUs = 0.25; f.blankRiseUs = 0.3;
        f.eqUs = 0; f.broadUs = 40.0; f.eqPulses = 0; f.broadPulses = 8;
        f.syncLevel = -3.0 / 7; f.setup = 0;
        f.broadStart[0] = 0; f.broadStart[1] = 405;
        f.picW = 504; f.picH = 376; f.fieldRows = 188;              // 4:3, 376 of the 405 lines
        f.firstLine[0] = 15; f.firstLine[1] = 218;
    } else if (l819) {
        // 819 lines (Table 1 column E): line 48.84 us, blanking 9.5 us with a front porch of 0.6 us, line sync 2.5 us; sync : picture = 25 : 75.
        // The field sync is modelled like the 405-line one (8 broad pulses half a line apart, no equalising pulses): BT.470-6 gives only its length.
        f.lines = 819; f.lineHz = 20475; f.fieldHz = 50; f.lineUs = 1e6 / f.lineHz; f.halfLines = 1638;
        f.frontPorchUs = 0.6; f.blankEndUs = 8.9; f.activeUs = f.lineUs - 9.5;
        f.syncUs = 2.5; f.syncRiseUs = 0.1; f.blankRiseUs = 0.12;
        f.eqUs = 0; f.broadUs = 20.0; f.eqPulses = 0; f.broadPulses = 8;
        f.syncLevel = -1.0 / 3; f.setup = 0;
        f.broadStart[0] = 0; f.broadStart[1] = 819;
        f.picW = 1024; f.picH = 768; f.fieldRows = 384;             // 4:3, 768 of the 819 lines
        f.firstLine[0] = 26; f.firstLine[1] = 436;
    } else {
        f.lines = 625; f.lineHz = 15625; f.fieldHz = 50; f.lineUs = 64; f.halfLines = 1250;
        f.frontPorchUs = 1.5; f.blankEndUs = 10.5;                  // blanking 12.0 +- 0.3 us, back edge 10.5
        f.activeUs = 52.0;
        f.syncUs = 4.7; f.syncRiseUs = 0.2; f.blankRiseUs = 0.3;
        f.eqUs = 2.35; f.broadUs = 27.3; f.eqPulses = 5; f.broadPulses = 5;
        f.syncLevel = -3.0 / 7; f.setup = 0;
        f.broadStart[0] = 0; f.broadStart[1] = 625;
        f.picW = 768; f.picH = 576; f.fieldRows = 288;
        f.firstLine[0] = 23; f.firstLine[1] = 336;
    }
    f.pal = colour == kAtvPal; f.ntsc = colour == kAtvNtsc; f.secam = colour == kAtvSecam;
    f.fscHz = 0; f.vSwitch = false; f.burstAmp = 0; f.burstCycles = 0;
    if (colour == kAtvPal) {
        f.vSwitch = true;
        if (sys == kAtvM) {            // PAL-M: fsc = 909/4 fH (Table 2 item 2.11), burst 9 cycles, 4/10 of white
            f.fscHz = 909.0 / 4 * f.lineHz; f.burstStartUs = 5.3; f.burstCycles = 9; f.burstAmp = 0.2; f.colourName = "PAL-M";
        } else if (sys == kAtvN) {     // Argentine PAL-N: (917/4 + 1/625) fH
            f.fscHz = (917.0 / 4 + 1.0 / 625) * f.lineHz; f.burstStartUs = 5.6; f.burstCycles = 9; f.burstAmp = 3.0 / 14; f.colourName = "PAL-N";
        } else {                       // (1135/4 + 1/625) fH = 4 433 618.75 Hz
            f.fscHz = (1135.0 / 4 + 1.0 / 625) * f.lineHz; f.burstStartUs = 5.6; f.burstCycles = 10; f.burstAmp = 3.0 / 14; f.colourName = "PAL";
        }
        f.chromaDelayNs = 170;
    } else if (colour == kAtvNtsc) {
        if (ntscLines) {               // 455/2 fH = 3 579 545.45 Hz, burst at 180 degrees to the B-Y axis, 9 cycles, 40 IRE p-p
            f.fscHz = 455.0 / 2 * f.lineHz; f.burstStartUs = 5.3; f.burstCycles = 9; f.burstAmp = 0.2; f.colourName = "NTSC";
        } else if (sys == kAtvN) {     // NTSC-N (Paraguay before PAL-N): NTSC on 625 lines with the 3.58 MHz subcarrier of system N
            f.fscHz = (917.0 / 4 + 1.0 / 625) * f.lineHz; f.burstStartUs = 5.6; f.burstCycles = 9; f.burstAmp = 3.0 / 14; f.colourName = "NTSC-N";
        } else {                       // NTSC 4.43 on 625 lines: the PAL subcarrier, an NTSC burst and chrominance (no V switch)
            f.fscHz = (1135.0 / 4 + 1.0 / 625) * f.lineHz; f.burstStartUs = 5.6; f.burstCycles = 10; f.burstAmp = 3.0 / 14; f.colourName = "NTSC 4.43";
        }
        f.chromaDelayNs = 170;
    } else if (colour == kAtvSecam) {
        f.colourName = "SECAM"; f.burstStartUs = 5.6;
        // SECAM on M and N: the subcarriers (3.9 to 4.756 MHz) need more than the 4.2 MHz of video of these systems and overlap their
        // 4.5 MHz sound carrier, so such a signal is a 525 (or 625) line SECAM picture sent without that sound carrier: the video band is 5 MHz
        if (sys == kAtvM || sys == kAtvN) f.videoBwMhz = 5.0;
    } else {
        f.colourName = "mono";
    }
    const std::string rate = ntscLines ? "59.94" : "50";
    const std::string raster = std::to_string(f.lines) + "/" + rate;
    f.name = colour == kAtvMono ? f.sysName + " " + raster + " mono"
           : (f.colourName.compare(0, 4, "PAL-") == 0 || f.colourName.compare(0, 5, "NTSC-") == 0) ? f.colourName + " " + raster   // PAL-M, PAL-N, NTSC-N name the system
                                                     : f.colourName + " " + f.sysName + " " + raster;
    return true;
}

bool atvSysPositive(int sys) { return sys == kAtvA || sys == kAtvC || sys == kAtvE || sys == kAtvL || sys == kAtvLp; }

int atvSysLines(int sys) { return sys == kAtvM ? 525 : sys == kAtvA ? 405 : sys == kAtvE ? 819 : 625; }

int atvDefaultColour(int sys) {
    return sys == kAtvM ? kAtvNtsc : (sys == kAtvL || sys == kAtvLp || sys == kAtvKp) ? kAtvSecam : (sys == kAtvA || sys == kAtvC || sys == kAtvE) ? kAtvMono : kAtvPal;
}

int AtvFormat::pulseAt(int h) const {
    h = ((h % halfLines) + halfLines) % halfLines;
    for (int fl = 0; fl < 2; fl++) {
        const int start = ((broadStart[fl] - eqPulses) % halfLines + halfLines) % halfLines;
        const int d = ((h - start) % halfLines + halfLines) % halfLines;
        if (d < 2 * eqPulses + broadPulses) return d >= eqPulses && d < eqPulses + broadPulses ? 3 : 2;
    }
    return (h & 1) ? 0 : 1;
}

bool AtvFormat::burstOnLine(int line) const {
    if (colour == kAtvMono) return false;
    if (secam) {                                      // SECAM has no burst; the lead-in reference is on the picture lines and the identification lines have chroma
        const int a0 = firstLine[0], a1 = firstLine[0] + fieldRows, b0 = firstLine[1], b1 = firstLine[1] + fieldRows;
        return (line >= a0 && line < a1) || (line >= b0 && line < b1);
    }
    if (lines == 625) {
        const int s1 = sys == kAtvI ? 623 : 622, e1 = sys == kAtvI ? 6 : 5, s2 = sys == kAtvI ? 311 : 310, e2 = sys == kAtvI ? 319 : 318;
        if (line >= s1 || line <= e1) return false;
        if (line >= s2 && line <= e2) return false;
        return true;
    }
    if (line >= 522 || line <= 7) return false;       // 11 lines: 522 to 7 and 260 to 270 (Table 2 item 2.17)
    if (line >= 260 && line <= 270) return false;
    return true;
}

void atvRgbToYuv(float r, float g, float b, float& y, float& u, float& v) {
    y = 0.299f * r + 0.587f * g + 0.114f * b;
    u = 0.493f * (b - y);
    v = 0.877f * (r - y);
}

void atvYuvToRgb(float y, float u, float v, float& r, float& g, float& b) {
    r = y + v / 0.877f;
    b = y + u / 0.493f;
    g = (y - 0.299f * r - 0.114f * b) / 0.587f;
}

void atvEbuBar(int i, float rgb[3]) {
    static const float kBars[8][3] = {{1, 1, 1}, {0.75f, 0.75f, 0}, {0, 0.75f, 0.75f}, {0, 0.75f, 0}, {0.75f, 0, 0.75f}, {0.75f, 0, 0}, {0, 0, 0.75f}, {0, 0, 0}};
    i = i < 0 ? 0 : i > 7 ? 7 : i;
    for (int k = 0; k < 3; k++) rgb[k] = kBars[i][k];
}

std::complex<double> atvSecamLfPreEmph(double fHz) {
    const double x = fHz / kSecamLf1Hz;
    return std::complex<double>(1, x) / std::complex<double>(1, x / 3);
}

std::complex<double> atvSecamHfPreEmph(double fHz) {
    const double f0 = kSecamBellHz, f = std::max(fHz, 1.0);
    const double F = f / f0 - f0 / f;
    return std::complex<double>(1, 16 * F) / std::complex<double>(1, 1.26 * F);
}

double atvSpacingMhz(int i) {
    static const double s[kAtvSpacingCount] = {5.5, 6.0, 6.5, 4.5, 3.5, 11.15};
    return s[i < 0 ? 0 : i >= kAtvSpacingCount ? kAtvSpacingCount - 1 : i];
}

} // namespace dect2
