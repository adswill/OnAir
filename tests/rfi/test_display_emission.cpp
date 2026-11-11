// Local display / HDMI emission: the timing table is self-consistent; wide spectra with the harmonics of each common pixel clock (with clock
// offsets) find the right clock; narrow captures of one harmonic tell the timings that share a clock apart by the line-rate comb, over offsets
// and a noise sweep; a lone carrier, a switching-supply comb and noise are not called a display; the signal identifier lists it.
// Simulated signals only (the generator in display_emission.cpp): no real recording of a display was available.
#include "dect2/rfi/display_emission.h"
#ifndef ONAIR_TEST_STANDALONE
#include "dect2/engine/sig_ident.h"
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dect2;
using namespace dect2::dispem;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* nameOf(const Result& r) { return r.matches.empty() || r.matches[0].timing < 0 ? "-" : timings()[(size_t)r.matches[0].timing].name; }
static bool listed(const Result& r, const char* name) {
    for (const Match& m : r.matches) if (m.timing >= 0 && std::string(timings()[(size_t)m.timing].name) == name) return true;
    return false;
}

int main() {
    const bool longRun = getenv("ONAIR_TEST_LONG") != nullptr;
    {   // the table: refresh = clock / (htotal * vtotal), twice that for interlaced fields
        for (const Timing& t : timings()) {
            const double r = t.pclkHz / ((double)t.htotal * t.vtotal) * (t.interlaced ? 2 : 1);
            CHECK(std::fabs(r / t.refreshHz - 1) < 0.003, "%s: %.3f Hz from the totals, %.3f listed", t.name, r, t.refreshHz);
            CHECK(t.htotal > t.width && t.vtotal > t.height, "%s: totals below the active size", t.name);
        }
        CHECK(findTiming("1920x1080 60 Hz") >= 0 && findTiming("nope") < 0, "findTiming");
    }
    {   // wide spectra (a sweep, 100 kHz bins, 20 MHz - 2 GHz): the pixel clock from its harmonics
        const char* kWide[] = {"640x480 60 Hz", "1280x720 60 Hz", "1920x1080 60 Hz", "3840x2160 30 Hz", "3840x2160 60 Hz", "1024x768 60 Hz", "1280x1024 60 Hz",
                               "1920x1080 60 Hz (reduced blanking)", "1920x1080 59.94 Hz", "2560x1440 60 Hz (reduced blanking)"};
        const double kPpm[] = {-180, 0, 250};
        const double f0 = 20e6, bin = 100e3;
        const int n = 19800;
        int seed = 1;
        for (const char* nm : kWide) for (double ppm : kPpm) {
            const int ti = findTiming(nm);
            const std::vector<float> db = simulateSpectrum(ti, f0, bin, n, ppm, -100, 30, (uint64_t)seed++);
            const Result r = analyseSpectrum(db.data(), n, f0, bin);
            const double want = timings()[(size_t)ti].pclkHz * (1 + ppm * 1e-6);
            CHECK(r.found && r.tableMatch && std::fabs(r.pclkHz / want - 1) < 30e-6, "wide %s %+.0f ppm: found %d clock %.4f MHz (want %.4f), %s", nm, ppm, r.found, r.pclkHz / 1e6, want / 1e6, r.summary.c_str());
            // same clock: the right timing is among the matches (the line rate cannot be seen at 100 kHz)
            CHECK(listed(r, nm), "wide %s: not among the matches (best %s)", nm, nameOf(r));
            CHECK(r.confidence > 0.7, "wide %s: confidence %.2f", nm, r.confidence);
        }
        // 1080p60 and 1080p59.94 are 1000 ppm apart: told apart
        const std::vector<float> db = simulateSpectrum(findTiming("1920x1080 59.94 Hz"), f0, bin, n, 0, -100, 30, 77);
        const Result r = analyseSpectrum(db.data(), n, f0, bin);
        CHECK(r.matches.size() && std::string(nameOf(r)).find("59.94") != std::string::npos, "59.94 Hz taken for %s", nameOf(r));
    }
    {   // narrow captures (2 Msps at one harmonic): the line-rate comb picks the timing among those sharing the clock
        struct C { const char* name; int harmonic; };
        const C kN[] = {{"1920x1080 60 Hz", 1}, {"1920x1080 50 Hz", 1}, {"1280x720 60 Hz", 2}, {"1280x720 50 Hz", 3}, {"1920x1080 24 Hz", 2},
                        {"3840x2160 30 Hz", 1}, {"1024x768 60 Hz", 3}, {"1280x1024 60 Hz", 2}};
        const double kPpm[] = {-120, 0, 90};
        const float kLevel[] = {45, 36};   // clock line over the noise (per 64 k bin)
        int seed = 100;
        for (const C& c : kN) for (double ppm : kPpm) for (float lv : kLevel) {
            if (!longRun && lv < 40 && ppm != 0) continue;   // ONAIR_TEST_LONG=1: every offset at the weaker level too
            GenConfig g;
            g.timing = findTiming(c.name);
            const double p = timings()[(size_t)g.timing].pclkHz;
            g.rfCentreHz = c.harmonic * p + 230e3;   // not tuned exactly on the line
            g.ppm = ppm; g.levelDb = lv; g.seconds = 0.3; g.seed = (uint64_t)seed++;
            const std::vector<cf32> x = generateIq(g);
            const Result r = analyseIq(x.data(), x.size(), g.rateHz, g.rfCentreHz);
            CHECK(r.found && std::string(nameOf(r)) == c.name, "narrow %s h%d %+.0f ppm %.0f dB: found %d best %s (%s)", c.name, c.harmonic, ppm, lv, r.found, nameOf(r), r.summary.c_str());
            if (!r.matches.empty()) CHECK(r.matches[0].combLines >= 6, "narrow %s: %d comb lines", c.name, r.matches[0].combLines);
            CHECK(r.confidence > (lv < 40 ? 0.65 : 0.75), "narrow %s %.0f dB: confidence %.2f", c.name, lv, r.confidence);
        }
        // the label and the text
        GenConfig g; g.timing = findTiming("1920x1080 60 Hz"); g.rfCentreHz = 148.6e6; g.seconds = 0.3;
        const std::vector<cf32> x = generateIq(g);
        const Result r = analyseIq(x.data(), x.size(), g.rateHz, g.rfCentreHz);
        const std::string t = toText(r);
        CHECK(t.find("not your SDR") != std::string::npos && t.find("1920x1080 60 Hz") != std::string::npos && t.find("2200 pixels per line") != std::string::npos, "text:\n%s", t.c_str());
        CHECK(r.matches.size() >= 2 && listed(r, "1920x1080 50 Hz"), "alternatives not listed (%zu)", r.matches.size());
        printf("%s", t.c_str());
    }
    {   // not a display
        // a lone carrier at a clock frequency: one line is not enough
        std::vector<float> db(20000, -100.f);
        db[(size_t)((148.5e6 - 140e6) / 1e3)] = -50;
        Result r = analyseSpectrum(db.data(), 20000, 140e6, 1e3);
        CHECK(!r.found, "a lone 148.5 MHz carrier called a display (%.2f)", r.confidence);
        // a carrier at 150 MHz (no clock), with a 25 kHz FM-like comb around it
        for (float& v : db) v = -100;
        for (int m = -6; m <= 6; m++) db[(size_t)((150e6 + m * 25e3 - 140e6) / 1e3)] = (float)(-50 - std::abs(m) * 2);
        r = analyseSpectrum(db.data(), 20000, 140e6, 1e3);
        CHECK(!r.found, "a 150 MHz comb called a display: %s", r.summary.c_str());
        // a switching supply: 100 kHz comb from 1 to 30 MHz
        std::vector<float> w(30000, -100.f);
        for (int k = 10; k < 300; k++) w[(size_t)(k * 100)] = -60;
        r = analyseSpectrum(w.data(), 30000, 0, 1e3);
        CHECK(!r.found, "a switching-supply comb called a display: %s", r.summary.c_str());
        // noise only
        GenConfig g; g.timing = 0; g.levelDb = -100; g.combDb = -100;
        const std::vector<cf32> x = generateIq(g);
        r = analyseIq(x.data(), x.size(), g.rateHz, 148.5e6);
        CHECK(!r.found, "noise called a display (%.2f)", r.confidence);
    }
#ifndef ONAIR_TEST_STANDALONE
    {   // the signal identifier lists it
        GenConfig g; g.timing = findTiming("1920x1080 60 Hz"); g.rfCentreHz = 148.7e6; g.seconds = 0.3; g.levelDb = 45;
        const std::vector<cf32> x = generateIq(g);
        sigid::Options o; o.rfHz = g.rfCentreHz; o.loHz = 148.5e6 - g.rfCentreHz - 20e3; o.hiHz = 148.5e6 - g.rfCentreHz + 20e3;
        const sigid::Result r = sigid::identify(x.data(), x.size(), g.rateHz, o);
        bool seen = false;
        for (size_t i = 0; i < r.cands.size(); i++) if (r.cands[i].name == label()) { seen = true; CHECK(i == 0, "identifier: display emission ranked %zu", i + 1); CHECK(r.cands[i].why.find("1920x1080 60 Hz") != std::string::npos, "identifier why '%s'", r.cands[i].why.c_str()); }
        CHECK(seen, "identifier: no display emission candidate (%zu candidates, headline %s)", r.cands.size(), r.headline.c_str());
    }
#endif
    printf(fails ? "display_emission: %d FAILED\n" : "display_emission: all passed\n", fails);
    return fails ? 1 : 0;
}
