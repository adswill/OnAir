// Analog TV: known answers from the standards (ITU-R BT.470-6, EBU colour bars). The tables in atv_std are checked against numbers
// written down here from the specification, and against well known published values (the vectorscope angles of the colour bars).
#include "dect2/atv_std.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main() {
    // ---- timing, BT.470-6 Table 1 and Table 1-1
    {
        AtvFormat f;
        CHECK(atvMakeFormat(kAtvG, kAtvPal, f), "B/G PAL exists");
        CHECK(f.lines == 625 && near(f.fieldHz, 50, 1e-9) && near(f.lineHz, 15625, 1e-9) && near(f.lineUs, 64, 1e-9), "625/50 raster");
        CHECK(f.halfLines == 1250, "half-lines");
        CHECK(near(f.syncUs, 4.7, 1e-9), "sync pulse 4.7 us");
        CHECK(near(f.frontPorchUs + f.blankEndUs, 12.0, 1e-9), "line blanking 12 us (Table 1-1 symbol a)");
        CHECK(near(f.blankEndUs, 10.5, 1e-9), "back edge of blanking 10.5 us after 0H (symbol b)");
        CHECK(near(f.lineUs - f.frontPorchUs - f.blankEndUs, f.activeUs, 1e-9) && near(f.activeUs, 52, 1e-9), "active line 52 us");
        CHECK(near(f.eqUs, 2.35, 1e-9) && near(f.broadUs, 27.3, 1e-9), "equalising 2.35 us, field sync 27.3 us (Table 1-2)");
        CHECK(near(f.syncLevel, -0.3 / 0.7, 1e-12), "sync 300 mV against 700 mV white");
        CHECK(f.setup == 0, "no setup in B/G");
        CHECK(near(f.fscHz, 4433618.75, 1e-6), "PAL subcarrier 4 433 618.75 Hz, got %.4f", f.fscHz);
        CHECK(near(f.fscHz, (283.75 + 1.0 / 625) * 15625, 1e-6), "fsc = (1135/4 + 1/625) fH");
        CHECK(near(f.burstAmp * 2 * 0.7, 0.3, 1e-9), "burst 300 mV peak to peak");
        CHECK(f.burstCycles == 10 && near(f.burstStartUs, 5.6, 1e-9), "burst 10 cycles from 5.6 us");
        CHECK(near(f.soundSpacingMhz, 5.5, 1e-9) && near(f.soundDevKhz, 50, 1e-9) && near(f.preEmphUs, 50, 1e-9), "B/G sound +5.5 MHz, 50 kHz, 50 us (Table 3)");
        CHECK(near(f.chanMhz, 8, 1e-9) && near(f.visionOffMhz, 1.25, 1e-9), "8 MHz channel, vision carrier 1.25 MHz above the lower edge");
        CHECK(near(atvVisionOffsetHz(f), -2.75e6, 1e-6) && near(atvSoundOffsetHz(f), 2.75e6, 1e-6), "carriers at -2.75 and +2.75 MHz from the centre of a G channel");
        AtvFormat b; atvMakeFormat(kAtvB, kAtvPal, b);
        CHECK(near(atvVisionOffsetHz(b), -2.25e6, 1e-6) && near(atvSoundOffsetHz(b), 3.25e6, 1e-6), "B channel: -2.25 and +3.25 MHz");
        AtvFormat i; atvMakeFormat(kAtvI, kAtvPal, i);
        CHECK(near(i.soundSpacingMhz, 5.9996, 1e-9) && near(i.vestigialMhz, 1.25, 1e-9) && near(i.whitePct, 20, 1e-9), "system I: sound +5.9996 MHz, vestigial sideband 1.25 MHz, white 20 %%");
        AtvFormat d; atvMakeFormat(kAtvDK, kAtvPal, d);
        CHECK(near(d.soundSpacingMhz, 6.5, 1e-9) && near(d.videoBwMhz, 6.0, 1e-9), "D/K sound +6.5 MHz, 6 MHz video");
        AtvFormat n; atvMakeFormat(kAtvM, kAtvNtsc, n);
        CHECK(n.lines == 525 && near(n.lineHz, 15734.265734, 1e-4) && near(n.fieldHz, 59.94006, 1e-4), "525 lines, 15 734.27 Hz, 59.94 fields/s");
        CHECK(near(n.lineUs, 63.5556, 1e-3), "line 63.5556 us");
        CHECK(near(n.fscHz, 3579545.4545, 1e-3) && near(n.fscHz, 315e6 / 88, 1e-3), "NTSC subcarrier 3 579 545.45 Hz = 455/2 fH");
        CHECK(near(n.frontPorchUs + n.blankEndUs, 10.9, 1e-9), "NTSC blanking 10.9 us");
        CHECK(near(n.syncLevel, -0.4, 1e-12) && near(n.setup, 0.075, 1e-12), "NTSC: sync -40 IRE, setup 7.5 IRE");
        CHECK(n.burstCycles == 9 && near(n.burstAmp * 2, 0.4, 1e-12), "NTSC burst 9 cycles, 40 IRE peak to peak");
        CHECK(near(n.soundSpacingMhz, 4.5, 1e-9) && near(n.soundDevKhz, 25, 1e-9) && near(n.preEmphUs, 75, 1e-9), "M: sound +4.5 MHz, 25 kHz, 75 us");
        CHECK(near(n.eqUs, 2.3, 1e-9) && near(n.broadUs, 27.1, 1e-9) && n.eqPulses == 6, "NTSC: 6 equalising pulses of 2.3 us, field sync 27.1 us");
        AtvFormat pm; atvMakeFormat(kAtvM, kAtvPal, pm);
        CHECK(near(pm.fscHz, 909.0 / 4 * pm.lineHz, 1e-6) && near(pm.fscHz, 3575611.9, 1.0), "PAL-M subcarrier 3.575611 MHz, got %.2f", pm.fscHz);
        AtvFormat pn; atvMakeFormat(kAtvN, kAtvPal, pn);
        CHECK(near(pn.fscHz, 3582056.25, 1e-6), "PAL-N (Argentina) subcarrier 3 582 056.25 Hz, got %.4f", pn.fscHz);
        AtvFormat bad;
        CHECK(!atvMakeFormat(kAtvG, kAtvNtsc, bad) && !atvMakeFormat(kAtvM, kAtvSecam, bad) && !atvMakeFormat(kAtvN, kAtvSecam, bad), "impossible combinations are refused");
        CHECK(atvMakeFormat(kAtvDK, kAtvSecam, bad) && bad.secam, "SECAM exists on D/K");
        CHECK(near(kSecamF0R, 282 * 15625.0, 1e-6) && near(kSecamF0B, 272 * 15625.0, 1e-6), "SECAM rest frequencies 282 fH and 272 fH");
    }
    // ---- the synchronising waveform (BT.470-6 Table 1-2, Fig. 2)
    for (int sys : {kAtvG, kAtvM}) {
        AtvFormat f;
        atvMakeFormat(sys, sys == kAtvM ? kAtvNtsc : kAtvPal, f);
        int count[4] = {};
        for (int h = 0; h < f.halfLines; h++) count[f.pulseAt(h)]++;
        const int eq = f.lines == 625 ? 5 : 6;
        CHECK(count[3] == 2 * eq && count[2] == 4 * eq, "%s: %d broad and %d equalising pulses a frame, expected %d and %d", f.name.c_str(), count[3], count[2], 2 * eq, 4 * eq);
        // sequences: equalising, broad, equalising, each eq pulses long, and the pulses are one half-line apart
        for (int fl = 0; fl < 2; fl++) {
            const int s = f.broadStart[fl];
            bool ok = true;
            for (int k = 1; k <= eq; k++) ok &= f.pulseAt(s - k) == 2;
            for (int k = 0; k < eq; k++) ok &= f.pulseAt(s + k) == 3;
            for (int k = eq; k < 2 * eq; k++) ok &= f.pulseAt(s + k) == 2;
            CHECK(ok, "%s: field %d sequence 2-3-2", f.name.c_str(), fl + 1);
        }
        // Note 2 and 3 of Fig. 2: the field-synchronising pulse of the first field coincides with a line sync edge, the second falls midway
        CHECK((f.broadStart[0] & 1) == 0 && (f.broadStart[1] & 1) == 1, "%s: first field starts on the line grid, the second half a line off", f.name.c_str());
        CHECK(f.broadStart[1] - f.broadStart[0] == f.lines, "%s: the fields are %d half-lines (half a frame) apart", f.name.c_str(), f.lines);
        // regular line pulses sit on even half-lines only
        bool even = true;
        for (int h = 0; h < f.halfLines; h++) if (f.pulseAt(h) == 1 && (h & 1)) even = false;
        CHECK(even, "%s: line syncs on the line grid", f.name.c_str());
        // burst blanking: 9 lines in PAL, 11 in NTSC around each field sync (Table 2 item 2.17)
        int nb = 0;
        for (int l = 1; l <= f.lines; l++) if (!f.burstOnLine(l)) nb++;
        CHECK(nb == (f.lines == 625 ? 18 : 22), "%s: %d lines without burst", f.name.c_str(), nb);
    }
    // ---- colour: BT.470-6 Table 2 item 2.4, 2.5; the vectorscope angles of EBU colour bars are the usual published numbers
    {
        float rgb[3];
        const double yExp[8] = {1.0, 0.6645, 0.5258, 0.4403, 0.3098, 0.2243, 0.0855, 0.0};
        const double angExp[8] = {0, 167.1, 283.5, 240.7, 60.7, 103.5, 347.1, 0};   // degrees of atan2(V, U)
        const double ampExp[8] = {0, 0.3361, 0.4741, 0.4429, 0.4429, 0.4741, 0.3361, 0};
        for (int i = 0; i < 8; i++) {
            atvEbuBar(i, rgb);
            float y, u, v;
            atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, v);
            CHECK(near(y, yExp[i], 6e-4), "bar %d: Y %.4f, expected %.4f", i, y, yExp[i]);
            if (i > 0 && i < 7) {
                double a = std::atan2(v, u) * 180 / M_PI;
                if (a < 0) a += 360;
                CHECK(near(a, angExp[i], 0.15), "bar %d: vector angle %.2f, expected %.1f", i, a, angExp[i]);
                CHECK(near(std::hypot(u, v), ampExp[i], 6e-4), "bar %d: chroma amplitude %.4f, expected %.4f", i, std::hypot(u, v), ampExp[i]);
            }
            float r, g, b;
            atvYuvToRgb(y, u, v, r, g, b);
            CHECK(near(r, rgb[0], 2e-5) && near(g, rgb[1], 2e-5) && near(b, rgb[2], 2e-5), "bar %d: YUV to RGB round trip", i);
        }
        // the coefficients themselves
        float y, u, v;
        atvRgbToYuv(1, 0, 0, y, u, v);
        CHECK(near(y, 0.299, 1e-6) && near(u, 0.493 * (0 - 0.299), 1e-6) && near(v, 0.877 * (1 - 0.299), 1e-6), "red: Y 0.299, U = 0.493 (B-Y), V = 0.877 (R-Y)");
        atvRgbToYuv(0, 1, 0, y, u, v);
        CHECK(near(y, 0.587, 1e-6), "green luminance 0.587");
        atvRgbToYuv(0, 0, 1, y, u, v);
        CHECK(near(y, 0.114, 1e-6) && near(u, 0.493 * (1 - 0.114), 1e-6), "blue: Y 0.114");
        // the NTSC I and Q axes are U and V rotated by 33 degrees (item 2.9): I = V cos33 - U sin33, Q = V sin33 + U cos33 gives the
        // published weights I = 0.74 (R-Y) - 0.27 (B-Y), Q = 0.48 (R-Y) + 0.41 (B-Y)
        const double c33 = std::cos(33 * M_PI / 180), s33 = std::sin(33 * M_PI / 180);
        const double iR = 0.877 * c33, iB = -0.493 * s33, qR = 0.877 * s33, qB = 0.493 * c33;
        CHECK(near(iR, 0.74, 0.01) && near(iB, -0.27, 0.01) && near(qR, 0.48, 0.01) && near(qB, 0.41, 0.01), "I/Q axes: %.3f %.3f %.3f %.3f", iR, iB, qR, qB);
        // SECAM D'R = -1.902 (R-Y), D'B = 1.505 (B-Y)
        atvRgbToYuv(1, 0, 0, y, u, v);
        CHECK(near(atvSecamDr(1, y), -1.902 * 0.701, 1e-5), "SECAM D'R");
        atvRgbToYuv(0, 0, 1, y, u, v);
        CHECK(near(atvSecamDb(1, y), 1.505 * 0.886, 1e-5), "SECAM D'B");
        // the 75 % bars at 0.7 V: peak of the composite signal equals white (0.7 V) for yellow and cyan: Y + chroma amplitude <= 1
        for (int i = 1; i < 7; i++) {
            atvEbuBar(i, rgb);
            atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, v);
            CHECK(y + std::hypot(u, v) <= 1.0 + 1e-3, "bar %d stays within white (%.4f)", i, y + std::hypot(u, v));
        }
    }
    // ---- SECAM: BT.470-6 Table 2 items 2.7, 2.10 to 2.13. Values computed by hand from the formulas of the Recommendation.
    {
        // high-frequency pre-emphasis G(f)/M0 = (1 + j16F) / (1 + j1.26F), F = f/f0 - f0/f, f0 = 4.286 MHz
        auto g = [](double f) { return atvSecamHfPreEmph(f); };
        CHECK(near(std::abs(g(4.286e6)), 1.0, 1e-9) && near(std::arg(g(4.286e6)), 0.0, 1e-9), "G at the centre of the bell is 1");
        CHECK(near(std::abs(g(4.40625e6)), 1.3326, 5e-4), "|G| at the D'R rest frequency %.4f, expected 1.3326", std::abs(g(4.40625e6)));
        CHECK(near(std::arg(g(4.40625e6)) * 180 / M_PI, 37.5, 0.1), "arg G at 4.40625 MHz %.2f deg, expected 37.5", std::arg(g(4.40625e6)) * 180 / M_PI);
        CHECK(near(std::abs(g(4.25e6)), 1.0356, 5e-4), "|G| at the D'B rest frequency %.4f, expected 1.0356", std::abs(g(4.25e6)));
        CHECK(near(std::abs(g(3.9e6)), 3.0994, 2e-3), "|G| at 3.9 MHz %.4f, expected 3.0994", std::abs(g(3.9e6)));
        CHECK(near(std::abs(g(4.756e6)), 3.3680, 2e-3), "|G| at 4.756 MHz %.4f, expected 3.3680", std::abs(g(4.756e6)));
        // low-frequency pre-emphasis A_BF = (1 + j f/f1) / (1 + j f/(3 f1)), f1 = 85 kHz
        auto a = [](double f) { return atvSecamLfPreEmph(f); };
        CHECK(near(std::abs(a(0)), 1.0, 1e-12), "A_BF at DC is 1");
        CHECK(near(std::abs(a(85e3)), 1.3416, 5e-4), "|A_BF| at 85 kHz %.4f, expected 1.3416", std::abs(a(85e3)));
        CHECK(near(std::abs(a(1.3e6)), 2.950, 2e-3), "|A_BF| at 1.3 MHz %.4f, expected 2.950", std::abs(a(1.3e6)));
        CHECK(near(std::abs(a(1e9)), 3.0, 1e-3), "A_BF tends to 3 at high frequencies");
        // rest frequencies are 282 fH and 272 fH, the limits are 3.9 and 4.756 MHz, 2 M0 = 23 %
        CHECK(near(kSecamF0R, 282 * 15625.0, 1e-6) && near(kSecamF0B, 272 * 15625.0, 1e-6), "rest frequencies");
        CHECK(near(kSecamMinB, 3.9e6, 1) && near(kSecamMaxB, 4.756e6, 1) && near(kSecamMinR, 3900250, 1) && near(kSecamMaxR, 4756250, 1), "frequency limits %.0f %.0f / %.0f %.0f", kSecamMinR, kSecamMaxR, kSecamMinB, kSecamMaxB);
        CHECK(near(2 * kSecamM0, 0.23, 1e-12), "2 M0 = 23 %%");
        // the deviation of the 75 % colour bars: D'R = -1.902 (R - Y) reaches +-1 on the saturated bars, so +-280 kHz and +-230 kHz
        for (int i = 1; i < 7; i++) {
            float rgb[3], y, u, v;
            atvEbuBar(i, rgb);
            atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, v);
            const double dr = atvSecamDr(rgb[0], y), db = atvSecamDb(rgb[2], y);
            CHECK(std::fabs(dr) <= 1.0 + 1e-3 && std::fabs(db) <= 1.0 + 1e-3, "bar %d: D'R %.4f D'B %.4f within 1", i, dr, db);
            const double fr = kSecamF0R + kSecamDevR * dr, fb = kSecamF0B + kSecamDevB * db;
            CHECK(fr >= kSecamMinR && fr <= kSecamMaxR && fb >= kSecamMinB && fb <= kSecamMaxB, "bar %d: subcarriers at %.0f and %.0f Hz inside the limits", i, fr, fb);
        }
        // the identification signals end at the limits: D'R = +1.25 gives +350 kHz, D'B = -1.52 gives -350 kHz
        CHECK(near(kSecamF0R + kSecamDevR * kSecamBottleR, kSecamMaxR, 1), "identification signal on D'R lines ends at %.0f", kSecamF0R + kSecamDevR * kSecamBottleR);
        CHECK(near(kSecamF0B + kSecamDevB * kSecamBottleB, kSecamMinB, 1000), "identification signal on D'B lines ends at %.0f", kSecamF0B + kSecamDevB * kSecamBottleB);
        // lines of the identification signals and the lines with a lead-in
        AtvFormat f; atvMakeFormat(kAtvG, kAtvSecam, f);
        int nBottle = 0, nLead = 0;
        for (int l = 1; l <= 625; l++) { if (f.secamBottleLine(l)) nBottle++; if (f.burstOnLine(l)) nLead++; }
        CHECK(nBottle == 18, "%d identification lines, expected 9 + 9", nBottle);
        CHECK(nLead == 2 * 288 && !f.burstOnLine(22) && f.burstOnLine(23) && f.burstOnLine(310) && !f.burstOnLine(311) && f.burstOnLine(336) && f.burstOnLine(623) && !f.burstOnLine(624), "lines with a lead-in: %d", nLead);
    }
    // the sound carrier spacings a receiver searches
    CHECK(near(atvSpacingMhz(0), 5.5, 1e-9) && near(atvSpacingMhz(1), 6.0, 1e-9) && near(atvSpacingMhz(2), 6.5, 1e-9) && near(atvSpacingMhz(3), 4.5, 1e-9), "spacings");
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
