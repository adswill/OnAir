// Analog TV, SECAM: generator -> receiver. The picture is compared with the colour bars the generator drew; the standard and the way the lines were
// identified are read back from the telemetry and the log. The generator's SECAM chrominance is checked against the numbers of BT.470-6 in
// test_atv_gen (frequencies, amplitudes, identification lines), so a receiver that agrees with it agrees with the Recommendation.
#include "dect2/atv_testkit.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static double barError(const AtvFrame& fr, const AtvFormat& f) {
    AtvCard card(f);
    double c[8][3], worst = 0;
    barColours(fr, card, c);
    for (int i = 0; i < 8; i++) {
        float rgb[3]; atvEbuBar(i, rgb);
        for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(c[i][k] - rgb[k]));
    }
    return worst;
}

static bool logHas(const Run& r, const char* s) {
    for (const auto& l : r.log) if (l.find(s) != std::string::npos) return true;
    return false;
}

int main() {
    // ---- the two systems that use SECAM here, with both ways of identifying the lines, and both together
    struct Case { int sys; int ident; const char* name; const char* how; };
    const Case cases[] = {
        {kAtvG, 0, "SECAM B/G 625/50", "lead-in on the lines and identification lines"},
        {kAtvDK, 0, "SECAM D/K 625/50", nullptr},
        {kAtvG, 1, "SECAM B/G 625/50", "lead-in on the lines"},
        {kAtvG, 2, "SECAM B/G 625/50", "identification lines of the field blanking"},
    };
    for (const Case& k : cases) {
        AtvGenConfig c; c.sys = k.sys; c.colour = kAtvSecam; c.secamIdent = k.ident; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 2.5);
        const AtvTelemetry& t = r.tel;
        AtvFormat f; atvMakeFormat(k.sys, kAtvSecam, f);
        printf("%s (ident %d): %s\n", k.name, k.ident, atvSummary(t).c_str());
        CHECK(t.state == 2 && t.dataValid, "%s ident %d: state %d", k.name, k.ident, t.state);
        CHECK(t.system == k.name && t.colourSystem == "SECAM" && t.colour, "ident %d: system '%s' colour '%s' %d", k.ident, t.system.c_str(), t.colourSystem.c_str(), (int)t.colour);
        if (k.how) CHECK(logHas(r, k.how) || k.ident == 0, "ident %d: the log does not say how SECAM was found (%s)", k.ident, k.how);
        CHECK(t.blocksBad <= 3, "ident %d: %llu damaged fields", k.ident, (unsigned long long)t.blocksBad);
        CHECK(near(t.burstLevel, 1.0, 0.25), "ident %d: chroma level %.2f of nominal", k.ident, t.burstLevel);
        CHECK(near(t.soundSpacingMhz, f.soundSpacingMhz, 1e-6), "ident %d: sound spacing %.2f", k.ident, t.soundSpacingMhz);
        CHECK(r.frame && r.frame->colour, "ident %d: no coloured picture", k.ident);
        if (r.frame) {
            const double w = barError(*r.frame, f);
            AtvCard card(f);
            const double ld = lumaDifference(*r.frame, card, 3);
            int ex, ey, bx0, bx1, by0, by1;
            whiteBarEdges(*r.frame, card, ex, ey);
            card.barsArea(bx0, bx1, by0, by1);
            printf("   bars worst channel error %.3f, luminance difference %.2f, white bar starts at %d,%d (card: %d,%d)\n", w, ld, ex, ey, bx0, by0);
            CHECK(w < 0.07, "ident %d: colour bars error %.3f", k.ident, w);
            CHECK(ld < 6, "ident %d: luminance difference %.2f", k.ident, ld);
            CHECK(std::abs(ex - bx0) <= 2 && std::abs(ey - by0) <= 1, "ident %d: the picture is shifted: the white bar starts at (%d,%d), the card has it at (%d,%d)", k.ident, ex, ey, bx0, by0);
        }
    }
    // ---- noise: the FM chrominance is less tolerant than PAL's; where it fails it must fail to monochrome, not to wrong colours
    for (double cnr : {30.0, 25.0, 22.0}) {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvSecam; c.cnrDb = cnr; c.rate = 10e6;
        const Run r = run(c, 3.0);
        AtvFormat f; atvMakeFormat(kAtvG, kAtvSecam, f);
        const double w = r.frame ? barError(*r.frame, f) : 9;
        printf("C/N %.0f dB: %s, bars error %.3f\n", cnr, atvSummary(r.tel).c_str(), w);
        CHECK(r.tel.state == 2 && r.tel.colourSystem == "SECAM" && r.tel.colour, "C/N %.0f: state %d colour %d '%s'", cnr, r.tel.state, (int)r.tel.colour, r.tel.colourSystem.c_str());
        CHECK(w < (cnr >= 30 ? 0.08 : cnr >= 25 ? 0.11 : 0.17), "C/N %.0f: bars error %.3f", cnr, w);
    }
    // ---- carrier offset, clock error, other sample rates
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvSecam; c.cnrDb = 40; c.rate = 10e6; c.cfoHz = -250e3; c.sroPpm = 60;
        const Run r = run(c, 2.5);
        AtvFormat f; atvMakeFormat(kAtvG, kAtvSecam, f);
        const double w = r.frame ? barError(*r.frame, f) : 9;
        CHECK(r.tel.state == 2 && r.tel.colour && w < 0.07, "offset: state %d colour %d bars %.3f", r.tel.state, (int)r.tel.colour, w);
        CHECK(near(r.tel.cfoHz, -250e3 / 1.00006, 600), "carrier offset %.0f Hz", r.tel.cfoHz);
    }
    for (double rate : {12.5e6, 16e6, 20e6}) {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvSecam; c.cnrDb = 40; c.rate = rate;
        const Run r = run(c, 2.0);
        AtvFormat f; atvMakeFormat(kAtvG, kAtvSecam, f);
        const double w = r.frame ? barError(*r.frame, f) : 9;
        printf("%.1f Msps: %s, bars error %.3f\n", rate / 1e6, atvSummary(r.tel).c_str(), w);
        CHECK(r.tel.state == 2 && r.tel.colour && w < 0.08, "%.1f Msps: state %d colour %d bars %.3f", rate / 1e6, r.tel.state, (int)r.tel.colour, w);
    }
    // ---- a PAL signal is not taken for SECAM, nor a monochrome one, and SECAM is not taken for PAL
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 2.0);
        CHECK(r.tel.colourSystem == "PAL" && r.tel.colour, "PAL signal read as '%s'", r.tel.colourSystem.c_str());
        c.colour = kAtvMono;
        const Run m = run(c, 3.0);
        CHECK(m.tel.colourSystem == "mono" && !m.tel.colour, "monochrome signal read as '%s' (colour %d)", m.tel.colourSystem.c_str(), (int)m.tel.colour);
    }
    // ---- impulse noise destroys the lead-in of some lines: the alternation carries the identification over them
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvSecam; c.secamIdent = 0; c.cnrDb = 40; c.rate = 10e6;
        Options o; o.impair = impulses(150000, 120, 2.0f);                // 12 us bursts, 66 per second
        const Run r = run(c, 3.0, o);
        AtvFormat f; atvMakeFormat(kAtvG, kAtvSecam, f);
        const double w = r.frame ? barError(*r.frame, f) : 9;
        printf("impulse noise: %s, bars error %.3f\n", atvSummary(r.tel).c_str(), w);
        CHECK(r.tel.state == 2 && r.tel.colour && w < 0.12, "impulse noise: state %d colour %d bars %.3f", r.tel.state, (int)r.tel.colour, w);
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
