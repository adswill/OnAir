// Analog TV receiver against things in the path: a gap in the samples, impulse noise, carriers of other signals, ghosts, mains hum, sync compression,
// a step in the signal level, and the controls (the detector, the standard) while it runs.
#include "dect2/atv_testkit.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static double barError(const Run& r, const AtvFormat& f) {
    if (!r.frame) return 9;
    AtvCard card(f);
    double c[8][3], worst = 0;
    barColours(*r.frame, card, c);
    for (int i = 0; i < 8; i++) {
        float rgb[3]; atvEbuBar(i, rgb);
        for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(c[i][k] - rgb[k]));
    }
    return worst;
}

static AtvGenConfig base(int sys = kAtvG, int col = kAtvPal) {
    AtvGenConfig c; c.sys = sys; c.colour = col; c.cnrDb = 40; c.rate = 10e6; c.sound = 4;
    return c;
}

static void line(const char* what, const Run& r, double bars) {
    printf("%-40s state %d %-16s colour %d  bars %.3f  video %.1f dB  sync %.1f %%  compression %.0f %%  line %+.1f ppm  fields bad %llu\n", what, r.tel.state,
           r.tel.system.empty() ? "-" : r.tel.system.c_str(), (int)r.tel.colour, bars, r.tel.snrDb, r.tel.syncDepthPct, r.tel.syncCompressionPct, r.tel.lineErrPpm, (unsigned long long)r.tel.blocksBad);
}

int main() {
    AtvFormat gp; atvMakeFormat(kAtvG, kAtvPal, gp);
    // ---- a gap in the samples (the radio loses a buffer, the USB stalls): the receiver finds its way back and says how long that took
    for (double ms : {2.0, 5.0, 20.0, 60.0, 250.0}) {
        AtvGenConfig c = base();
        Options o; o.impair = dropout(1.5, ms, 10e6);
        const Run r = run(c, 3.5 + ms * 1e-3, o);
        const double end = 1.5 + ms * 1e-3;
        double firstAfter = -1;
        for (double t : r.frameTimes) if (t > end) { firstAfter = t - end; break; }
        int bad = 0;
        for (const auto& s : r.stateLog) if (s.first > end + 1.0 && s.second != 2) bad++;
        char nm[48]; snprintf(nm, sizeof nm, "gap of %.0f ms", ms);
        line(nm, r, barError(r, gp));
        printf("   first picture %.0f ms after the end of the gap, fields lost in the telemetry %llu, reports out of lock one second on: %d\n", firstAfter * 1e3,
               (unsigned long long)r.tel.blocksBad, bad);
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < 0.06, "gap %.0f ms: state %d colour %d bars %.3f", ms, r.tel.state, (int)r.tel.colour, barError(r, gp));
        CHECK(firstAfter >= 0 && firstAfter < (ms < 100 ? 0.30 : 3.0), "gap %.0f ms: the first picture came %.0f ms after the end", ms, firstAfter * 1e3);
        CHECK(bad == 0, "gap %.0f ms: %d reports out of lock more than a second after the gap", ms, bad);
        const size_t n = r.audio.size();
        CHECK(n > 48000 * 3 && near(toneAmp(r.audio, 1000, n - 24000, n), 0.5, 0.05), "gap %.0f ms: the sound did not come back", ms);
    }
    // ---- the carrier jumps (an oscillator that is retuned, a transmitter that is switched): the loop has to find it again
    for (double hz : {5e3, 40e3, -200e3}) {
        AtvGenConfig c = base();
        Options o; o.impair = frequencyStep(1.5, hz, 10e6);
        const Run r = run(c, 4.0, o);
        double firstAfter = -1;
        for (double t : r.frameTimes) if (t > 1.6) { firstAfter = t - 1.5; break; }
        char nm[48]; snprintf(nm, sizeof nm, "carrier jumps by %+.0f kHz", hz / 1e3);
        line(nm, r, barError(r, gp));
        int bad = 0;
        for (const auto& s : r.stateLog) if (s.first > 3.0 && s.second != 2) bad++;
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < 0.06 && bad == 0, "carrier jump %+.0f kHz: state %d colour %d bars %.3f, %d reports out of lock after 3 s", hz / 1e3, r.tel.state, (int)r.tel.colour, barError(r, gp), bad);
        CHECK(near(r.tel.cfoHz, hz, 500), "carrier jump %+.0f kHz: carrier offset read as %.0f Hz", hz / 1e3, r.tel.cfoHz);
    }
    // ---- impulse noise (ignition, switches): bursts of 5 us at three times the carrier, 50 a second
    {
        AtvGenConfig c = base();
        Options o; o.impair = impulses(200000, 50, 1.5f);
        const Run r = run(c, 3.0, o);
        line("impulse noise, 50 bursts a second", r, barError(r, gp));
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < 0.06, "impulses: state %d bars %.3f", r.tel.state, barError(r, gp));
        Options o2; o2.impair = impulses(10000, 30, 1.0f);
        const Run q = run(c, 3.0, o2);
        line("impulse noise, 1000 bursts a second", q, barError(q, gp));
        CHECK(q.tel.state == 2, "heavy impulses: state %d", q.tel.state);
    }
    // ---- a strong carrier in the part of the band that is outside the channel but inside the radio's band (the sound carrier of the next channel
    // down aliases there at 10 Msps): the receiver must take the vision carrier of the channel for the picture, not the stronger line
    for (double db : {-30.0, -15.0, -6.0, 0.0}) {
        AtvGenConfig c = base();
        Options o; o.impair = tone(4.75e6, 0.4 * std::pow(10.0, db / 20), 10e6);
        const Run r = run(c, 6.0, o);
        char nm[48]; snprintf(nm, sizeof nm, "CW at +4.75 MHz, %.0f dB", db);
        line(nm, r, barError(r, gp));
        CHECK(r.tel.state == 2 && near(r.tel.visionHz, -2.75e6, 1000) && r.tel.colour && barError(r, gp) < 0.06, "CW %.0f dB: state %d vision carrier %.0f Hz bars %.3f", db, r.tel.state, r.tel.visionHz, barError(r, gp));
    }
    // ---- a carrier inside the picture band: a beat pattern in the picture, nothing worse
    for (double db : {-40.0, -30.0, -20.0}) {
        AtvGenConfig c = base();
        Options o; o.impair = tone(-1.0e6, 0.4 * std::pow(10.0, db / 20), 10e6);
        const Run r = run(c, 3.0, o);
        char nm[48]; snprintf(nm, sizeof nm, "CW at -1.0 MHz, %.0f dB", db);
        line(nm, r, barError(r, gp));
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < (db <= -40 ? 0.06 : db <= -30 ? 0.08 : 0.18), "CW in band %.0f dB: bars %.3f", db, barError(r, gp));
    }
    // ---- ghosts (a delayed copy): the delay of 1.5 us is about 22 pixels; in-phase and quadrature phases
    for (double db : {25.0, 15.0, 9.0}) for (double ph : {0.0, 90.0, 180.0}) {
        AtvGenConfig c = base(); c.echoDb = db; c.echoDelayUs = 1.5; c.echoPhaseDeg = ph;
        const Run r = run(c, 2.5);
        char nm[48]; snprintf(nm, sizeof nm, "ghost %.0f dB, %.0f deg", db, ph);
        const double w = barError(r, gp);
        line(nm, r, w);
        CHECK(r.tel.state == 2 && r.tel.colour && w < (db >= 25 ? 0.07 : db >= 15 ? 0.13 : 0.25), "ghost %.0f dB %.0f deg: state %d colour %d bars %.3f", db, ph, r.tel.state, (int)r.tel.colour, w);
    }
    // ---- mains hum on the carrier: bars crawl up the picture
    for (double h : {4.0, 10.0}) {
        AtvGenConfig c = base(); c.humPct = h; c.humHz = 50;
        const Run r = run(c, 3.0);
        char nm[48]; snprintf(nm, sizeof nm, "hum %.0f %%", h);
        line(nm, r, barError(r, gp));
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < (h < 5 ? 0.06 : 0.09), "hum %.0f %%: state %d bars %.3f", h, r.tel.state, barError(r, gp));
    }
    // ---- sync compression: everything below blanking is squeezed (the sync pulses and the lower half of every burst cycle); the picture gain follows the
    // burst (as high as the sync when nothing is squeezed), corrected for the lifted level of the burst, and only when the sync height says it is squeezed
    for (double comp : {0.2, 0.4, 0.6}) {
        AtvGenConfig c = base(); c.syncCompression = comp;
        const Run r = run(c, 3.0);
        char nm[48]; snprintf(nm, sizeof nm, "sync compression %.0f %%", comp * 100);
        const double w = barError(r, gp);
        line(nm, r, w);
        CHECK(r.tel.state == 2 && r.tel.colour, "compression %.0f %%: state %d colour %d", comp * 100, r.tel.state, (int)r.tel.colour);
        CHECK(near(r.tel.syncCompressionPct, comp * 100 * 0.75, 0.15 * comp * 100 + 3) || near(r.tel.syncCompressionPct, comp * 100, 0.15 * comp * 100 + 3),
              "compression %.0f %%: measured %.1f %%", comp * 100, r.tel.syncCompressionPct);
        CHECK(near(r.tel.syncDepthPct, 26.25 * (1 - comp), 2.0), "compression %.0f %%: sync depth %.1f %%", comp * 100, r.tel.syncDepthPct);
        CHECK(w < (comp < 0.3 ? 0.08 : comp < 0.5 ? 0.12 : 0.18), "compression %.0f %%: bars %.3f", comp * 100, w);
    }
    // ---- a step in the level (an AGC in the radio, a fade): 5 times down for 1.5 s and back
    {
        AtvGenConfig c = base();
        Options o; o.impair = levelStep(1.5, 3.0, 0.2f, 10e6);
        const Run r = run(c, 4.5, o);
        line("level step x0.2 for 1.5 s", r, barError(r, gp));
        int out = 0;
        for (const auto& s : r.stateLog) if (s.first > 1.0 && s.second != 2) out++;
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < 0.06, "level step: state %d bars %.3f", r.tel.state, barError(r, gp));
        printf("   reports out of lock after the first second: %d of %zu\n", out, r.stateLog.size());
        CHECK(out <= 2, "level step: %d reports out of lock", out);
    }
    // ---- 525 lines with the same trouble: the 59.94 Hz hum, a gap, noise
    {
        AtvGenConfig c = base(kAtvM, kAtvNtsc); c.humPct = 5; c.humHz = 60;
        Options o; o.impair = dropout(1.5, 30, 10e6);
        const Run r = run(c, 3.5, o);
        AtvFormat f; atvMakeFormat(kAtvM, kAtvNtsc, f);
        line("NTSC M: hum 5 % and a gap of 30 ms", r, barError(r, f));
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, f) < 0.08, "NTSC: state %d bars %.3f", r.tel.state, barError(r, f));
    }
    // ---- the controls: the envelope detector only; a forced standard; no colour; bob; saturation
    {
        AtvGenConfig c = base();
        Options o; o.onTime = [](AtvReceiver& rx, double) { rx.setDetector(1); };
        const Run r = run(c, 3.0, o);
        line("envelope detector only", r, barError(r, gp));
        CHECK(r.tel.state == 2 && !r.tel.syncDetector && r.tel.colour, "envelope: state %d sync detector %d colour %d", r.tel.state, (int)r.tel.syncDetector, (int)r.tel.colour);
        CHECK(barError(r, gp) < 0.22, "envelope detector: bars %.3f (a vestigial sideband signal is distorted by an envelope detector)", barError(r, gp));
        const Run s = run(c, 3.0);
        CHECK(s.tel.syncDetector, "the synchronous detector is not used by default");
        // Where the synchronous detector is better: the response of the video band. Everything above the vestigial region (about 1 MHz) is sent in one
        // sideband, an envelope detector gives half of it (-6 dB at 2 MHz), a synchronous detector with the correction for the double-sideband part
        // gives all of it. The colour bars (the middle of the bars is low frequency, where both agree) cannot show that: they are as good with the
        // envelope detector, which also reads a better video S/N because it has less gain at the high end where most of the noise is.
        double aS[6], aE[6];
        AtvCard card(gp);
        multiburstAmps(*s.frame, card, gp, aS);
        multiburstAmps(*r.frame, card, gp, aE);
        const double dbS = 20 * std::log10(aS[2] / aS[0]), dbE = 20 * std::log10(aE[2] / aE[0]);
        printf("   multiburst 2 MHz against 0.5 MHz: synchronous %+.1f dB, envelope %+.1f dB\n", dbS, dbE);
        CHECK(std::fabs(dbS) < 1.5, "synchronous detector: 2 MHz is %+.1f dB against 0.5 MHz", dbS);
        CHECK(dbE < dbS - 3.0, "the synchronous detector is not flatter than the envelope detector (%+.1f dB against %+.1f dB at 2 MHz)", dbS, dbE);
        Options ob; ob.onTime = [](AtvReceiver& rx, double) { rx.setDeinterlace(1); };
        ob.keepFrames = true;
        const Run b = run(c, 2.0, ob);
        CHECK(b.frames.size() > 80, "bob: %zu pictures in 2 s (a picture per field, 100 expected)", b.frames.size());
        Options on; on.onTime = [](AtvReceiver& rx, double) { rx.setColour(false); };
        const Run n = run(c, 2.0, on);
        CHECK(n.frame && !n.frame->colour, "colour off: the picture is not grey");
        Options of; of.onTime = [](AtvReceiver& rx, double) { rx.setStandard(kAtvG, kAtvMono); };
        const Run m = run(c, 2.0, of);
        CHECK(m.tel.colourSystem == "mono" && m.frame && !m.frame->colour, "forced monochrome: '%s'", m.tel.colourSystem.c_str());
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
