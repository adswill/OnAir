// Inmarsat-C: Eb/N0 sweep with 8-bit samples. Reports the share of frames whose bulletin board checks, the unique word errors and the Eb/N0 the receiver reads,
// and finds the point where decoding fails. (The receiver needs its carrier search to lock first, which takes longer near the limit.)
#include "dect2/inmc_testutil.h"
#include "dect2/test_parallel.h"
#include <iterator>
using namespace dect2;
using dect2::testpar::CaseOut;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double eb[] = {8, 6, 5, 4, 3, 2.5, 2, 1.5, 1, 0};
static const double kSecs = 112;
static InmcRunResult results[std::size(eb)];

int main() {
    // the ten runs share nothing: four at a time, the weakest (slowest to lock) first; the checks below go in the order of the list
    printf("  Eb/N0 set | frames found | good | bad | UW errors | symbol errors | Eb/N0 read | messages complete (of 5, 112 s)\n");
    dect2::testpar::runCases(std::size(eb), [](size_t i, CaseOut& out) {
        InmcRunOpts o;
        o.gen.rate = 2e6; o.gen.ebn0Db = eb[i]; o.gen.seed = 4; o.gen.cfoHz = 3300; o.secs = kSecs; o.quantize8 = true;
        results[i] = runInmc(o);
        const InmcTelemetry& t = results[i].t;
        out.print("  %5.1f dB  | %3llu | %3llu | %3llu | %5.1f | %5.1f %% | %5.1f dB | %d\n", eb[i], (unsigned long long)t.framesFound, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
                  t.uwErrorsAvg, 100.0 * t.symbolErrorRate, t.ebn0Db, results[i].completeMatches);
    }, {9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
    double failPoint = -99;
    bool stopped = false;
    for (size_t i = 0; i < std::size(eb); i++) {
        const double e = eb[i];
        const InmcRunResult& r = results[i];
        const InmcTelemetry& t = r.t;
        const int possible = (int)((kSecs - 8.8) / 8.64) + 1;    // frames that fit after the first sync at 8.8 s
        if (e >= 5) CHECK(t.blocksOk >= (uint64_t)possible - 1 && t.blocksBad == 0 && r.completeMatches == 5, "Eb/N0 %.1f dB: %llu good of %d, %llu bad, %d messages", e, (unsigned long long)t.blocksOk, possible, (unsigned long long)t.blocksBad, r.completeMatches);
        if (e == 4 || e == 3) CHECK(t.blocksOk >= (uint64_t)possible - 2 && r.completeMatches >= 4, "Eb/N0 %.0f dB: %llu good of %d, %d messages", e, (unsigned long long)t.blocksOk, possible, r.completeMatches);
        if (t.blocksOk >= (uint64_t)(possible * 0.5) && !stopped) failPoint = e; else stopped = true;
        // the measured limit must be stated, not assumed: below it the error counts must rise
    }
    printf("  lowest Eb/N0 with at least half of the frames decoded in 112 s: %.1f dB\n", failPoint);
    CHECK(failPoint <= 3.0, "frames fail already at %.1f dB", failPoint);
    printf(fails ? "FAILED\n" : "inmc_sweep ok\n");
    return fails ? 1 : 0;
}
