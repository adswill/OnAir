// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV: the systems (B/G, I, D/K, M, N, and the positive-modulation systems A, C, E, L, L' with AM sound, plus H and K'), their timing and
// levels, and the colour encodings (PAL, NTSC, SECAM). Shared by the receiver, the test-signal generator and the tests. Every number comes from
// ITU-R BT.470-6 (Tables 1 to 3, Figs 1 and 2) and ITU-R BT.1700 unless a comment says otherwise. Systems A, C and E were withdrawn from later
// editions of BT.470; their numbers are those of BT.470-6 Table 1 and of the CCIR Report 624 tables it was built from. Times are in microseconds measured from the datum 0H (the 50 % point of the leading edge of the line
// synchronising pulse). Levels are normalised: blanking = 0, peak white = 1 (0.7 V in the 625-line systems, 100 IRE in 525).
#pragma once
#include <complex>
#include <string>

namespace dect2 {

// The order is the order of the lists in the interface and the test signal's options: new systems go at the end.
//   A   405 lines / 50 Hz, 5 MHz channel, 3 MHz video, sound 3.5 MHz BELOW the vision carrier, positive, AM sound (UK, until 1985)
//   C   625 / 50, 7 MHz, 5 MHz video, sound +5.5 MHz, positive, AM sound (Belgium, Luxembourg)
//   E   819 / 50, 14 MHz, 10 MHz video, sound +-11.15 MHz, vestigial sideband 2 MHz, positive, AM sound (France, until 1984)
//   H   625 / 50, 8 MHz, 5 MHz video, sound +5.5 MHz, vestigial sideband 1.25 MHz, negative, FM sound (Belgium UHF)
//   K'  625 / 50, 8 MHz, 6 MHz video, sound +6.5 MHz, vestigial sideband 1.25 MHz, negative, FM sound (French overseas territories)
//   L   625 / 50, 8 MHz, 6 MHz video, sound +6.5 MHz, vestigial sideband 1.25 MHz, positive, AM sound (France)
//   L'  the same in band I: the channel is turned round, the sound 6.5 MHz below the vision carrier
enum AtvSys { kAtvB = 0, kAtvG, kAtvI, kAtvDK, kAtvM, kAtvN, kAtvA, kAtvC, kAtvE, kAtvH, kAtvKp, kAtvL, kAtvLp, kAtvSysCount };
enum AtvColourKind { kAtvMono = 0, kAtvPal, kAtvNtsc, kAtvSecam };

struct AtvFormat {
    int sys = kAtvG;
    int colour = kAtvPal;
    bool pal = false, ntsc = false, secam = false;
    std::string sysName;          // "B/G", "I", "D/K", "M", "N", "A", "C", "E", "H", "K'", "L", "L'"
    std::string colourName;       // "PAL", "NTSC", "SECAM", "mono", "PAL-M", "PAL-N", "NTSC-N", "NTSC 4.43"
    std::string name;             // "PAL B/G 625/50"

    // raster
    int lines = 625;              // lines per frame: 625, 525, 405 (A) or 819 (E)
    double fieldHz = 50;          // 50 or 60/1.001
    double lineHz = 15625;        // 15625, 4.5 MHz / 286, 10125 (A) or 20475 (E)
    double lineUs = 64;           // 1e6 / lineHz
    int halfLines = 1250;         // half-lines per frame (the vertical interval is built from them)

    // radio frequency layout
    double chanMhz = 8;           // nominal channel width (B 7, G/I/D/K 8, M/N 6)
    double visionOffMhz = 1.25;   // vision carrier above the lower edge of the channel
    double soundSpacingMhz = 5.5; // sound carrier above the vision carrier (negative: below it, systems A and L')
    double videoBwMhz = 5.0;      // nominal video bandwidth
    double vestigialMhz = 0.75;   // width of the vestigial sideband (the transmitted spectrum is flat down to this offset below the carrier)
    double whitePct = 12.5;       // carrier amplitude at peak white, in percent of the peak carrier (sync tip = 100 %; positive modulation: 100)
    // Positive modulation (A, C, E, L, L'): more video = more carrier. The sync tip is the lowest carrier level, peak white the highest (100 %),
    // blanking sits at blankPct (BT.470-6 Table 1 item 2.11: 30 % in A, C and L, 25 % in E; the sync tip is at 0, L allows up to 5 %).
    bool positive = false;
    double syncPct = 0, blankPct = 30;
    bool amSound = false;         // the sound carrier is amplitude modulated (A, C, E, L, L'); FM otherwise
    double amDepth = 0.8;         // AM sound: modulation depth at full level
    bool mirrored = false;        // the channel is turned round: the main sideband and the sound are below the vision carrier (A, L')
    double soundDevKhz = 50;      // peak deviation of the FM sound
    double preEmphUs = 50;        // sound pre-emphasis
    double soundRelDb = -13;      // sound carrier power relative to the peak vision carrier (typical: 20/1 to 10/1)

    // line structure (times in us, from 0H)
    double syncUs = 4.7;          // line synchronising pulse
    double frontPorchUs = 1.5;    // blanking starts this long before 0H
    double blankEndUs = 10.5;     // blanking ends (the active picture starts) this long after 0H
    double activeUs = 52.0;       // active picture duration
    double syncRiseUs = 0.2;      // 10 to 90 % build-up time of sync edges
    double blankRiseUs = 0.3;     // 10 to 90 % build-up time of blanking edges
    double eqUs = 2.35;           // equalising pulse
    double broadUs = 27.3;        // field-synchronising (broad) pulse
    int eqPulses = 5;             // per sequence: 5 in 625 lines, 6 in 525, none in 405 and 819 lines
    int broadPulses = 5;          // field-synchronising pulses: 5 in 625 lines, 6 in 525, 8 in 405 and 819

    // levels (normalised)
    double syncLevel = -3.0 / 7;  // sync tip: -0.3 V / 0.7 V, or -40 IRE
    double setup = 0;             // black level above blanking (0 in 625 lines, 7.5 IRE in M)

    // colour
    double fscHz = 0;             // subcarrier (SECAM: 0, see kSecamF0R and kSecamF0B)
    double burstStartUs = 5.6;    // burst starts this long after 0H
    int burstCycles = 10;
    double burstAmp = 3.0 / 14;   // burst peak amplitude (PAL: 300 mV p-p = 3/14 of white, NTSC 40 IRE p-p = 0.2)
    bool vSwitch = false;         // PAL: the V component and the burst phase alternate line by line
    double chromaDelayNs = 0;     // transmitters pre-correct the chrominance by this much (BT.470 Table 3 item 14: 170 ns at the subcarrier)

    // picture: what the decoder produces and the generator draws
    int picW = 768, picH = 576;
    int firstLine[2] = {23, 336}; // first visible line of field 1 and 2 (continuous line numbers, 1 = the line that starts at the first
                                  // field-synchronising pulse of field 1 in 625-line systems, or the first equalising pulse in 525)
    int fieldRows = 288;          // visible lines per field

    // The pulse that starts half-line h of the frame (h = 0 .. halfLines - 1): 0 none, 1 line sync, 2 equalising, 3 broad.
    // Line L (1 based) starts at half-line 2 (L - 1); all line syncs sit on even half-lines, the vertical blocks of the two fields are half a line apart.
    int pulseAt(int h) const;
    // Half-line of the first field-synchronising (broad) pulse of field 1 / field 2
    int broadStart[2] = {0, 625};
    // Does line L (1 based, continuous numbering) carry a burst? (burst blanking around the field sync, BT.470 Table 2 item 2.17)
    // SECAM: the same question for the lead-in reference: the picture lines have it, the field blanking does not.
    bool burstOnLine(int line) const;
    // SECAM, 625 lines: one of the 9 lines of field identification signals (7 to 15 and 320 to 328)?
    bool secamBottleLine(int line) const { return secam && lines == 625 && ((line >= 7 && line <= 15) || (line >= 320 && line <= 328)); }
    // Half-line at which the first visible line of field f (0, 1) starts, and the number of half-lines between lines (2)
    int visibleStartH(int field) const { return 2 * (firstLine[field] - 1); }
};

// Fills `f` for a system and a colour encoding. Returns false when the combination does not exist (colour on A or E, NTSC on a positive system).
// colour: kAtvMono, kAtvPal, kAtvNtsc, kAtvSecam; PAL on M is PAL-M, PAL on N is the Argentine PAL-N (3.58205625 MHz), NTSC on N is NTSC-N
// (the same 3.58205625 MHz subcarrier on 625 lines), NTSC on the other 625-line systems is "NTSC 4.43" (4.43361875 MHz, as VCRs and some
// closed systems made it), SECAM on M and N keeps the SECAM subcarriers of 625 lines (they overlap the 4.5 MHz sound carrier of M and N:
// such a signal has no sound carrier, and its video band is 5 MHz).
bool atvMakeFormat(int sys, int colour, AtvFormat& f);
bool atvSysPositive(int sys);                 // A, C, E, L, L'
int atvSysLines(int sys);                     // 625, 525 (M), 405 (A), 819 (E)
int atvDefaultColour(int sys);                // what the system was broadcast with: NTSC (M), SECAM (L, L', K'), mono (A, C, E), PAL otherwise

// Baseband position of the carriers for a channel centred at 0: vision = -(chanMhz/2 - visionOffMhz), sound = vision + spacing
inline double atvVisionOffsetHz(const AtvFormat& f) { return -(f.chanMhz / 2 - f.visionOffMhz) * 1e6; }
inline double atvSoundOffsetHz(const AtvFormat& f) { return atvVisionOffsetHz(f) + f.soundSpacingMhz * 1e6; }

// ---- colour: BT.470-6 Table 2 items 2.4 and 2.5
// E'Y = 0.299 R + 0.587 G + 0.114 B,  E'U = 0.493 (B - Y),  E'V = 0.877 (R - Y)   (PAL, and the U/V form of NTSC: the I/Q axes are these rotated by 33 degrees)
void atvRgbToYuv(float r, float g, float b, float& y, float& u, float& v);
void atvYuvToRgb(float y, float u, float v, float& r, float& g, float& b);
// SECAM: D'R = -1.902 (R - Y), D'B = 1.505 (B - Y)  (Table 2 item 2.7)
inline float atvSecamDr(float r, float y) { return -1.902f * (r - y); }
inline float atvSecamDb(float b, float y) { return 1.505f * (b - y); }
// SECAM (BT.470-6 Table 2; the numbers below are read from the text of the Recommendation unless a comment says otherwise)
// rest frequencies of the subcarriers, 282 fH and 272 fH (items 2.10, 2.11); nominal deviation per unit of D' (item 2.12): f = f0 + dev * D'
constexpr double kSecamF0R = 4406250.0, kSecamF0B = 4250000.0;
constexpr double kSecamDevR = 280e3, kSecamDevB = 230e3;
// The frequency stays between the limits of the maximum deviations: -506 / +350 kHz from f0R on D'R lines, -350 / +506 kHz from f0B on D'B lines
constexpr double kSecamMinR = kSecamF0R - 506e3, kSecamMaxR = kSecamF0R + 350e3;        // 3.90025 .. 4.75625 MHz
constexpr double kSecamMinB = kSecamF0B - 350e3, kSecamMaxB = kSecamF0B + 506e3;        // 3.900 .. 4.756 MHz
constexpr double kSecamMinHz = 3.9e6, kSecamMaxHz = 4.756e6;                            // the range both stay in, to within 250 Hz
constexpr double kSecamBellHz = 4286e3;      // f0 of the high-frequency pre-emphasis (item 2.13)
constexpr double kSecamM0 = 0.115;           // 2 M0 = 23 % of the luminance amplitude (blanking to peak white), item 2.13
constexpr double kSecamLf1Hz = 85e3;         // low-frequency pre-emphasis of D'R and D'B, item 2.7: A_BF = (1 + j f/f1) / (1 + j f/(3 f1))
// line-blanking reference ("lead-in"): the subcarrier at its rest frequency from this long after 0H (blanked before it, item 2.17), and the
// 9 lines of identification signals in the field blanking (Table 2 item 2.18 / note 24): trapezoids that end at D'R = +1.25 or D'B = -1.52
// (that is +350 and -350 kHz)
constexpr double kSecamLeadInUs = 5.6;
constexpr double kSecamBottleR = 1.25, kSecamBottleB = -1.52;
// A_BF(f), the low-frequency pre-emphasis the transmitter applies to D'R and D'B (the receiver applies the inverse)
std::complex<double> atvSecamLfPreEmph(double fHz);
// G(f) / M0 = (1 + j16 F) / (1 + j1.26 F), F = f/f0 - f0/f, for the instantaneous subcarrier frequency f > 0 ("anti-bell"; the receiver's bell is its inverse)
std::complex<double> atvSecamHfPreEmph(double fHz);

// EBU colour bars 100/0/75/0 (EBU Tech 3213, ITU-R BT.471-1): white, yellow, cyan, green, magenta, red, blue, black as R'G'B' in 0..1
void atvEbuBar(int i, float rgb[3]);

// The sound carrier spacings a receiver looks for (BT.470-6 Table 3 item 2): 5.5, 6.0 (5.9996), 6.5 (D/K, L), 4.5 (M, N), 3.5 (A: below the
// vision carrier, found in the mirrored spectrum), 11.15 (E)
constexpr int kAtvSpacingCount = 6;
double atvSpacingMhz(int i);

} // namespace dect2
