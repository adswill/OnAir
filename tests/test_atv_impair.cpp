// Analog TV receiver against what a real radio and a real path do to the signal: noise down to the threshold, carrier offset, sample clock error,
// other sample rates, chunks of any size, 8 bit samples at low level, DC offset and IQ imbalance. The generator makes the signal and draws the
// card the receiver is compared with. Every line printed here is a number that is in the report.
#include "dect2/atv_testkit.h"
#include "jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::atvkit;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)
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
    jprintf("%-34s state %d %-18s colour %d  bars %.3f  video %.1f dB  C/N %.1f dB  line %+.1f ppm  carrier %+.0f Hz  fields bad %llu\n", what, r.tel.state,
           r.tel.system.empty() ? "-" : r.tel.system.c_str(), (int)r.tel.colour, bars, r.tel.snrDb, r.tel.carrierToNoiseDb, r.tel.lineErrPpm, r.tel.cfoHz, (unsigned long long)r.tel.blocksBad);
}

int main() {
    AtvFormat gp; atvMakeFormat(kAtvG, kAtvPal, gp);
    // ---- carrier-to-noise ratio: where the picture, the colour and the lock go
    struct Snr { double cnr; double bars; bool colour; };
    const Snr snrs[] = {{35, 0.06, true}, {30, 0.08, true}, {25, 0.11, true}, {22, 0.15, true}, {20, 0.17, true}, {18, 0.25, true}, {16, 9, false}};
    // the cases are independent: each one runs on its own thread, the output keeps the order of the cases
    testjobs::Jobs jobs;
    for (const Snr& s : snrs) jobs.add([&, s] {
        AtvGenConfig c = base(); c.cnrDb = s.cnr;
        const Run r = run(c, 3.0);
        const double w = barError(r, gp);
        char nm[48]; snprintf(nm, sizeof nm, "C/N %.0f dB", s.cnr);
        line(nm, r, w);
        CHECK(r.tel.state == 2, "C/N %.0f: state %d", s.cnr, r.tel.state);
        CHECK(r.tel.colour == s.colour || !s.colour, "C/N %.0f: colour %d", s.cnr, (int)r.tel.colour);
        CHECK(w < s.bars, "C/N %.0f: bars error %.3f", s.cnr, w);
        CHECK(near(r.tel.carrierToNoiseDb, s.cnr, 1.5), "C/N %.0f: measured %.1f dB", s.cnr, r.tel.carrierToNoiseDb);
        CHECK(near(r.tel.snrDb, s.cnr - 7.2, 2.0) || s.cnr > 33, "C/N %.0f: video SNR %.1f dB (expected about C/N - 7)", s.cnr, r.tel.snrDb);
    });
    // ---- carrier offset: the whole channel found anywhere, the offset measured
    for (double cfo : {-1.5e6, -600e3, -150e3, -12e3, 12e3, 150e3, 600e3, 1.2e6}) jobs.add([&, cfo] {
        AtvGenConfig c = base(); c.cfoHz = cfo;
        const Run r = run(c, 2.5);
        const double w = barError(r, gp);
        char nm[48]; snprintf(nm, sizeof nm, "carrier offset %+.0f kHz", cfo / 1e3);
        line(nm, r, w);
        CHECK(r.tel.state == 2 && r.tel.colour && w < 0.06, "offset %+.0f kHz: state %d colour %d bars %.3f", cfo / 1e3, r.tel.state, (int)r.tel.colour, w);
        CHECK(near(r.tel.cfoHz, cfo, 400), "offset %+.0f kHz: measured %.0f Hz", cfo / 1e3, r.tel.cfoHz);
        const size_t n = r.audio.size();
        CHECK(n > 96000 && near(toneAmp(r.audio, 1000, n - 24000, n), 0.5, 0.05), "offset %+.0f kHz: sound", cfo / 1e3);
    });
    // ---- the offset is against the channel layout; for 5.5 MHz sound the layout (7 or 8 MHz) comes from the user, because the signal cannot tell
    // (an 8 MHz channel with the carrier 500 kHz high is a 7 MHz channel: same spectrum)
    jobs.add([&] {
        AtvGenConfig c = base(kAtvB); c.cfoHz = 100e3;           // PAL B: the carrier is where a 7 MHz channel puts it, plus 100 kHz
        Options o; o.channelWidth = 7;
        const Run r = run(c, 2.5, o);
        line("B, 7 MHz channel, +100 kHz", r, barError(r, gp));
        CHECK(r.tel.state == 2 && near(r.tel.cfoHz, 100e3, 400), "7 MHz channel: state %d offset %.0f Hz", r.tel.state, r.tel.cfoHz);
        const Run u = run(c, 2.5);
        CHECK(near(u.tel.cfoHz, 600e3, 400), "same signal read as an 8 MHz channel: %.0f Hz (the default)", u.tel.cfoHz);
    });
    // ---- sample clock error: the line rate is measured against the standard's, the picture does not care
    for (double ppm : {-100.0, -40.0, 40.0, 100.0}) jobs.add([&, ppm] {
        AtvGenConfig c = base(); c.sroPpm = ppm;
        const Run r = run(c, 3.0);
        const double w = barError(r, gp);
        char nm[48]; snprintf(nm, sizeof nm, "clock error %+.0f ppm", ppm);
        line(nm, r, w);
        CHECK(r.tel.state == 2 && r.tel.colour && w < 0.06, "clock %+.0f ppm: state %d colour %d bars %.3f", ppm, r.tel.state, (int)r.tel.colour, w);
        CHECK(near(r.tel.lineErrPpm, -ppm, 8), "clock %+.0f ppm: line rate error read as %.1f ppm (expected %.0f)", ppm, r.tel.lineErrPpm, -ppm);
        CHECK(near(r.tel.fieldHz, 50.0 * (1 - ppm * 1e-6), 0.01), "clock %+.0f ppm: field rate %.4f", ppm, r.tel.fieldHz);
    });
    // ---- sample rates a radio offers (below 8 Msps the receiver says so and does not run)
    for (double rate : {8e6, 10e6, 12.5e6, 16e6, 20e6}) jobs.add([&, rate] {
        AtvGenConfig c = base(); c.rate = rate;
        const Run r = run(c, 2.5);
        const double w = barError(r, gp);
        char nm[48]; snprintf(nm, sizeof nm, "%.1f Msps", rate / 1e6);
        line(nm, r, w);
        if (rate < 9e6) {
            // the video band ends below the colour subcarrier: the picture is monochrome, it says so, and the grey levels are those of the bars' luminance
            CHECK(r.tel.state == 2 && !r.tel.colour && r.tel.colourSystem == "mono", "%.1f Msps: state %d colour %d '%s'", rate / 1e6, r.tel.state, (int)r.tel.colour, r.tel.colourSystem.c_str());
            double worstY = 9;
            if (r.frame) {
                AtvCard card(gp);
                double col[8][3];
                barColours(*r.frame, card, col);
                worstY = 0;
                for (int i = 0; i < 8; i++) {
                    float rgb[3]; atvEbuBar(i, rgb);
                    const double y = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
                    for (int k = 0; k < 3; k++) worstY = std::max(worstY, std::fabs(col[i][k] - y));
                }
            }
            jprintf("   luma error of the bars (grey picture) %.3f\n", worstY);
            CHECK(worstY < 0.08, "%.1f Msps: luma error %.3f", rate / 1e6, worstY);
        }
        else CHECK(r.tel.state == 2 && r.tel.colour && w < 0.06, "%.1f Msps: state %d colour %d bars %.3f", rate / 1e6, r.tel.state, (int)r.tel.colour, w);
    });
    for (double rate : {2e6, 4e6, 6e6}) jobs.add([&, rate] {
        AtvReceiver rx;
        rx.configure(rate);
        CHECK(!rx.ready(), "%.1f Msps is too low and the receiver must say so", rate / 1e6);
        std::vector<cf32> x(65536, cf32(0.1f, 0.f));
        rx.feed(x.data(), x.size());                      // must not crash or block
    });
    // ---- chunks of any size
    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)333, (size_t)4096, (size_t)65536}) jobs.add([&, chunk] {
        AtvGenConfig c = base();
        Options o; o.chunk = chunk;
        const Run r = run(c, chunk == 1 ? 0.8 : 2.0, o);
        const double w = barError(r, gp);
        char nm[48]; snprintf(nm, sizeof nm, "chunks of %zu", chunk);
        line(nm, r, w);
        CHECK(r.tel.state == 2 && r.tel.system == "PAL B/G 625/50", "chunk %zu: state %d '%s'", chunk, r.tel.state, r.tel.system.c_str());
        if (chunk > 1) CHECK(r.tel.colour && w < 0.06, "chunk %zu: colour %d bars %.3f", chunk, (int)r.tel.colour, w);
    });
    // ---- level and 8 bit samples: the radio's -30 to -10 dBFS means 5 to 30 % of full scale for the signal; the receiver has its own gain control
    {
        const double lv[] = {0.5, 0.2, 0.1, 0.05};
        for (double sc : lv) jobs.add([&, sc] {
            AtvGenConfig c = base();
            Options o; o.scale = sc;
            const Run r = run(c, 3.0, o);
            const double w = barError(r, gp);
            char nm[48]; snprintf(nm, sizeof nm, "level %.0f %% of nominal", sc * 100);
            line(nm, r, w);
            CHECK(r.tel.state == 2, "level %.2f: state %d", sc, r.tel.state);
            if (sc >= 0.2) CHECK(r.tel.colour && w < (sc >= 0.5 ? 0.06 : 0.09), "level %.2f: colour %d bars %.3f", sc, (int)r.tel.colour, w);
        });
        jobs.add([&] {
        AtvGenConfig c = base();
        Options o; o.quantise = false;
        const Run r = run(c, 2.5, o);
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < 0.05, "without 8 bit quantisation: state %d bars %.3f", r.tel.state, barError(r, gp));
        });
    }
    // ---- DC offset (the spike at the centre of a HackRF), IQ imbalance
    jobs.add([&] {
        AtvGenConfig c = base();
        Options o; o.impair = dcOffset(0.06f, -0.04f);
        const Run r = run(c, 2.5, o);
        line("DC offset 0.06 / -0.04", r, barError(r, gp));
        CHECK(r.tel.state == 2 && r.tel.colour && barError(r, gp) < 0.06, "DC offset: state %d bars %.3f", r.tel.state, barError(r, gp));
        Options o2; o2.impair = iqImbalance(1.15, 6);
        const Run q = run(c, 2.5, o2);
        line("IQ imbalance 1.2 dB, 6 deg", q, barError(q, gp));
        CHECK(q.tel.state == 2 && q.tel.colour && barError(q, gp) < 0.07, "IQ imbalance: state %d bars %.3f", q.tel.state, barError(q, gp));
        Options o3; o3.impair = chain({dcOffset(0.05f, 0.05f), iqImbalance(0.9, -8)});
        o3.scale = 0.3;
        const Run p = run(c, 2.5, o3);
        line("DC + IQ + level 30 %", p, barError(p, gp));
        CHECK(p.tel.state == 2 && p.tel.colour && barError(p, gp) < 0.09, "DC, IQ and level: state %d bars %.3f", p.tel.state, barError(p, gp));
    });
    // ---- the standards at a modest carrier-to-noise ratio: the system is still found
    struct Sys { int sys, col; const char* name; };
    for (const Sys& s : {Sys{kAtvB, kAtvPal, "PAL B/G 625/50"}, Sys{kAtvI, kAtvPal, "PAL I 625/50"}, Sys{kAtvDK, kAtvPal, "PAL D/K 625/50"}, Sys{kAtvM, kAtvNtsc, "NTSC M 525/59.94"},
                         Sys{kAtvM, kAtvPal, "PAL-M 525/59.94"}, Sys{kAtvN, kAtvPal, "PAL-N 625/50"}, Sys{kAtvG, kAtvSecam, "SECAM B/G 625/50"}, Sys{kAtvDK, kAtvSecam, "SECAM D/K 625/50"}}) jobs.add([&, s] {
        AtvGenConfig c = base(s.sys, s.col); c.cnrDb = 24;
        const Run r = run(c, 3.0);
        AtvFormat f; atvMakeFormat(s.sys, s.col, f);
        const double w = barError(r, f);
        line(s.name, r, w);
        CHECK(r.tel.state == 2 && r.tel.system == s.name && r.tel.colour && w < 0.16, "%s at C/N 24: state %d '%s' colour %d bars %.3f", s.name, r.tel.state, r.tel.system.c_str(), (int)r.tel.colour, w);
    });
    jobs.run();
    if (fails) { jprintf("%d check(s) failed\n", fails.load()); return 1; }
    jprintf("OK\n");
    return 0;
}
