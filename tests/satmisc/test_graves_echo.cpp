// GRAVES satellite echoes through the meteor scatter receiver: the beacon and a meteor ping from the mode's generator, and a simulated satellite echo whose
// Doppler follows the bistatic curve of a known orbit. Simulated only: no real GRAVES recording goes through this test.
#include "dect2/mscatter/mscatter_gen.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/mscatter/mscatter_tel.h"
#include "dect2/satmisc/sat_id.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static Tle makeTle(int norad, double incl, double raan, double m, double nm) {
    Tle t; t.name = "SAT " + std::to_string(norad); t.norad = norad; t.epochJd = 2461000.5; t.bstar = 1e-5; t.inclDeg = incl; t.raanDeg = raan; t.ecc = 0.001; t.argpDeg = 90; t.meanAnomDeg = m; t.meanMotion = nm;
    return t;
}
struct Run { int echoes, named; std::string name; };
static Run run(const std::vector<Tle>& cat, const Sgp4* target, double t0, const Observer& rx, double snrDb, double beaconOff, double echoLen) {
    const double rate = 1e6; const int block = 8192;
    MscatterGenConfig g; g.rate = rate; g.snrDb = 45; g.carrierHz = beaconOff; g.meanGapSec = 4; g.firstSec = 3;
    MscatterGenerator gen(g);
    MscatterLiveReceiver rxr; rxr.configure(rate); rxr.setCenterHz(143.05e6);
    rxr.setStartUtc((int64_t)t0);
    rxr.setSatelliteMatch(cat, rx.latDeg, rx.lonDeg, rx.altKm);
    satmisc::SatIdOptions o; o.refHz = 143.05e6; o.bistatic = true; o.tx = Observer{47.348, 5.515, 0.2};
    std::vector<cf32> x((size_t)block);
    double phase = 0;
    const double total = echoLen + 12;
    const double amp = 0.3 * std::pow(10.0, (snrDb - 45) / 20);   // the beacon's SNR is 45 dB in 100 Hz at 0.3
    for (double t = 0; t < total; t += block / rate) {
        gen.generate(x.data(), (size_t)block);
        if (target && t > 5 && t < 5 + echoLen)
            for (int i = 0; i < block; i++) {
                const double ts = t0 + t + i / rate;
                static thread_local double dop = 0;
                if (i % 512 == 0) { double hh = 0, ee = 0; if (satmisc::predictDopplerHz(*target, rx, ts, o, hh, ee)) dop = hh; }
                phase += 2 * M_PI * (beaconOff + dop) / rate;
                x[(size_t)i] += cf32((float)(amp * std::cos(phase)), (float)(amp * std::sin(phase)));
            }
        rxr.feed(x.data(), (size_t)block);
    }
    MsTelemetry t; rxr.telemetry(t, 0);
    Run r{(int)t.satEchoTotal, (int)t.satEchoMatched, t.satEchoes.empty() ? "" : t.satEchoes[0].name};
    printf("  beacon state %d, pings %llu, echoes %llu, matched %llu, first: %s\n", t.state, (unsigned long long)t.blocksOk, (unsigned long long)t.satEchoTotal, (unsigned long long)t.satEchoMatched, r.name.c_str());
    return r;
}
int main() {
    const Observer rx{54.7, 25.3, 0.1};
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(0, 360), inc(50, 100), nm(13.5, 15.6);
    std::vector<Tle> cat;
    for (int i = 0; i < 800; i++) cat.push_back(makeTle(20000 + i, inc(rng), u(rng), u(rng), nm(rng)));
    const double start = jdToUnix(2461000.5) + 3600;
    int done = 0, good = 0;
    for (int i = 3; i < 800 && done < 3; i += 29) {
        Sgp4 s; if (!s.init(cat[(size_t)i])) continue;
        auto p = findPasses(s, rx, start, start + 86400, 25);
        const Pass* use = nullptr;
        satmisc::SatIdOptions o; o.refHz = 143.05e6; o.bistatic = true; o.tx = Observer{47.348, 5.515, 0.2};
        for (const auto& x : p) { double h, e; if (satmisc::predictDopplerHz(s, rx, x.tca, o, h, e) && std::fabs(h) > 200 && std::fabs(h) < 5000) { use = &x; break; } }
        if (!use) continue;
        done++;
        const double off = 300.0 * done;
        Run r = run(cat, &s, use->tca - 40, rx, 22 + 4 * done, off, 25);
        CHECK(r.echoes >= 1, "no echo at sat %d", cat[(size_t)i].norad);
        if (r.name == cat[(size_t)i].name) good++; else printf("  expected %s got %s\n", cat[(size_t)i].name.c_str(), r.name.c_str());
    }
    CHECK(done == 3 && good >= 2, "%d of %d", good, done);
    Run q = run(cat, nullptr, start, rx, 30, 500, 15);
    CHECK(q.named == 0, "a matched echo without a satellite in the signal");
    Run e = run({}, nullptr, start, rx, 30, 500, 15);
    CHECK(e.echoes == 0, "%d echoes from beacon and pings alone", e.echoes);
    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
