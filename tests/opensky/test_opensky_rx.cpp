// OpenSky identifier: the measurements on generated signals with the published shape of OpenSky (9600 baud, four levels, 4-slot TDMA in 25 kHz) and on other channel shapes it must not call OpenSky.
// No real OpenSky signal was analysed: the air interface is not public.
#include "dect2/opensky/opensky_rx.h"
#include "dect2/opensky/opensky_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<cf32> fsk(double baud, int levels, double devHz, double dur, double rate, double slotMs, int slots, int active, double noise, double cfo, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)noise);
    const size_t N = (size_t)(dur * rate);
    std::vector<cf32> x(N);
    const double sps = rate / baud;
    double ph = 0, smooth = 0, sym = 0;
    const double al = 1 - std::exp(-2 * M_PI * baud * 0.8 / rate);
    const size_t slotN = (size_t)(slotMs * 1e-3 * rate);
    size_t nextSym = 0;
    for (size_t i = 0; i < N; i++) {
        if (i >= nextSym) {
            nextSym = (size_t)((double)(i / sps + 1) * sps);
            const int k = (int)(rng() % (unsigned)levels);
            sym = levels == 2 ? (k ? 1.0 : -1.0) : (2.0 * k - 3.0) / 3.0;
        }
        smooth += al * (sym - smooth);
        ph += 2 * M_PI * (smooth * devHz + cfo) / rate;
        if (ph > 2 * M_PI) ph -= 2 * M_PI; if (ph < -2 * M_PI) ph += 2 * M_PI;
        bool on = true;
        if (slots) { const size_t si = (i / slotN) % (size_t)slots; on = (int)si < active; }
        const float a = on ? 0.4f : 0.f;
        x[i] = cf32(a * (float)std::cos(ph), a * (float)std::sin(ph)) + cf32(g(rng), g(rng));
    }
    return x;
}

static OpenSkyTelemetry run(const std::vector<cf32>& x, double rate) {
    OpenSkyReceiver rx;
    rx.configure(rate);
    for (size_t i = 0; i < x.size(); i += 4096) rx.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    OpenSkyTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

int main() {
    const double r = 240000;
    {
        auto t = run(fsk(9600, 4, 4800, 4, r, 7.5, 4, 2, 0.01, 0, 1), r);
        printf("OpenSky shape: %.0f baud (line %.1f dB), %d levels, dev %.0f, burst %.1f ms, duty %.2f, width %.1f kHz, %s\n", t.baud, t.lineDb, t.levels, t.deviationHz, t.burstPeriodMs, t.burstDuty, t.occupiedHz / 1e3, t.verdict.c_str());
        CHECK(t.looksLikeOpenSky, "OpenSky shape not recognised");
        CHECK(t.burstPeriodMs > 0, "burst period not found");
    }
    { auto t = run(fsk(9600, 4, 4800, 4, r, 7.5, 4, 2, 0.03, 900, 2), r); CHECK(t.looksLikeOpenSky, "with offset and noise"); printf("noise + offset: %.0f baud, %d levels\n", t.baud, t.levels); }
    { auto t = run(fsk(4800, 4, 1944, 4, r, 0, 0, 0, 0.01, 0, 3), r); printf("P25-like: %.0f baud, %d levels, %s\n", t.baud, t.levels, t.looksLikeOpenSky ? "OpenSky" : "not"); CHECK(!t.looksLikeOpenSky, "P25-like shape called OpenSky"); }
    { auto t = run(fsk(9600, 2, 2500, 4, r, 0, 0, 0, 0.01, 0, 4), r); printf("EDACS-like: %.0f baud, %d levels, %s\n", t.baud, t.levels, t.looksLikeOpenSky ? "OpenSky" : "not"); CHECK(!t.looksLikeOpenSky, "two levels called OpenSky"); }
    { auto t = run(fsk(3600, 2, 1200, 4, r, 0, 0, 0, 0.01, 0, 5), r); CHECK(!t.looksLikeOpenSky, "3600 baud called OpenSky"); }
    {
        std::mt19937 rng(9); std::normal_distribution<float> g(0.f, 0.05f);
        std::vector<cf32> x((size_t)(r * 4));
        for (auto& v : x) v = cf32(g(rng), g(rng));
        auto t = run(x, r);
        CHECK(!t.looksLikeOpenSky, "noise called OpenSky");
    }
    return testSummary("opensky_rx");
}
