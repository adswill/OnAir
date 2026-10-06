// FM receiver: generator (stereo, pilot, RDS, noise, carrier offset) -> receiver, checking audio, stereo separation, RDS and signal quality.
#include "dect2/fm_gen.h"
#include "dect2/fm_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Run {
    FmTelemetry tel;
    std::vector<float> l, r;
};

static Run run(const FmGenConfig& cfg, double secs) {
    Run out;
    FmGenerator gen(cfg);
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(cfg.rate);
    rx.setAudioTap([&](const float* l, const float* r, size_t n) { out.l.insert(out.l.end(), l, l + n); out.r.insert(out.r.end(), r, r + n); });
    std::vector<cf32> buf;
    const size_t block = 65536;
    size_t total = (size_t)(secs * cfg.rate);
    uint64_t seq = 0;
    for (size_t done = 0; done < total; done += block) {
        buf.clear();
        gen.generate(std::min(block, total - done), buf);
        rx.feed(buf.data(), buf.size());
        FmTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; out.tel = t; }
    }
    return out;
}

// amplitude of a tone in the last `secs` seconds of a 48 kHz signal
static double toneAmp(const std::vector<float>& x, double hz, double secs) {
    const size_t n = std::min(x.size(), (size_t)(secs * 48000));
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        const double ph = 2 * M_PI * hz * (double)i / 48000.0;
        const double v = x[x.size() - n + i];
        re += v * std::cos(ph); im += v * std::sin(ph);
    }
    return 2 * std::sqrt(re * re + im * im) / n;
}

static double db(double a, double b) { return 20 * std::log10(std::max(a, 1e-9) / std::max(b, 1e-9)); }

static void checkStation(const Run& r, const char* what, bool rds) {
    const FmTelemetry& t = r.tel;
    printf("%s: state %d SNR %.1f dB level %.1f dBFS cfo %+.0f Hz stereo %d pilot %.1f%% dev %.0f kHz | RDS sync %d ok %.0f%% groups %llu PS '%s' RT '%s' PI %04X %s\n",
           what, t.state, t.snrDb, t.levelDbfs, t.cfoHz, t.stereo, t.pilotPct, t.devKhz, t.rdsSync, t.rdsBlockOkPct, (unsigned long long)t.rdsGroups,
           t.psName.c_str(), t.radioText.c_str(), t.piCode, t.ptyText.c_str());
    CHECK(t.state == 2, "%s: the station is not reported as locked", what);
    CHECK(t.stereo, "%s: stereo not detected", what);
    CHECK(t.pilotPct > 7 && t.pilotPct < 11, "%s: pilot injection %.1f%% (sent 9%%)", what, t.pilotPct);
    const double l1 = toneAmp(r.l, 1000, 3), r3 = toneAmp(r.r, 3000, 3), l3 = toneAmp(r.l, 3000, 3), r1 = toneAmp(r.r, 1000, 3);
    printf("  left 1 kHz %.3f (3 kHz %.4f), right 3 kHz %.3f (1 kHz %.4f), separation %.0f / %.0f dB\n", l1, l3, r3, r1, db(l1, l3), db(r3, r1));
    CHECK(l1 > 0.30 && l1 < 0.50, "%s: left tone amplitude %.3f (expected about 0.4)", what, l1);
    CHECK(r3 > 0.30 && r3 < 0.50, "%s: right tone amplitude %.3f (expected about 0.4)", what, r3);
    CHECK(db(l1, l3) > 30 && db(r3, r1) > 30, "%s: stereo separation only %.0f / %.0f dB", what, db(l1, l3), db(r3, r1));
    if (rds) {
        CHECK(t.rdsSync, "%s: no RDS synchronisation", what);
        CHECK(t.psName == "TESTFM  ", "%s: station name '%s'", what, t.psName.c_str());
        CHECK(t.radioText == "OnAir FM test signal", "%s: radio text '%s'", what, t.radioText.c_str());
        CHECK(t.piCode == 0x4A21, "%s: PI %04X", what, t.piCode);
        CHECK(t.ptyText == "Pop music", "%s: programme type '%s'", what, t.ptyText.c_str());
        CHECK(t.trafficProgram && !t.trafficAlert && t.music, "%s: TP/TA/MS flags", what);
    }
}

int main() {
    {   // a clean stereo station with RDS at 2 Msps
        FmGenConfig c; c.rate = 2e6; c.cnrDb = 45;
        checkStation(run(c, 10), "clean 2 Msps", true);
    }
    {   // an input rate that is not a multiple of 500 kHz, and the carrier 7 kHz off
        FmGenConfig c; c.rate = 2.048e6; c.cnrDb = 40; c.cfoHz = 7000; c.pi = 0x4A21;
        Run r = run(c, 10);
        checkStation(r, "2.048 Msps, 7 kHz off", true);
        CHECK(std::fabs(r.tel.cfoHz - 7000) < 500, "reported carrier offset %.0f Hz (true 7000)", r.tel.cfoHz);
    }
    {   // a high input rate goes through the multi-stage filter
        FmGenConfig c; c.rate = 6e6; c.cnrDb = 40;
        checkStation(run(c, 8), "6 Msps", true);
    }
    {   // a mono station: no pilot, the same signal on both channels
        FmGenConfig c; c.rate = 2e6; c.cnrDb = 45; c.stereo = false; c.rds = false; c.rightAmp = 0.5f; c.rightHz = 1000; c.leftHz = 1000;
        Run r = run(c, 5);
        const double l = toneAmp(r.l, 1000, 2), rr = toneAmp(r.r, 1000, 2);
        printf("mono: state %d stereo %d left %.3f right %.3f\n", r.tel.state, r.tel.stereo, l, rr);
        CHECK(!r.tel.stereo, "a mono station is reported as stereo");
        CHECK(r.tel.state == 2, "mono station not locked");
        CHECK(l > 0.30 && l < 0.50 && std::fabs(l - rr) < 0.02, "mono audio %.3f / %.3f", l, rr);
    }
    {   // signal quality falls with the carrier to noise ratio, and an empty channel is not a station
        double prev = 100;
        for (double cnr : {45.0, 30.0, 20.0, 12.0}) {
            FmGenConfig c; c.rate = 2e6; c.cnrDb = cnr;
            Run r = run(c, 3);
            printf("CNR %.0f dB: state %d SNR %.1f dB\n", cnr, r.tel.state, r.tel.snrDb);
            CHECK(r.tel.snrDb < prev - 3, "SNR %.1f dB at CNR %.0f dB is not below the previous %.1f", r.tel.snrDb, cnr, prev);
            prev = r.tel.snrDb;
        }
        FmGenConfig n; n.rate = 2e6; n.cnrDb = -25;
        Run r = run(n, 3);
        printf("noise only: state %d SNR %.1f dB stereo %d PS '%s'\n", r.tel.state, r.tel.snrDb, r.tel.stereo, r.tel.psName.c_str());
        CHECK(r.tel.state == 0 && !r.tel.stereo && r.tel.psName.empty(), "noise reported as a station");
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
