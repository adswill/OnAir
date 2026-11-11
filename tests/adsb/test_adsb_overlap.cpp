// ADS-B overlapping replies (3.9, docs/ADSB-REMOTEID-3.9.md): the receiver with its overlap recovery (AdsbDemod step 6: subtraction of decoded
// frames, retries of the candidates under them, separation slicers) against the same receiver with it switched off, on
//   1. the overlap scenario of the generator (adsbOverlapScenario): pairs of replies 2 to 118 us apart, -12 to +12 dB, own carrier offsets
//      and phases; at 2, 2.4 and 8 Msps, with a carrier offset of the whole capture, at three noise levels, error correction 1 and 2
//   2. a busy airspace from the generator (AdsbAirspace, 150 and 300 aircraft): the rate of the overlapped frames must go up, the rate of the
//      frames alone must not go down, and no phantom messages may appear
// Simulated signals only. ONAIR_TEST_LONG=1 adds more pairs, rates and densities.
#include "dect2/adsb/adsb_gen.h"
#include "dect2/adsb/adsb_sim.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Res { size_t sent = 0, ok = 0, both = 0, pairs = 0, phantom = 0; double cpu = 0, sig = 0; size_t okAlone = 0, nAlone = 0, okOver = 0, nOver = 0; };

static Res runPairs(bool overlap, double rate, double snr, double cfo, int fix, int pairs, uint32_t seed) {
    AdsbOverlapConfig oc;
    oc.pairs = pairs; oc.seed = seed;
    AdsbSimConfig c;
    c.rate = rate; c.snrDb = snr; c.cfoHz = cfo; c.fixBits = fix; c.seconds = 0; c.seed = seed;
    c.custom = adsbOverlapScenario(oc);
    AdsbReceiver rx;
    rx.setOverlap(overlap);
    rx.setEffort(2);
    const AdsbSimResult r = adsbSimulate(c, rx);
    Res o;
    o.sent = r.sent.size(); o.ok = r.decoded; o.phantom = r.phantom; o.cpu = r.cpuSec; o.sig = r.signalSec;
    // the list is in order of time, and a pair's second reply starts before the next pair's first
    for (size_t i = 0; i + 1 < r.sent.size(); i += 2) { o.pairs++; if (r.sent[i].decoded && r.sent[i + 1].decoded) o.both++; }
    return o;
}

static Res runAir(bool overlap, int aircraft, double mult, double secs, int fix) {
    AdsbSimConfig c;
    c.rate = 2e6; c.snrDb = 25; c.aircraft = aircraft; c.mult = mult; c.seconds = secs; c.seed = 3; c.fixBits = fix;
    AdsbReceiver rx;
    rx.setOverlap(overlap);
    rx.setEffort(2);
    const AdsbSimResult r = adsbSimulate(c, rx);
    Res o;
    o.sent = r.sent.size(); o.ok = r.decoded; o.phantom = r.phantom; o.cpu = r.cpuSec; o.sig = r.signalSec;
    for (const auto& f : r.sent) {
        if (f.snrDb < 8) continue;   // the floor of the receiver: below it overlap or not makes little difference
        if (f.overlapped) { o.nOver++; o.okOver += f.decoded; } else { o.nAlone++; o.okAlone += f.decoded; }
    }
    return o;
}

static double pct(size_t a, size_t b) { return b ? 100.0 * (double)a / (double)b : 0.0; }

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;
    const int pairs = longRun ? 600 : 150;

    // 1. the overlap scenario
    struct Case { double rate, snr, cfo; int fix; };
    std::vector<Case> cases = {{2e6, 30, 0, 1}, {2e6, 30, 0, 2}, {2e6, 20, 40e3, 2}, {2.4e6, 30, -60e3, 1}, {8e6, 30, 25e3, 2}, {2e6, 14, 0, 2}};
    if (longRun) for (double rate : {2e6, 2.4e6, 4e6, 10e6}) for (double snr : {12.0, 18.0, 25.0, 35.0}) cases.push_back({rate, snr, 50e3, 2});
    for (const Case& k : cases) {
        const Res off = runPairs(false, k.rate, k.snr, k.cfo, k.fix, pairs, 11);
        const Res on = runPairs(true, k.rate, k.snr, k.cfo, k.fix, pairs, 11);
        printf("pairs %.1f Msps, %2.0f dB, %+3.0f kHz, fix %d: decoded %5.1f %% -> %5.1f %%, both of a pair %5.1f %% -> %5.1f %%, phantoms %zu -> %zu, "
               "CPU %.3f -> %.3f s/s\n", k.rate / 1e6, k.snr, k.cfo / 1e3, k.fix, pct(off.ok, off.sent), pct(on.ok, on.sent), pct(off.both, off.pairs),
               pct(on.both, on.pairs), off.phantom, on.phantom, off.cpu / off.sig, on.cpu / on.sig);
        CHECK(on.phantom <= off.phantom + 1, "phantoms %zu -> %zu", off.phantom, on.phantom);
        // the recovery is meant to show above the noise floor; near it the gain is smaller but there may be no loss
        if (k.snr >= 20) CHECK(pct(on.ok, on.sent) >= pct(off.ok, off.sent) + 10.0, "%.1f Msps %.0f dB: decoded %.1f %% -> %.1f %%", k.rate / 1e6, k.snr, pct(off.ok, off.sent), pct(on.ok, on.sent));
        else CHECK(on.ok + 2 >= off.ok, "%.1f Msps %.0f dB: decoded %zu -> %zu", k.rate / 1e6, k.snr, off.ok, on.ok);
    }

    // 2. traffic densities from the airspace generator
    std::vector<std::pair<int, double>> dens = {{150, 2.0}, {300, 2.0}};
    if (longRun) { dens.push_back({300, 4.0}); dens.push_back({60, 1.0}); }
    for (const auto& d : dens) {
        const Res off = runAir(false, d.first, d.second, longRun ? 4.0 : 1.0, 2);
        const Res on = runAir(true, d.first, d.second, longRun ? 4.0 : 1.0, 2);
        printf("airspace %d aircraft x%.0f: %zu frames (%.1f %% overlapped); overlapped %5.1f %% -> %5.1f %%, alone %5.1f %% -> %5.1f %%, "
               "phantoms %zu -> %zu, CPU %.3f -> %.3f s/s\n", d.first, d.second, off.sent, pct(off.nOver, off.nOver + off.nAlone),
               pct(off.okOver, off.nOver), pct(on.okOver, on.nOver), pct(off.okAlone, off.nAlone), pct(on.okAlone, on.nAlone), off.phantom, on.phantom,
               off.cpu / off.sig, on.cpu / on.sig);
        CHECK(pct(on.okOver, on.nOver) >= pct(off.okOver, off.nOver) + 5.0, "overlapped frames %.1f %% -> %.1f %%", pct(off.okOver, off.nOver), pct(on.okOver, on.nOver));
        CHECK(on.okAlone + 2 >= off.okAlone, "frames alone %zu -> %zu", off.okAlone, on.okAlone);
        CHECK(on.phantom <= off.phantom + 1, "phantoms %zu -> %zu", off.phantom, on.phantom);
    }

    // 3. nothing on the air but noise: the recovery must not invent anything
    {
        AdsbSimConfig c;
        c.rate = 2e6; c.aircraft = 0; c.seconds = 1.0; c.custom = {};
        AdsbOverlapConfig oc; oc.pairs = 0;
        AdsbReceiver rx;
        rx.setOverlap(true);
        c.custom = adsbOverlapScenario(oc);
        c.aircraft = 0;
        const AdsbSimResult r = adsbSimulate(c, rx);
        CHECK(r.phantom == 0, "noise only: %zu phantoms", r.phantom);
    }

    // 4. the scenario itself: the same list for the same seed, offsets and levels inside their ranges
    {
        AdsbOverlapConfig oc; oc.pairs = 50; oc.seed = 5;
        const auto a = adsbOverlapScenario(oc), b = adsbOverlapScenario(oc);
        CHECK(a.size() == 100 && b.size() == 100, "scenario size %zu", a.size());
        bool same = a.size() == b.size(), inRange = true;
        for (size_t i = 0; i + 1 < a.size() && same; i += 2) {
            same = a[i].t == b[i].t && a[i + 1].amp == b[i + 1].amp;
            const double off = (a[i + 1].t - a[i].t) * 1e6, rel = 20 * std::log10(a[i + 1].amp / a[i].amp);
            if (off < oc.offLoUs - 1e-6 || off > oc.offHiUs + 1e-6 || rel < oc.relLoDb - 1e-3 || rel > oc.relHiDb + 1e-3) inRange = false;
        }
        CHECK(same, "scenario not repeatable");
        CHECK(inRange, "scenario offsets or levels out of range");
    }
    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
