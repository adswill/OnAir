// NAVTEX through the radio layer: sample rates, chunk sizes, 8 bit samples, DC, a gap, a reset, carrier offsets, mistuning, fading.
#include "data/marine/rf_util.h"
using namespace mt;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void want(const char* what, Scenario s, int minExact = 1, double maxCer = 0.0) {
    Outcome o = run(s);
    const NavScore sc = scoreNavtex(o.tel);
    printf("%-34s %d messages, %d exact, %u/%u edits, snr %.1f dB, centre %.1f Hz, %.0fx real time\n", what, sc.msgs, sc.exact, sc.dist, sc.chars, o.tel.snrDb, o.tel.cfoHz, o.rtf);
    CHECK(sc.exact >= minExact || (maxCer > 0 && sc.msgs >= 1 && (double)sc.dist / sc.chars <= maxCer), "%s: %d exact of %d messages", what, sc.exact, sc.msgs);
    CHECK(o.seqOk && o.reports >= (uint64_t)(s.secs * 3), "%s: %llu reports", what, (unsigned long long)o.reports);
    CHECK(o.tel.serviceActive == 1 && o.tel.state >= 1, "%s: service %d state %d", what, o.tel.serviceActive, o.tel.state);
}

int main() {
    Scenario base; base.secs = 20; base.idle = 2;
    { Scenario s = base; want("250 ksps", s); }
    for (double r : {2e6, 2.4e6, 8e6, 10e6}) { if (kSanitized && r > 2.5e6) continue; Scenario s = base; s.rate = r; char n[40]; snprintf(n, sizeof n, "%.1f Msps", r / 1e6); want(n, s); }
    for (size_t c : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) { if (kSanitized && c < 100) continue; Scenario s = base; s.chunk = c; if (c < 100) { s.secs = 19; s.idle = 1; } char n[40]; snprintf(n, sizeof n, "chunks of %zu", c); want(n, s); }
    { Scenario s = base; s.rate = 2e6; s.quant8 = true; s.snr = 20; want("8 bit samples, 2 Msps", s); }
    { Scenario s = base; s.dc = 0.05f; s.quant8 = true; want("DC offset 0.05, 8 bit", s); }
    { Scenario s = base; s.gapAt = 6; s.gapMs = 20; want("20 ms gap", s, 1, 0.25); }
    { Scenario s = base; s.resetAt = 5; s.secs = 40; want("reset mid stream", s); }
    // carrier offsets: +-10 ppm of 518 kHz is 5 Hz, of 4209.5 kHz 42 Hz; a mistuned receiver +-50 Hz and more
    for (double f : {-45.0, 45.0, -50.0, 50.0, 150.0, -300.0}) { Scenario s = base; s.cfo = f; char n[40]; snprintf(n, sizeof n, "carrier offset %+.0f Hz", f); want(n, s); }
    { Scenario s = base; s.mist = 50; s.sro = 50; want("mistuned +50 Hz, clock +50 ppm", s); }
    { Scenario s = base; s.sro = -50; want("clock -50 ppm", s); }
    { Scenario s = base; s.snr = 15; s.fade = 1; s.secs = 45; want("two-path fading, 15 dB", s, 1, 0.03); }
    // the service chosen by the frequency
    { Scenario s = base; s.setting = 0; s.freqHz = 518e3; want("auto, 518 kHz", s); }
    {
        Scenario s = base; s.setting = 3; Outcome o = run(s);
        printf("fax selected on a NAVTEX signal: %zu messages\n", o.tel.navtex.size());
        CHECK(o.tel.navtex.empty(), "NAVTEX decoded while the service is fax");
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
