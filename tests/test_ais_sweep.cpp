// AIS: how many bursts decode at a given signal to noise ratio, with random class A messages on both channels, 2 Msps.
// SNR is the burst power against the noise in 48 kHz (a 25 kHz channel sees 2.8 dB more).
// argv: [snr list in dB, comma separated] [bursts per point] [rate]
#include "dect2/ais_testutil.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace dect2;
using namespace dect2::aistest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Point { double snr; int sent, got, bad; };
static const double kMax90 = 3.5;   // measured 2.5 dB; the limit leaves room for another machine's rounding

static Point run(double snrDb, int nBursts, double rate, double cfo, uint32_t seed, double sro = 0) {
    std::mt19937 rng(seed);
    std::vector<AisBurstSpec> specs;
    std::set<std::vector<uint8_t>> sent;
    const double gap = 0.09;
    for (int i = 0; i < nBursts; i++) {
        AisBurstSpec s;
        s.payload = randomPosition(rng);
        s.channel = (i & 1) ? 'B' : 'A';
        s.startSec = 0.3 + gap * i;
        s.cfoHz = cfo;
        s.phase = (double)(rng() % 1000) / 1000.0 * 6.28;
        sent.insert(s.payload);
        specs.push_back(s);
    }
    AisRenderConfig rc;
    rc.rate = rate; rc.durationSec = 0.3 + gap * nBursts + 0.3; rc.snrDb = snrDb; rc.seed = seed; rc.sroPpm = sro;
    const std::vector<cf32> x = renderAisBursts(specs, rc);
    AisReceiver rx;
    {   // bench: AIS_W (integration width), AIS_PASS and AIS_STOP (channel filter), AIS_HYP, AIS_REFINE, AIS_MODE
        AisPhyConfig pc;
        if (const char* e = getenv("AIS_W")) pc.intWidth = atof(e);
        if (const char* e = getenv("AIS_PASS")) pc.chanPassHz = atof(e);
        if (const char* e = getenv("AIS_STOP")) pc.chanStopHz = atof(e);
        if (const char* e = getenv("AIS_HYP")) pc.hypotheses = atoi(e);
        if (const char* e = getenv("AIS_REFINE")) pc.refine = atoi(e);
        if (const char* e = getenv("AIS_MODE")) pc.mode = atoi(e);
        if (const char* e = getenv("AIS_MLSE")) pc.mlse = atoi(e) != 0;
        if (const char* e = getenv("AIS_NPASS")) pc.narrowPassHz = atof(e);
        if (const char* e = getenv("AIS_NSTOP")) pc.narrowStopHz = atof(e);
        rx.setPhyConfig(pc);
    }
    rx.configure(rate);
    Collector col;
    const AisTelemetry t = runReceiver(rx, x, 16384, &col);
    Point p{snrDb, nBursts, 0, (int)t.blocksBad};
    std::set<std::vector<uint8_t>> seen;
    for (const auto& g : col.got) if (sent.count(g.bits) && seen.insert(g.bits).second) p.got++;
    CHECK(col.got.size() == (size_t)(p.got) || col.got.size() >= (size_t)p.got, "collector");
    return p;
}

int main(int argc, char** argv) {
    std::vector<double> snrs = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 20, 30};
    if (argc > 1) { snrs.clear(); for (char* s = strtok(argv[1], ","); s; s = strtok(nullptr, ",")) snrs.push_back(atof(s)); }
    const int n = argc > 2 ? atoi(argv[2]) : 100;
    const double rate = argc > 3 ? atof(argv[3]) : 2e6;
    std::vector<Point> pts;
    for (double snr : snrs) {
        const Point p = run(snr, n, rate, 0, 7);
        pts.push_back(p);
        printf("snr %5.1f dB (%.1f dB in 25 kHz): %3d of %3d decoded (%.0f %%), %d bad\n", snr, snr + 2.8, p.got, p.sent, 100.0 * p.got / p.sent, p.bad);
    }
    if (argc > 1) return 0;           // a bench run with chosen points
    // where 90 % of the bursts decode (linear between the points around it)
    double at90 = -99;
    for (size_t i = 1; i < pts.size(); i++) {
        const double a = (double)pts[i - 1].got / pts[i - 1].sent, b = (double)pts[i].got / pts[i].sent;
        if (a < 0.9 && b >= 0.9) at90 = pts[i - 1].snr + (0.9 - a) / (b - a) * (pts[i].snr - pts[i - 1].snr);
    }
    printf("90 %% of the bursts decode at %.1f dB in 48 kHz (%.1f dB in a 25 kHz channel, %.1f dB Eb/N0)\n", at90, at90 + 2.8, at90 + 10 * std::log10(48000.0 / 9600.0));
    CHECK(at90 > -99 && at90 < kMax90, "90 %% point at %.1f dB, expected below %.1f dB", at90, kMax90);
    for (const Point& p : pts) {
        if (p.snr >= 6) CHECK(p.got == p.sent, "%d of %d at %.0f dB", p.got, p.sent, p.snr);
        else if (p.snr >= 4) CHECK(p.got * 100 >= p.sent * 97, "%d of %d at %.0f dB", p.got, p.sent, p.snr);
    }
    return fails ? 1 : 0;
}
