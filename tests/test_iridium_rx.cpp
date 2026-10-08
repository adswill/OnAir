// Iridium receiver round trip with random payloads: bursts all over the band at every radio rate, any chunk size, downlink and
// uplink, bursts overlapping in time on other channels; the bits after the unique word must come back exactly.
#include "dect2/iridium_testkit.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::iridiumtest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Match roundTrip(const char* name, const SceneOpts& so, const RunOpts& ro, RunResult* out = nullptr) {
    const auto bursts = makeScene(so, ro.noiseSigma);
    RunResult r = runScene(bursts, so.rate, so.secs, ro);
    const Match m = match(bursts, r.got, ro.centerMhz * 1e6);
    printf("%-34s %4d sent, %4d found, %4d exact, %5d bit errors, %d extra, max errors %.0f Hz %.1f us, %.2fx real time\n", name, m.sent, m.found, m.exact,
           m.bitErrors, m.extra, m.maxFreqErr, m.maxTimeErr * 1e6, so.secs / std::max(1e-9, r.cpuSecs));
    if (out) *out = std::move(r);
    return m;
}

int main() {
    // every rate, the whole band each covers
    for (double rate : {2e6, 2.4e6, 4e6, 8e6, 10e6, 20e6}) {
        SceneOpts so; so.rate = rate; so.secs = 1.5; so.seed = (uint32_t)(rate / 1e5);
        RunOpts ro;
        char nm[64]; snprintf(nm, sizeof nm, "%.1f Msps", rate / 1e6);
        const Match m = roundTrip(nm, so, ro);
        CHECK(m.sent > 100 && m.exact == m.sent && m.extra == 0, "%s: %d of %d exact, %d extra", nm, m.exact, m.sent, m.extra);
        CHECK(m.maxFreqErr < 150 && m.maxTimeErr < 3e-6, "%s: frequency %.0f Hz, time %.2f us", nm, m.maxFreqErr, m.maxTimeErr * 1e6);
    }
    // chunk sizes
    for (size_t ch : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) {
        SceneOpts so; so.rate = 2.4e6; so.secs = ch == 1 ? 0.6 : 1.0; so.seed = 40 + (uint32_t)ch;
        RunOpts ro; ro.chunk = ch;
        char nm[64]; snprintf(nm, sizeof nm, "chunks of %zu", ch);
        const Match m = roundTrip(nm, so, ro);
        CHECK(m.sent > 30 && m.exact == m.sent, "%s: %d of %d exact", nm, m.exact, m.sent);
    }
    // the simplex channels only (2.4 Msps on 1626.25 MHz), long bursts
    {
        SceneOpts so; so.rate = 2.4e6; so.secs = 2; so.bandHz = 450e3; so.simplexFrac = 1; so.longSimplex = true; so.perSec = 60; so.seed = 79;
        RunOpts ro; ro.centerMhz = 1626.25;
        const Match m = roundTrip("simplex band, long bursts", so, ro);
        CHECK(m.sent > 50 && m.exact == m.sent, "%d of %d exact", m.exact, m.sent);
    }
    // uplink and downlink mixed
    {
        SceneOpts so; so.rate = 10e6; so.secs = 1.5; so.uplinkFrac = 0.5; so.seed = 77;
        RunOpts ro;
        const Match m = roundTrip("uplink and downlink", so, ro);
        CHECK(m.sent > 100 && m.exact == m.sent && m.wrongDir == 0, "%d of %d exact, %d wrong direction", m.exact, m.sent, m.wrongDir);
    }
    // bursts overlapping in time on different channels (at least 3 channels apart), 600 bursts a second
    {
        SceneOpts so; so.rate = 10e6; so.secs = 1.5; so.overlap = true; so.perSec = 600; so.seed = 78;
        RunOpts ro;
        RunResult r;
        const Match m = roundTrip("overlapping, 600 bursts/s", so, ro, &r);
        CHECK(m.sent > 700 && m.exact >= m.sent * 0.995, "%d of %d exact", m.exact, m.sent);
        CHECK(r.tel.bursts >= (uint64_t)m.sent && r.tel.uwOk >= (uint64_t)m.exact && r.tel.dropped == 0, "telemetry: %llu bursts, %llu unique words, %llu dropped",
              (unsigned long long)r.tel.bursts, (unsigned long long)r.tel.uwOk, (unsigned long long)r.tel.dropped);
        CHECK(r.tel.seq >= 5 && r.tel.state >= 1 && r.tel.scatter.size() > 300, "report %llu state %d scatter %zu",
              (unsigned long long)r.tel.seq, r.tel.state, r.tel.scatter.size());
        CHECK(r.tel.snrDb > 18 && r.tel.snrDb < 30 && r.tel.confidence > 95, "snr %.1f confidence %.0f", r.tel.snrDb, r.tel.confidence);
    }
    printf(fails ? "iridium rx: %d FAILED\n" : "iridium rx: all passed\n", fails);
    return fails ? 1 : 0;
}
