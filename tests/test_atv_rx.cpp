// Analog TV receiver: generator -> receiver on clean and simulated signals. The picture is compared with the card the generator drew, the sound
// with the tone it sent, the timing with the standard.
#include "dect2/atv_testkit.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// the colour bars of the decoded picture against the EBU bars
static double barError(const Run& r, const AtvFormat& f, bool colour, double* worstOut = nullptr) {
    AtvCard card(f);
    double c[8][3];
    barColours(*r.frame, card, c);
    double worst = 0;
    for (int i = 0; i < 8; i++) {
        float rgb[3]; atvEbuBar(i, rgb);
        double e[3] = {c[i][0] - rgb[0], c[i][1] - rgb[1], c[i][2] - rgb[2]};
        if (!colour) {                                    // monochrome: compare the luminance
            float y, u, v; atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, v);
            const double yy = 0.299 * c[i][0] + 0.587 * c[i][1] + 0.114 * c[i][2];
            e[0] = e[1] = e[2] = yy - y;
        }
        for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(e[k]));
    }
    if (worstOut) *worstOut = worst;
    return worst;
}

int main() {
    // ---- B/G PAL at 10 Msps, clean
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 45; c.rate = 10e6;
        const Run r = run(c, 3.0);
        const AtvTelemetry& t = r.tel;
        for (const auto& l : r.log) printf("  log: %s\n", l.c_str());
        printf("%s\n", atvSummary(t).c_str());
        CHECK(t.state == 2 && t.dataValid, "locked: state %d", t.state);
        CHECK(t.system == "PAL B/G 625/50", "system '%s'", t.system.c_str());
        CHECK(t.colourSystem == "PAL" && t.colour && t.lines == 625, "colour '%s' %d lines %d", t.colourSystem.c_str(), (int)t.colour, t.lines);
        CHECK(r.lockedAfterMs >= 0 && r.lockedAfterMs < 1000, "locked after %d ms", r.lockedAfterMs);
        CHECK(t.fieldCount > 100 && t.frameCount > 50, "fields %llu pictures %llu", (unsigned long long)t.fieldCount, (unsigned long long)t.frameCount);
        CHECK(t.blocksBad <= 2, "damaged fields %llu", (unsigned long long)t.blocksBad);
        CHECK(near(t.lineErrPpm, 0, 6), "line rate %.1f ppm off", t.lineErrPpm);
        CHECK(near(t.fieldHz, 50.0, 0.005), "field rate %.4f", t.fieldHz);
        CHECK(near(t.visionHz, -2.75e6, 300) && near(t.cfoHz, 0, 300), "vision carrier %.1f Hz (cfo %.1f)", t.visionHz, t.cfoHz);
        CHECK(near(t.soundSpacingMhz, 5.5, 1e-9) && near(t.soundHz, 2.75e6, 1500), "sound carrier %.0f Hz, spacing %.2f", t.soundHz, t.soundSpacingMhz);
        CHECK(near(t.syncDepthPct, 26.25, 1.0), "sync depth %.2f %% (expected 26.25)", t.syncDepthPct);
        CHECK(near(t.burstLevel, 1.0, 0.12), "burst %.2f of nominal", t.burstLevel);
        CHECK(t.syncDetector, "the synchronous detector is not in use");
        CHECK(t.snrDb > 30, "video SNR %.1f dB", t.snrDb);
        CHECK(t.lineWave.size() >= 400 && t.vbiWave.size() >= 400, "scope data %zu %zu", t.lineWave.size(), t.vbiWave.size());
        // the picture
        AtvFormat f; atvMakeFormat(kAtvG, kAtvPal, f);
        CHECK(r.frame && r.frame->width == 768 && r.frame->height == 576 && r.frame->colour, "picture size/colour");
        if (r.frame) {
            double worst;
            barError(r, f, true, &worst);
            AtvCard card(f);
            const double ld = lumaDifference(*r.frame, card, 3);
            int ex, ey, bx0, bx1, by0, by1;
            whiteBarEdges(*r.frame, card, ex, ey);
            card.barsArea(bx0, bx1, by0, by1);
            printf("picture: bars worst channel error %.3f, luminance difference %.2f, white bar starts at %d,%d (card %d,%d)\n", worst, ld, ex, ey, bx0, by0);
            CHECK(worst < 0.09, "colour bars: worst channel error %.3f", worst);
            CHECK(ld < 4.5, "luminance difference %.2f", ld);
            CHECK(std::abs(ex - bx0) <= 2 && std::abs(ey - by0) <= 1, "the picture is shifted: the white bar starts at (%d,%d), the card has it at (%d,%d)", ex, ey, bx0, by0);
        }
        // the sound: 1 kHz at half of 50 kHz deviation, played at half scale, with a gap of 0.2 s every 1.5 s
        const size_t n = r.audio.size();
        CHECK(n > 48000 * 2, "sound: %zu samples", n);
        if (n > 48000 * 2) {
            const double hz = peakTone(r.audio, 24000, 24000 + 12000);
            const double amp = toneAmp(r.audio, 1000, 24000, 24000 + 12000);
            printf("sound: tone %.1f Hz amplitude %.3f deviation reported %.1f kHz\n", hz, amp, t.soundDevKhz);
            CHECK(near(hz, 1000, 3), "tone at %.1f Hz", hz);
            CHECK(near(amp, 0.5, 0.06), "tone amplitude %.3f (sent 0.5 of full scale)", amp);
        }
    }
    // ---- the other systems
    struct Sys { int sys, col; const char* name; int lines; double spacing; double vision; };
    const Sys systems[] = {
        {kAtvB, kAtvPal, "PAL B/G 625/50", 625, 5.5, -2.25e6},
        {kAtvI, kAtvPal, "PAL I 625/50", 625, 6.0, -2.75e6},
        {kAtvDK, kAtvPal, "PAL D/K 625/50", 625, 6.5, -2.75e6},
        {kAtvM, kAtvNtsc, "NTSC M 525/59.94", 525, 4.5, -1.75e6},
        {kAtvM, kAtvPal, "PAL-M 525/59.94", 525, 4.5, -1.75e6},
        {kAtvN, kAtvPal, "PAL-N 625/50", 625, 4.5, -1.75e6},
    };
    for (const Sys& s : systems) {
        AtvGenConfig c; c.sys = s.sys; c.colour = s.col; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 2.5);
        const AtvTelemetry& t = r.tel;
        AtvFormat f; atvMakeFormat(s.sys, s.col, f);
        printf("%s\n", atvSummary(t).c_str());
        CHECK(t.state == 2, "%s: state %d", s.name, t.state);
        CHECK(t.system == s.name, "%s: system '%s'", s.name, t.system.c_str());
        CHECK(t.colour, "%s: colour not decoded", s.name);
        CHECK(near(t.soundSpacingMhz, s.spacing, 1e-6), "%s: sound spacing %.4f", s.name, t.soundSpacingMhz);
        CHECK(near(t.visionHz, s.vision, 400), "%s: vision carrier %.0f Hz", s.name, t.visionHz);
        CHECK(near(t.lineErrPpm, 0, 8) && t.blocksBad <= 3, "%s: line error %.1f ppm, %llu bad fields", s.name, t.lineErrPpm, (unsigned long long)t.blocksBad);
        CHECK(t.soundPresent && t.soundDevKhz > 0.4 * f.soundDevKhz, "%s: sound deviation %.1f kHz", s.name, t.soundDevKhz);
        if (r.frame) {
            double worst;
            barError(r, f, true, &worst);
            AtvCard card(f);
            int ex, ey, bx0, bx1, by0, by1;
            whiteBarEdges(*r.frame, card, ex, ey);
            card.barsArea(bx0, bx1, by0, by1);
            printf("   bars worst channel error %.3f, white bar starts at %d,%d (card %d,%d)\n", worst, ex, ey, bx0, by0);
            CHECK(worst < 0.10, "%s: colour bars worst channel error %.3f", s.name, worst);
            CHECK(std::abs(ex - bx0) <= 2 && std::abs(ey - by0) <= 1, "%s: the picture is shifted: the white bar starts at (%d,%d), the card has it at (%d,%d)", s.name, ex, ey, bx0, by0);
        } else CHECK(false, "%s: no picture", s.name);
    }
    // ---- a signal without colour
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvMono; c.cnrDb = 40; c.rate = 10e6;
        const Run r = run(c, 2.0);
        AtvFormat f; atvMakeFormat(kAtvG, kAtvMono, f);
        CHECK(r.tel.state == 2 && !r.tel.colour && r.tel.colourSystem == "mono", "monochrome: state %d colour %d '%s'", r.tel.state, (int)r.tel.colour, r.tel.colourSystem.c_str());
        if (r.frame) {
            double worst;
            barError(r, f, false, &worst);
            // the bars of a monochrome signal carry only luminance: the picture must be grey
            double c3[8][3];
            AtvCard card(f);
            barColours(*r.frame, card, c3);
            double dev = 0;
            for (int i = 0; i < 8; i++) dev = std::max(dev, std::fabs(c3[i][0] - c3[i][2]));
            printf("monochrome: bars luminance error %.3f, colour cast %.3f\n", worst, dev);
            CHECK(worst < 0.08 && dev < 0.02, "monochrome picture: luminance error %.3f cast %.3f", worst, dev);
        }
    }
    // ---- 8 Msps: the rate the receiver needs at least; the colour subcarrier does not fit, the picture is monochrome
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 40; c.rate = 8e6;
        const Run r = run(c, 2.0);
        printf("%s\n", atvSummary(r.tel).c_str());
        CHECK(r.tel.state == 2 && !r.tel.colour, "8 Msps: state %d colour %d", r.tel.state, (int)r.tel.colour);
    }
    // ---- noise only: no picture, nothing found
    {
        AtvReceiver rx;
        rx.setSilent(true);
        rx.configure(10e6);
        std::mt19937 rng(5);
        std::normal_distribution<float> g(0.f, 0.08f);
        std::vector<cf32> x(65536);
        for (int k = 0; k < 20; k++) {
            for (auto& s : x) s = cf32(g(rng), g(rng));
            rx.feed(x.data(), x.size());
        }
        AtvTelemetry t;
        CHECK(rx.telemetry(t, 0), "telemetry from noise");
        uint64_t fs = 0;
        CHECK(t.state == 0 && !t.dataValid && rx.frame(fs) == nullptr, "noise gives state %d", t.state);
        // a carrier without anything on it is not a television signal either
        for (int k = 0; k < 20; k++) {
            for (size_t i = 0; i < x.size(); i++) x[i] = cf32(g(rng), g(rng)) + 0.2f * cf32((float)std::cos(2 * M_PI * 1.0e6 * ((double)k * x.size() + i) / 10e6), (float)std::sin(2 * M_PI * 1.0e6 * ((double)k * x.size() + i) / 10e6));
            rx.feed(x.data(), x.size());
        }
        rx.telemetry(t, 0);
        CHECK(t.state == 0 && rx.frame(fs) == nullptr, "a bare carrier gives state %d", t.state);
    }
    // ---- reset in the middle: the telemetry numbering goes on, the signal is found again
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 40; c.rate = 10e6;
        AtvGenerator g(c);
        AtvReceiver rx;
        rx.setSilent(true);
        rx.configure(10e6);
        std::vector<cf32> buf(65536);
        uint64_t seq = 0, last = 0;
        bool locked1 = false, locked2 = false, down = false;
        for (int k = 0; k < 90; k++) {
            g.generate(buf.data(), buf.size());
            for (auto& s : buf) s = cf32(std::round(s.real() * 128) / 128, std::round(s.imag() * 128) / 128);
            if (k == 40) { rx.reset(); AtvTelemetry t; rx.telemetry(t, 0); down = t.state == 0; }
            rx.feed(buf.data(), buf.size());
            AtvTelemetry t;
            if (rx.telemetry(t, seq)) {
                CHECK(t.seq > last, "telemetry numbering went back: %llu after %llu", (unsigned long long)t.seq, (unsigned long long)last);
                last = t.seq; seq = t.seq;
                if (k < 40 && t.state == 2) locked1 = true;
                if (k > 60 && t.state == 2) locked2 = true;
            }
        }
        CHECK(locked1 && down && locked2, "reset: locked before %d, down after reset %d, locked again %d", (int)locked1, (int)down, (int)locked2);
    }
    // ---- chunk sizes
    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096}) {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 40; c.rate = 10e6;
        Options o; o.chunk = chunk;
        const double secs = chunk == 1 ? 0.7 : 1.5;
        const Run r = run(c, secs, o);
        AtvFormat f; atvMakeFormat(kAtvG, kAtvPal, f);
        printf("chunks of %zu: %s\n", chunk, atvSummary(r.tel).c_str());
        CHECK(r.tel.state == 2 && r.tel.system == "PAL B/G 625/50", "chunk %zu: state %d '%s'", chunk, r.tel.state, r.tel.system.c_str());
        if (r.frame && chunk != 1) { double w; barError(r, f, true, &w); CHECK(w < 0.09, "chunk %zu: bars error %.3f", chunk, w); }
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
