// HD Radio against the faults of REAL_WORLD_CHECKLIST.md, applied with tests/impair.h (written independently of the receiver) to the generator's
// clean signal: 50 ppm tuning errors (+-5.4 kHz in band II; AM: a few hundred Hz), +-100 ppm sample clocks, swapped I/Q, NaN samples, a USB
// drop, and one combined case per band (worst offset, +80 ppm, an echo and 8-bit clipping).
#include "dect2/hdr_gen.h"
#include "dect2/hdr_rx.h"
#include "impair.h"
#include "jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kRate = 1e6;

static std::vector<cf32> clean(int mode, double secs, double analogDb = 0) {
    SynthConfig sc;
    sc.mode = 23; sc.modeOpt[0] = mode; sc.snrDb = 30; sc.modeVal[0] = analogDb;
    auto syn = makeHdrSynth(sc, kRate);
    std::vector<cf32> x((size_t)(secs * kRate));
    syn->generate(x.data(), x.size());
    return x;
}

static void check(const char* what, int mode, const std::vector<cf32>& x, double cfo = std::numeric_limits<double>::quiet_NaN(), unsigned maxBad = 3) {
    HdrReceiver rx;
    rx.configure(kRate);
    if (getenv("HDR_LOG")) rx.setLogCallback([](const std::string& m) { fprintf(stderr, "LOG %s\n", m.c_str()); });
    for (size_t i = 0; i < x.size(); i += 20000) rx.feed(x.data() + i, std::min<size_t>(20000, x.size() - i));
    HdrTelemetry t;
    rx.telemetry(t, 0);
    jprintf("%-44s state %d band %d, CFO %+7.1f Hz, P1 %llu ok %llu bad, call sign '%s'\n", what, t.state, t.band, t.cfoHz, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
            t.callSign.c_str());
    CHECK(t.state == 3 && t.band == (mode == 1 ? 2 : 1) && t.blocksOk >= (mode == 1 ? 10u : 5u) && t.blocksBad <= maxBad && !t.callSign.empty(), "%s", what);
    if (!std::isnan(cfo)) CHECK(std::fabs(t.cfoHz - cfo) < 20, "%s: CFO %.1f, sent %.1f", what, t.cfoHz, cfo);
}

// keeps only lo..hi Hz (a complex band-pass FIR, 401 taps): what is left when a neighbour's filter or an interferer takes one sideband
static std::vector<cf32> band(const std::vector<cf32>& x, double lo, double hi) {
    const int H = 200;
    const double fc = (hi - lo) / 2 / kRate, f0 = (hi + lo) / 2 / kRate;
    std::vector<cf32> h(2 * H + 1);
    for (int k = -H; k <= H; k++) {
        const double sinc = k == 0 ? 2 * fc : std::sin(2 * M_PI * fc * k) / (M_PI * k);
        const double w = 0.42 + 0.5 * std::cos(M_PI * k / H) + 0.08 * std::cos(2 * M_PI * k / H);
        h[(size_t)(k + H)] = (float)(sinc * w) * std::polar(1.f, (float)(2 * M_PI * f0 * k));
    }
    std::vector<cf32> y(x.size());
    for (size_t n = (size_t)H; n + (size_t)H < x.size(); n++) {
        cf32 a = 0;
        for (int k = -H; k <= H; k++) a += h[(size_t)(k + H)] * x[n - (size_t)k];
        y[n] = a;
    }
    return y;
}

int main() {
    testjobs::Jobs jobs;
    const double fmSecs = 12, amSecs = 16;
    for (double hz : {5400.0, -5400.0}) jobs.add([=] {
        auto x = clean(0, fmSecs); impair::shift(x, hz, kRate);
        char b[64]; snprintf(b, sizeof b, "FM, tuning error %+.0f Hz", hz);
        check(b, 0, x, hz);
    });
    for (double hz : {400.0, -400.0}) jobs.add([=] {
        auto x = clean(1, amSecs); impair::shift(x, hz, kRate);
        char b[64]; snprintf(b, sizeof b, "AM, tuning error %+.0f Hz", hz);
        check(b, 1, x, hz);
    });
    for (int m : {0, 1}) for (double ppm : {-100.0, 100.0}) jobs.add([=] {
        auto x = impair::clock(clean(m, m ? amSecs : fmSecs), ppm); impair::shift(x, 150, kRate);
        char b[64]; snprintf(b, sizeof b, "%s, sample clock %+.0f ppm", m ? "AM" : "FM", ppm);
        check(b, m, x);
    });
    for (int m : {0, 1}) jobs.add([=] {
        auto x = clean(m, 30); impair::shift(x, 230, kRate); impair::swapIq(x);   // the other side is tried after 12 s without a P1 frame
        check(m ? "AM, swapped I/Q" : "FM, swapped I/Q", m, x, std::numeric_limits<double>::quiet_NaN(), 12);   // frames before the switch fail
    });
    // analog host louder or quieter than nominal (digital sidebands at -30 and -10 dBc), AM host +6 dB
    for (double db : {10.0, -10.0}) jobs.add([=] {
        auto x = clean(0, fmSecs, db); impair::shift(x, 900, kRate);
        char b[64]; snprintf(b, sizeof b, "FM, analog %+.0f dB against the digital part", db);
        check(b, 0, x);
    });
    jobs.add([=] { auto x = clean(1, amSecs, 6); check("AM, analog +6 dB", 1, x); });
    // one digital sideband gone (a neighbour on the other side, or its filter), and one buried under a strong neighbour
    jobs.add([=] { auto x = band(clean(0, fmSecs), -230000, 100000); check("FM, upper sideband missing", 0, x); });
    jobs.add([=] { auto x = band(clean(0, fmSecs), -100000, 230000); check("FM, lower sideband missing", 0, x); });
    for (double jamDb : {20.0, 30.0}) jobs.add([=] {   // a neighbour (first-adjacent FM) 20 or 30 dB above the upper digital sideband, on top of it
        auto x = clean(0, fmSecs);
        std::vector<cf32> z(x.size());
        std::mt19937 rng(9);
        std::normal_distribution<float> g(0.f, 1.f);
        for (auto& v : z) v = cf32(g(rng), g(rng));
        z = band(z, 125000, 205000);
        const auto up = band(x, 129000, 199000);
        double pj = 0, ps = 0;
        for (size_t i = 0; i < x.size(); i++) { pj += std::norm(z[i]); ps += std::norm(up[i]); }
        const float k = (float)std::sqrt(std::pow(10.0, jamDb / 10) * ps / pj);
        for (size_t i = 0; i < x.size(); i++) x[i] += k * z[i];
        char b[64]; snprintf(b, sizeof b, "FM, upper sideband under a neighbour +%.0f dB", jamDb);
        check(b, 0, x);
    });
    jobs.add([=] {
        auto x = clean(0, fmSecs);
        for (size_t i = 0; i < 4000; i++) x[(size_t)(3 * kRate) + i] = cf32(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
        check("FM, NaN and infinite samples", 0, x);
    });
    jobs.add([=] { auto x = clean(0, 20); impair::skip(x, 77777); impair::drop(x, (size_t)(4 * kRate), 31415); check("FM, start mid-frame, samples lost", 0, x); });
    jobs.add([=] {
        auto x = impair::clock(clean(0, fmSecs), 80); impair::shift(x, 5400, kRate); impair::echo(x, 3, -6, 2.0); impair::clip8(x, 2.0);
        check("FM combined: +5.4 kHz, +80 ppm, echo, 8 bit", 0, x, 5400);
    });
    jobs.add([=] {
        auto x = impair::clock(clean(1, amSecs), 80); impair::shift(x, 400, kRate); impair::echo(x, 3, -10, 2.0); impair::clip8(x, 2.0);
        check("AM combined: +400 Hz, +80 ppm, echo, 8 bit", 1, x, 400);
    });
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
