// Inmarsat-C at the sample rates of the radios: 0.25 (RTL-SDR minimum), 0.96, 2, 2.4, 3.2, 8, 10, 20 Msps, with a non-integer ratio among them.
// Slow: the generator has to make up to 20 million samples a second.
#include "dect2/inmc_testutil.h"
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rates[] = {250000, 960000, 2e6, 2.4e6, 3.2e6, 2.88e6, 8e6, 10e6, 20e6};
    for (double rate : rates) {
        InmcRunOpts o;
        o.gen.rate = rate; o.gen.ebn0Db = 8; o.gen.seed = 6; o.gen.cfoHz = -2500; o.gen.sroPpm = 20;
        o.secs = rate > 5e6 ? 32 : 40;
        o.chunk = 16384;
        o.quantize8 = true;
        const InmcRunResult r = runInmc(o);
        const InmcTelemetry& t = r.t;
        printf("  %.2f Msps: sync %.1f s, %llu good, %llu bad frames, cfo %+.0f Hz, Eb/N0 %.1f dB, receiver %.0fx real time\n", rate / 1e6, r.firstSyncSec,
               (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.cfoHz, t.ebn0Db, r.rtf);
        CHECK(t.blocksOk >= 2 && t.blocksBad == 0, "%.2f Msps: %llu good, %llu bad", rate / 1e6, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(std::fabs(t.cfoHz + 2500) < 4, "%.2f Msps: carrier %+.1f Hz", rate / 1e6, t.cfoHz);
        CHECK(r.rtf > 3, "%.2f Msps: only %.1fx real time", rate / 1e6, r.rtf);
    }
    printf(fails ? "FAILED\n" : "inmc_slow_rates ok\n");
    return fails ? 1 : 0;
}
