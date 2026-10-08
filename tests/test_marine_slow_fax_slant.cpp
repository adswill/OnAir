// Weather fax through the radio layer: a picture of 800 lines (200 s at 240 lpm) with a sample clock 30 ppm off. The slant of the picture
// (shift of the rows from the first to the last 20 % of the picture) with and without the automatic correction.
#include "data/marine/rf_util.h"
#include "data/marine/fax/fax_util.h"
using namespace mt;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static faxt::Quality one(const char* what, double ppm, bool autoSlant) {
    Scenario s; s.service = 3; s.lines = 800; s.lpm = 240; s.phasing = 15; s.snr = 30; s.sro = ppm; s.faxAuto = autoSlant;
    s.secs = 5 + 15 + 801 * 0.25 + 5 + 2;
    Outcome o = run(s);
    const FaxImage ref = faxTestChart(faxImageWidth(576), 800, 5);
    const faxt::Quality q = faxt::compare(o.img, ref, 1);
    printf("%-34s image %dx%d, row correlation %.3f, drift first to last rows %.2f px, slant in use %.1f ppm, %.0fx real time\n", what, o.img.width, o.img.height, q.meanCorr, q.drift,
           o.tel.fax.slantPpm, o.rtf);
    return q;
}

int main() {
    const faxt::Quality a = one("clock +30 ppm, automatic", 30, true);
    CHECK(a.ok && std::fabs(a.drift) < 1.0 && a.meanCorr > 0.85, "automatic slant, +30 ppm: drift %.2f px", a.drift);
    const faxt::Quality b = one("clock -30 ppm, automatic", -30, true);
    CHECK(b.ok && std::fabs(b.drift) < 1.0 && b.meanCorr > 0.85, "automatic slant, -30 ppm: drift %.2f px", b.drift);
    const faxt::Quality c = one("clock +30 ppm, no correction", 30, false);
    printf("without the correction the drift would be %.1f px (30 ppm of 800 lines of 1809 px is about 43 px)\n", c.drift);
    CHECK(c.ok && std::fabs(c.drift) > 20.0, "the uncorrected slant should show: %.2f px", c.drift);
    if (fails || faxt::fails) return 1;
    printf("ok\n");
    return 0;
}
