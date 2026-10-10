// FM receiver against real-world faults (tests/impair.h, REAL_WORLD_CHECKLIST.md): the generator's signal is damaged independently of the receiver.
#include "dect2/fm_gen.h"
#include "dect2/fm_rx.h"
#include "impair.h"
#include <cstdio>
#include <functional>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// widthHz: the receiver's channel filter (FmReceiver::setChannelWidth; 0 = the standard one)
static FmTelemetry run(const char* what, const FmGenConfig& c, double secs, const std::function<void(std::vector<cf32>&)>& fault, double widthHz = 0) {
    FmGenerator g(c);
    std::vector<cf32> x;
    g.generate((size_t)(secs * c.rate), x);
    if (fault) fault(x);
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setChannelWidth(widthHz);
    FmTelemetry t;
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 65536) {
        rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        FmTelemetry u;
        if (rx.telemetry(u, seq)) { seq = u.seq; t = u; }
    }
    printf("%-34s state %d cfo %+.0f Hz stereo %d pilot %.1f%% | RDS sync %d ok %.0f%% groups %llu PS '%s' RT '%s' PI %04X\n", what, t.state, t.cfoHz, t.stereo,
           t.pilotPct, t.rdsSync, t.rdsBlockOkPct, (unsigned long long)t.rdsGroups, t.psName.c_str(), t.radioText.c_str(), t.piCode);
    return t;
}

// RDS sends 11.4 groups a second; secs: the length of the run (the first 1.5 s are allowed for locking)
static void rdsGood(const FmTelemetry& t, const char* what, float minOk, double secs) {
    CHECK(t.rdsSync && t.rdsBlockOkPct >= minOk, "%s: RDS blocks %.0f%% good (wanted %.0f%%)", what, t.rdsBlockOkPct, minOk);
    const double want = (secs - 1.5) * 1187.5 / 104 * 0.9;
    CHECK((double)t.rdsGroups >= want, "%s: %llu RDS groups (wanted %.0f)", what, (unsigned long long)t.rdsGroups, want);
    CHECK(t.psName == "TESTFM  " && t.radioText == "OnAir FM test signal" && t.piCode == 0x4A21, "%s: RDS contents", what);
}

int main() {
    FmGenConfig base;
    base.rate = 2e6;

    {   // one NaN sample in a file must not stop the receiver for good
        FmTelemetry t = run("NaN sample", base, 6, [](std::vector<cf32>& x) { x[1000000] = cf32(NAN, NAN); x[1000001] = cf32(INFINITY, 0); });
        CHECK(std::isfinite(t.cfoHz) && std::fabs(t.cfoHz) < 200, "NaN sample: carrier offset %f", t.cfoHz);
        CHECK(t.stereo && t.pilotPct > 7, "NaN sample: stereo lost");
        rdsGood(t, "NaN sample", 90, 6);
    }
    // the radio's sample clock off by +-100 ppm: the RDS bit clock (pilot / 16) then drifts against the receiver's 16 samples per bit
    for (double ppm : {-100.0, 100.0}) {
        char w[64]; snprintf(w, sizeof w, "sample clock %+.0f ppm", ppm);
        FmTelemetry t = run(w, base, 16, [ppm](std::vector<cf32>& x) { x = impair::clock(x, ppm); });
        rdsGood(t, w, 90, 8);
    }
    {   // a mono station with RDS (no pilot): RDS runs on its own 57 kHz carrier recovery
        FmGenConfig c = base; c.stereo = false;
        FmTelemetry t = run("mono with RDS", c, 8, nullptr);
        CHECK(!t.stereo, "mono with RDS: stereo reported");
        rdsGood(t, "mono with RDS", 90, 8);
    }
    // the radio's DC spike on a station tuned exactly (1.2 kHz of tuning error), 6 dB below the station
    {
        FmTelemetry t = run("DC spike -6 dB on the carrier", base, 8, [](std::vector<cf32>& x) { impair::shift(x, 1200, 2e6); impair::dc(x, -6); });
        CHECK(t.state == 2 && t.stereo, "DC spike: state %d stereo %d", t.state, t.stereo);
        rdsGood(t, "DC spike", 90, 8);
    }
    // a recording made beside the station (gqrx, SDR#): the station 420 kHz above the centre, a weaker one (-15 dB) 650 kHz below it
    for (double at : {420e3, -560e3}) {
        char w[64]; snprintf(w, sizeof w, "station %+.0f kHz off the centre", at / 1e3);
        FmTelemetry t = run(w, base, 8, [at](std::vector<cf32>& x) {
            FmGenConfig o; o.rate = 2e6; o.ps = "OTHER   "; o.pi = 0x1234; o.rt = "other"; o.seed = 7;
            FmGenerator g(o);
            std::vector<cf32> y;
            g.generate(x.size(), y);
            impair::shift(x, at, 2e6);
            impair::shift(y, at > 0 ? -650e3 : 650e3, 2e6);
            for (size_t i = 0; i < x.size(); i++) x[i] += y[i] * 0.178f;
        });
        CHECK(t.state == 2 && t.stereo, "%s: state %d stereo %d", w, t.state, t.stereo);
        CHECK(std::fabs(t.cfoHz - at) < 2000, "%s: offset read as %+.0f Hz", w, t.cfoHz);
        rdsGood(t, w, 90, 8);
    }
    // the station tuned to stays chosen beside a neighbour 20 dB stronger, 400 kHz away
    {
        FmTelemetry t = run("weak station, strong neighbour", base, 8, [](std::vector<cf32>& x) {
            FmGenConfig o; o.rate = 2e6; o.ps = "OTHER   "; o.pi = 0x1234; o.rt = "other"; o.seed = 7;
            FmGenerator g(o);
            std::vector<cf32> y;
            g.generate(x.size(), y);
            impair::shift(y, 400e3, 2e6);
            for (size_t i = 0; i < x.size(); i++) x[i] = x[i] * 0.1f + y[i];
        });
        CHECK(std::fabs(t.cfoHz) < 2000, "strong neighbour: moved to %+.0f Hz", t.cfoHz);
        rdsGood(t, "weak station, strong neighbour", 90, 8);
    }
    // a neighbour 150 kHz up and 10 dB stronger: with the standard 220 kHz filter its lower half takes over the discriminator; a manual
    // 150 kHz channel width (FmReceiver::setChannelWidth) keeps it out and the station plays
    for (double width : {0.0, 150e3}) {
        char w[64]; snprintf(w, sizeof w, "neighbour +150 kHz +10 dB, %s", width > 0 ? "150 kHz" : "standard");
        FmTelemetry t = run(w, base, 8, [](std::vector<cf32>& x) {
            FmGenConfig o; o.rate = 2e6; o.ps = "OTHER   "; o.pi = 0x1234; o.rt = "other"; o.seed = 7; o.leftHz = 3100; o.rightHz = 4700;
            FmGenerator g(o);
            std::vector<cf32> y;
            g.generate(x.size(), y);
            impair::shift(y, 150e3, 2e6);
            for (size_t i = 0; i < x.size(); i++) x[i] += y[i] * 3.162f;
        }, width);
        printf("%-34s SNR %.1f dB\n", "", t.snrDb);
        if (width > 0) CHECK(t.state == 2 && t.stereo && t.snrDb > 30, "%s: state %d stereo %d SNR %.1f dB", w, t.state, t.stereo, t.snrDb);
    }
    // multipath: an echo 10 us late, 6 dB down (a reflection off hills): the constant-modulus equaliser takes it out
    {
        FmTelemetry t = run("echo 10 us -6 dB", base, 8, [](std::vector<cf32>& x) { impair::echo(x, 20, -6, 1.0); });
        CHECK(t.state == 2 && t.stereo, "echo: state %d stereo %d", t.state, t.stereo);
        rdsGood(t, "echo 10 us -6 dB", 90, 8);
    }
    // combined: the worst tuning error of the band (50 ppm of 108 MHz), the sample clock +80 ppm, an echo of 3 us at -8 dB and 8-bit clipping;
    // stereo and mono (whose RDS carrier is then 4.6 Hz off the receiver's 57 kHz)
    for (bool stereo : {true, false}) {
        FmGenConfig c = base; c.stereo = stereo;
        const char* w = stereo ? "combined, stereo" : "combined, mono";
        FmTelemetry t = run(w, c, 10, [](std::vector<cf32>& x) {
            impair::shift(x, 5400, 2e6); x = impair::clock(x, 80); impair::echo(x, 6, -8, 2.0); impair::clip8(x, 3); });
        CHECK(t.state == 2 && std::fabs(t.cfoHz - 5400) < 300, "%s: state %d, offset %+.0f Hz", w, t.state, t.cfoHz);
        CHECK(t.stereo == stereo, "%s: stereo %d", w, t.stereo);
        rdsGood(t, w, 90, 10);
    }
    return fails ? 1 : 0;
}
