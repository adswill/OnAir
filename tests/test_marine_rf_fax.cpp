// Weather fax through the radio layer: the USB audio front end feeding the fax decoder. A picture of 60 lines at 240 lpm is sent as radio
// signal and compared with the chart the test source draws.
#include "data/marine/rf_util.h"
#include "data/marine/fax/fax_util.h"
using namespace mt;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void want(const char* what, Scenario s, double minCorr, double maxDrift = 1.5) {
    s.service = 3; s.lines = 60; s.lpm = 240; s.phasing = 15;
    if (s.secs < 40) s.secs = 41;
    Outcome o = run(s);
    const int w = faxImageWidth(s.ioc ? s.ioc : 576);
    const FaxImage ref = faxTestChart(w, s.lines, 5);
    const faxt::Quality q = faxt::compare(o.img, ref, 1);
    printf("%-30s image %dx%d, row correlation %.3f, drift %.2f px, state %d, lines %d, lpm %d, snr %.1f dB, %.0fx real time\n", what, o.img.width, o.img.height, q.meanCorr, q.drift,
           o.tel.fax.state, o.tel.fax.lines, o.tel.fax.lpm, o.tel.fax.snrDb, o.rtf);
    CHECK(o.img.width == w, "%s: width %d, want %d", what, o.img.width, w);
    CHECK(q.ok && q.meanCorr > minCorr, "%s: row correlation %.3f below %.2f", what, q.meanCorr, minCorr);
    CHECK(std::fabs(q.drift) < maxDrift, "%s: drift %.2f px", what, q.drift);
    CHECK(o.tel.serviceActive == 3 && o.tel.state >= 1, "%s: service %d state %d", what, o.tel.serviceActive, o.tel.state);
    if (o.tel.serviceActive != 3) for (const auto& c : o.tel.dsc) printf("  DSC call heard: %s, ECC %s, %s\n", c.vhf ? "VHF" : "MF/HF", c.eccOk ? "ok" : "bad", c.text.c_str());
    CHECK(o.seqOk && o.tel.dataValid, "%s: reports", what);
}

int main() {
    { Scenario s; s.snr = 30; want("250 ksps, 30 dB", s, 0.9); }
    for (double r : {2e6, 2.4e6, 8e6}) { if (kSanitized && r > 2.5e6) continue; Scenario s; s.snr = 30; s.rate = r; char n[40]; snprintf(n, sizeof n, "%.1f Msps", r / 1e6); want(n, s, 0.9); }
    if (!kSanitized) { Scenario s; s.snr = 30; s.chunk = 7; s.secs = 41; want("chunks of 7", s, 0.9); }
    { Scenario s; s.snr = 30; s.rate = 2e6; s.quant8 = true; s.dc = 0.05f; want("8 bit, DC offset, 2 Msps", s, 0.9); }
    for (double m : {-50.0, 50.0}) { Scenario s; s.snr = 30; s.mist = m; char n[40]; snprintf(n, sizeof n, "mistuned %+.0f Hz", m); want(n, s, 0.9); }
    for (double c : {-40.0, 40.0}) { Scenario s; s.snr = 30; s.cfo = c; char n[40]; snprintf(n, sizeof n, "carrier offset %+.0f Hz", c); want(n, s, 0.9); }
    { Scenario s; s.snr = 30; s.sro = 30; want("clock +30 ppm", s, 0.9); }
    { Scenario s; s.snr = 30; s.sro = -30; want("clock -30 ppm", s, 0.9); }
    {   // a gap of 20 ms is 8 % of a line at 240 lpm: the lines after it sit sideways (nothing in the picture says how much), but the picture goes on
        Scenario s; s.snr = 30; s.gapAt = 25; s.gapMs = 20; s.service = 3; s.lines = 60; s.lpm = 240; s.phasing = 15; s.secs = 41;
        Outcome o = run(s);
        printf("%-30s image %dx%d, state %d, lines %d\n", "20 ms gap (sideways after it)", o.img.width, o.img.height, o.tel.fax.state, o.tel.fax.lines);
        CHECK(o.img.width == 1809 && o.tel.fax.lines >= 60 && o.tel.fax.state == 3, "the picture does not go on after a gap");
    }
    { Scenario s; s.snr = 20; want("20 dB", s, 0.8); }
    { Scenario s; s.snr = 10; want("10 dB", s, 0.4); }
    { Scenario s; s.snr = 15; s.fade = 1; want("fading, 15 dB", s, 0.3, 4.0); }
    // the service chosen by the frequency: 12 MHz is none of the NAVTEX or DSC channels
    { Scenario s; s.snr = 30; s.setting = 0; s.freqHz = 12.345e6; want("auto, 12.345 MHz", s, 0.9); }
    if (fails || faxt::fails) return 1;
    printf("ok\n");
    return 0;
}
