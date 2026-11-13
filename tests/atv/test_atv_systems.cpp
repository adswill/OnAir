// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV: the systems added after B/G, I, D/K, M and N: the positive-modulation systems with AM sound (A 405 lines, C, E 819 lines, L, L'),
// the colour combinations NTSC-N, NTSC 4.43 and SECAM-M, and NBTV / mechanical television (atv_nbtv.h). Generator -> receiver: line and field
// lock, the system found, the test card back within tolerance; positive modulation off frequency; the system found by itself.
#include "dect2/atv/atv_nbtv.h"
#include "dect2/atv/atv_testkit.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// the bars of the decoded card against the EBU bars: luminance only (monochrome) or every channel
static double barError(const AtvFrame& fr, const AtvFormat& f, bool colour) {
    AtvCard card(f);
    double c[8][3];
    barColours(fr, card, c);
    double worst = 0;
    for (int i = 0; i < 8; i++) {
        float rgb[3]; atvEbuBar(i, rgb);
        if (colour) { for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(c[i][k] - rgb[k])); continue; }
        float y, u, v; atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, v);
        worst = std::max(worst, std::fabs(0.299 * c[i][0] + 0.587 * c[i][1] + 0.114 * c[i][2] - y));
    }
    return worst;
}

// the grey ramp of the card: the mean error of the luminance along it
static double rampError(const AtvFrame& fr, const AtvFormat& f) {
    AtvCard card(f);
    int x0, x1, y0, y1;
    card.rampArea(x0, x1, y0, y1);
    double e = 0; int n = 0;
    const int ya = y0 + (y1 - y0) / 4, yb = y1 - (y1 - y0) / 4;
    for (int x = x0 + (x1 - x0) / 10; x < x1 - (x1 - x0) / 10; x++) {
        double rgb[3];
        meanRgb(fr, x, ya, x + 1, yb, rgb);
        const double want = (x - x0 + 0.5) / (x1 - x0);
        e += std::fabs(0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2] - want); n++;
    }
    return n ? e / n : 1;
}

struct Case {
    const char* name;
    int sys, colour;
    double rate, secs, cfo;
    bool force;                 // tell the receiver the system (otherwise it has to find it)
    const char* expect;         // the system it must report
    int lines;
    double barTol;
};

static void runCase(const Case& k) {
    AtvGenConfig c; c.sys = k.sys; c.colour = k.colour; c.rate = k.rate; c.cnrDb = 40; c.cfoHz = k.cfo; c.sound = 4;
    AtvFormat f; atvMakeFormat(k.sys, k.colour, f);
    const bool noSound = k.colour == kAtvSecam && (k.sys == kAtvM || k.sys == kAtvN);   // SECAM on M and N: no 4.5 MHz sound carrier (atv_std.h)
    if (noSound) c.sound = 3;
    Options o;
    if (k.force) o.onTime = [&](AtvReceiver& rx, double s) { if (s < 0.01 + 65536 / k.rate) rx.setStandard(k.sys, k.colour); };
    const Run r = run(c, k.secs, o);
    const AtvTelemetry& t = r.tel;
    double bars = 9, ramp = 9;
    if (r.frame && r.frame->width == f.picW && r.frame->height == f.picH) {
        bars = barError(*r.frame, f, k.colour != kAtvMono); ramp = rampError(*r.frame, f);
        if (std::getenv("ATV_DBG")) {
            AtvCard card(f); double cc[8][3]; barColours(*r.frame, card, cc);
            printf("  colour %d killer %d burst %.2f colourSystem %s\n", (int)t.colour, (int)t.colourKiller, t.burstLevel, t.colourSystem.c_str());
            for (int i = 0; i < 8; i++) printf("  bar %d: %.2f %.2f %.2f\n", i, cc[i][0], cc[i][1], cc[i][2]);
            for (const auto& l : r.log) printf("    log: %s\n", l.c_str());
        }
    }
    printf("%-28s state %d  %-22s lines %d  field %.3f Hz  line %+.1f ppm  cfo %+.0f Hz  sync %.0f%%  depth %.1f%%  bars %.3f  ramp %.3f  SNR %.1f dB  fields ok %llu bad %llu  sound %s\n",
           k.name, t.state, t.system.c_str(), t.lines, t.fieldHz, t.lineErrPpm, t.cfoHz, t.syncQuality * 100, t.syncDepthPct, bars, ramp, t.snrDb,
           (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.soundPresent ? "yes" : "no");
    if (t.state != 2) for (const auto& l : r.log) printf("    log: %s\n", l.c_str());
    CHECK(t.state == 2 && t.dataValid, "%s: line and field sync not locked (state %d)", k.name, t.state);
    CHECK(t.lines == k.lines && t.system == k.expect, "%s: found '%s' with %d lines, expected '%s'", k.name, t.system.c_str(), t.lines, k.expect);
    CHECK(std::fabs(t.fieldHz - f.fieldHz) < 0.01, "%s: field rate %.4f Hz", k.name, t.fieldHz);
    CHECK(t.blocksOk > 20 && t.blocksBad <= 3, "%s: fields ok %llu bad %llu", k.name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    CHECK(std::fabs(t.cfoHz - k.cfo) < 1500, "%s: carrier offset %.0f Hz, sent %.0f", k.name, t.cfoHz, k.cfo);
    CHECK(bars < k.barTol, "%s: colour bars off by %.3f (at most %.3f)", k.name, bars, k.barTol);
    CHECK(ramp < 0.06, "%s: grey ramp off by %.3f", k.name, ramp);
    if (!noSound && (f.amSound || k.sys == kAtvG)) CHECK(t.soundPresent, "%s: no sound", k.name);
    if (r.audio.size() > 48000 && t.soundPresent) {
        const double hz = peakTone(r.audio, r.audio.size() - 24000, r.audio.size());
        CHECK(std::fabs(hz - 1000) < 5, "%s: sound tone at %.1f Hz", k.name, hz);
    }
}

// NBTV through the radio: AM or narrow FM at 10 Msps, the transmitter `cfo` off the centre
static void nbtvRf(const char* name, const NbtvFormat& f, bool fm, double cfo, double secs) {
    NbtvRfConfig c; c.fmt = f; c.rate = 10e6; c.fm = fm; c.cfoHz = cfo; c.cnrDb = 35;
    AtvNbtvRfGen g(c);
    AtvReceiver rx;
    rx.setSilent(true);
    std::vector<std::string> log;
    rx.setLogCallback([&](const std::string& m) { log.push_back(m); });
    rx.setNbtv(f.lines, f.frameHz, 5, f.vertical);
    rx.setModulation(fm ? 3 : 2);
    rx.configure(c.rate);
    std::vector<cf32> buf(65536);
    uint64_t seq = 0, fseq = 0;
    AtvTelemetry t;
    std::shared_ptr<const AtvFrame> last;
    for (double done = 0; done < secs * c.rate; done += (double)buf.size()) {
        g.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
        if (auto fr = rx.frame(fseq)) last = fr;
    }
    // the picture against the test pattern, cell by cell (lines are columns, the scale is the decoder's)
    double bars = 9, ramp = 9;
    if (last) {
        const int lines = f.lines, along = f.alongLine();
        const int S = last->width / lines;
        double eb = 0, er = 0; int nb = 0, nr = 0;
        for (int li = 1; li < lines - 1; li++) {
            const double x = (li + 0.5) / lines;
            if (std::fabs(x * 8 - std::round(x * 8)) < 0.6 * 8.0 / lines) continue;     // a column on a bar edge
            for (int k = 1; k < along - 1; k++) {
                const double y = (k + 0.5) / along;
                if (std::fabs(y - 0.45) < 0.04 || std::fabs(y - 0.55) < 0.04) continue;
                const uint8_t* p = &last->rgba[((size_t)(k * S + S / 2) * last->width + (size_t)(li * S + S / 2)) * 4];
                const double d = std::fabs(p[0] / 255.0 - nbtvTestPicture(0, x, y));
                if (y < 0.45) { eb += d; nb++; } else if (y > 0.55) { er += d; nr++; }
            }
        }
        bars = nb ? eb / nb : 9; ramp = nr ? er / nr : 9;
    }
    printf("%-28s state %d  %-26s line %.2f Hz (%+.0f ppm)  pictures %llu  SNR %.1f dB  bars %.3f  ramp %.3f  picture %dx%d\n", name, t.state, t.system.c_str(),
           t.lineHz, t.lineErrPpm, (unsigned long long)t.frameCount, t.snrDb, bars, ramp, last ? last->width : 0, last ? last->height : 0);
    if (t.state != 2) for (const auto& l : log) printf("    log: %s\n", l.c_str());
    CHECK(t.state == 2, "%s: line and picture sync not locked (state %d)", name, t.state);
    CHECK(std::fabs(t.lineHz - f.lineHz()) < 0.002 * f.lineHz(), "%s: line rate %.3f Hz, expected %.3f", name, t.lineHz, f.lineHz());
    CHECK(t.frameCount >= (uint64_t)(0.6 * secs * f.frameHz), "%s: %llu pictures in %.1f s", name, (unsigned long long)t.frameCount, secs);
    CHECK(bars < 0.06 && ramp < 0.06, "%s: picture off: bars %.3f ramp %.3f", name, bars, ramp);
}

// NBTV as a sound file: 48 kHz, AC coupled (no DC), turned over, the decoder alone
static void nbtvAudio(const char* name, const NbtvFormat& f, bool invert) {
    AtvNbtvGen g(f, 48000);
    AtvNbtvDecoder d;
    d.configure(48000, f);
    std::shared_ptr<const AtvFrame> last;
    uint64_t frames = 0;
    d.frameReady = [&](std::shared_ptr<const AtvFrame> fr) { last = fr; frames++; };
    std::vector<float> x(4800);
    double dc = 0;
    for (int b = 0; b < 60; b++) {          // 6 s
        g.generate(x.data(), x.size());
        for (auto& v : x) {
            float s = 2 * v - 1;            // -1 .. +1, then a 5 Hz high-pass like a sound card
            dc += (s - dc) * (2 * M_PI * 5 / 48000);
            s -= (float)dc;
            v = invert ? -s : s;
        }
        d.process(x.data(), x.size());
    }
    const NbtvStatus s = d.status();
    double err = 9;
    if (last) {
        const int along = f.alongLine(), S = d.scale();
        double e = 0; int n = 0;
        for (int li = 1; li < f.lines - 1; li++) {
            const double xx = (li + 0.5) / f.lines;
            if (std::fabs(xx * 8 - std::round(xx * 8)) < 0.6 * 8.0 / f.lines) continue;
            for (int k = 2; k < along - 2; k++) {
                const double y = (k + 0.5) / along;
                if (std::fabs(y - 0.45) < 0.05 || std::fabs(y - 0.55) < 0.05) continue;
                const uint8_t* p = &last->rgba[((size_t)(k * S + S / 2) * last->width + (size_t)(li * S + S / 2)) * 4];
                e += std::fabs(p[0] / 255.0 - nbtvTestPicture(0, xx, y)); n++;
            }
        }
        err = n ? e / n : 9;
        if (std::getenv("NBTV_DBG"))
            for (int li : {2, 13, 25}) {
                printf("  line %d:", li);
                for (int k = 0; k < along; k += 2) { const uint8_t* p = &last->rgba[((size_t)(k * S + S / 2) * last->width + (size_t)(li * S + S / 2)) * 4]; printf(" %.2f/%.2f", p[0] / 255.0, nbtvTestPicture(0, (li + 0.5) / f.lines, (k + 0.5) / along)); }
                printf("\n");
            }
    }
    printf("%-28s line %d frame %d  line %.2f Hz  pictures %llu  SNR %.1f dB  picture error %.3f\n", name, (int)s.lineLocked, (int)s.frameLocked, s.lineHz,
           (unsigned long long)frames, s.snrDb, err);
    CHECK(s.lineLocked && s.frameLocked, "%s: not locked (line %d, frame %d)", name, (int)s.lineLocked, (int)s.frameLocked);
    CHECK(frames >= 30, "%s: %llu pictures", name, (unsigned long long)frames);
    CHECK(err < 0.06, "%s: picture error %.3f", name, err);
}

int main(int argc, char** argv) {
    // an argument: run only the cases whose name contains one of its parts (separated by |)
    std::vector<std::string> only;
    if (argc > 1) { std::string a = argv[1]; size_t p = 0; while (true) { const size_t q = a.find('|', p); only.push_back(a.substr(p, q - p)); if (q == std::string::npos) break; p = q + 1; } }
    auto want = [&](const char* n) { if (only.empty()) return true; for (const auto& o : only) if (strstr(n, o.c_str())) return true; return false; };
    // the formats: BT.470-6 numbers
    {
        AtvFormat a, e, l, lp;
        atvMakeFormat(kAtvA, kAtvMono, a); atvMakeFormat(kAtvE, kAtvMono, e); atvMakeFormat(kAtvL, kAtvSecam, l); atvMakeFormat(kAtvLp, kAtvSecam, lp);
        CHECK(a.lines == 405 && std::fabs(a.lineHz - 10125) < 1e-9 && a.positive && a.amSound && a.soundSpacingMhz == -3.5 && a.videoBwMhz == 3.0, "system A");
        CHECK(e.lines == 819 && std::fabs(e.lineHz - 20475) < 1e-9 && e.positive && e.soundSpacingMhz == 11.15 && e.videoBwMhz == 10.0 && e.blankPct == 25, "system E");
        CHECK(l.lines == 625 && l.positive && l.amSound && l.soundSpacingMhz == 6.5 && l.vestigialMhz == 1.25 && l.name == "SECAM L 625/50", "system L: %s", l.name.c_str());
        CHECK(lp.mirrored && lp.soundSpacingMhz == -6.5 && std::fabs(atvVisionOffsetHz(lp) - 2.75e6) < 1, "system L'");
        AtvFormat h, kp, nn, n443, sm;
        atvMakeFormat(kAtvH, kAtvPal, h); atvMakeFormat(kAtvKp, kAtvSecam, kp); atvMakeFormat(kAtvN, kAtvNtsc, nn); atvMakeFormat(kAtvI, kAtvNtsc, n443); atvMakeFormat(kAtvM, kAtvSecam, sm);
        CHECK(!h.positive && h.soundSpacingMhz == 5.5 && h.vestigialMhz == 1.25 && !kp.positive && kp.soundSpacingMhz == 6.5 && kp.vestigialMhz == 1.25, "systems H and K'");
        CHECK(nn.name == "NTSC-N 625/50" && std::fabs(nn.fscHz - 3582056.25) < 1e-6 && !nn.vSwitch, "NTSC-N: %s %.2f", nn.name.c_str(), nn.fscHz);
        CHECK(n443.name == "NTSC 4.43 I 625/50" && std::fabs(n443.fscHz - 4433618.75) < 1e-6, "NTSC 4.43: %s", n443.name.c_str());
        CHECK(sm.name == "SECAM M 525/59.94" && sm.lines == 525, "SECAM-M: %s", sm.name.c_str());
        // the field structure: 8 broad pulses, no equalising pulses, the second field half a line later
        int broad = 0;
        for (int hh = 0; hh < a.halfLines; hh++) if (a.pulseAt(hh) == 3) broad++;
        CHECK(broad == 16 && a.pulseAt(0) == 3 && a.pulseAt(405) == 3 && a.pulseAt(403) == 0 && a.pulseAt(16) == 1, "405 lines: %d broad pulses", broad);
    }
    // generator -> receiver
    const Case cases[] = {
        {"A 405 lines (forced)", kAtvA, kAtvMono, 10e6, 2.0, 0, true, "A 405/50 mono", 405, 0.06},
        {"C 625 positive (forced)", kAtvC, kAtvMono, 10e6, 2.0, 0, true, "C 625/50 mono", 625, 0.06},
        {"E 819 lines (forced)", kAtvE, kAtvMono, 20e6, 1.6, 0, true, "E 819/50 mono", 819, 0.06},
        {"L SECAM (forced)", kAtvL, kAtvSecam, 10e6, 2.5, 0, true, "SECAM L 625/50", 625, 0.12},
        {"NTSC-N", kAtvN, kAtvNtsc, 10e6, 2.5, 0, true, "NTSC-N 625/50", 625, 0.10},
        {"NTSC 4.43 on I", kAtvI, kAtvNtsc, 10e6, 2.5, 0, false, "NTSC 4.43 I 625/50", 625, 0.10},
        {"SECAM-M", kAtvM, kAtvSecam, 10e6, 2.5, 0, true, "SECAM M 525/59.94", 525, 0.14},
        {"PAL H (forced)", kAtvH, kAtvPal, 10e6, 2.0, 0, true, "PAL H 625/50", 625, 0.09},
        {"SECAM K' (forced)", kAtvKp, kAtvSecam, 10e6, 2.5, 0, true, "SECAM K' 625/50", 625, 0.12},
        // positive modulation off frequency
        {"L +50 kHz", kAtvL, kAtvMono, 10e6, 2.0, 50e3, true, "L 625/50 mono", 625, 0.06},
        {"A -50 kHz", kAtvA, kAtvMono, 10e6, 2.0, -50e3, true, "A 405/50 mono", 405, 0.06},
        {"C +50 kHz (found)", kAtvC, kAtvMono, 10e6, 2.0, 50e3, false, "C 625/50 mono", 625, 0.06},
        // found by the receiver: polarity, line count, sound spacing, the channel turned round
        {"auto A", kAtvA, kAtvMono, 10e6, 2.5, 0, false, "A 405/50 mono", 405, 0.06},
        {"auto E", kAtvE, kAtvMono, 20e6, 1.6, 0, false, "E 819/50 mono", 819, 0.06},
        {"auto L", kAtvL, kAtvSecam, 10e6, 2.5, -50e3, false, "SECAM L 625/50", 625, 0.12},
        {"auto L'", kAtvLp, kAtvSecam, 10e6, 2.5, 0, false, "SECAM L' 625/50", 625, 0.12},
        {"auto B/G", kAtvG, kAtvPal, 10e6, 2.0, 0, false, "PAL B/G 625/50", 625, 0.09},
    };
    for (const Case& k : cases) if (want(k.name)) runCase(k);
    if (want("NBTV")) {
        nbtvRf("NBTV 32 lines AM", nbtvStandard(), false, 0, 3.0);
        nbtvRf("NBTV 32 lines FM +40 kHz", nbtvStandard(), true, 40e3, 3.0);
        nbtvAudio("NBTV 32 lines audio file", nbtvStandard(), false);
        nbtvAudio("NBTV audio turned over", nbtvStandard(), true);
    }
    if (want("mechanical")) {
        nbtvRf("mechanical 60 lines AM -50k", nbtvMechanical(60, 12.5), false, -50e3, 3.0);
        nbtvAudio("mechanical 60 lines audio", nbtvMechanical(60, 12.5), false);
    }
    printf(fails ? "atv systems: %d FAILED\n" : "atv systems: all passed\n", fails);
    return fails ? 1 : 0;
}
