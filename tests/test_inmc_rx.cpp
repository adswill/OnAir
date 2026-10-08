// Inmarsat-C: generator to receiver round trip. The loop of five messages must come back bit exact (text, service, priority, id),
// with normal and inverted polarity, and the channel readings must be sane.
#include "dect2/inmc_testutil.h"
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void one(const char* name, bool invert, double ebn0, double secs) {
    InmcRunOpts o;
    o.gen.rate = 2e6; o.gen.ebn0Db = ebn0; o.gen.invert = invert; o.secs = secs;
    const InmcRunResult r = runInmc(o);
    const InmcTelemetry& t = r.t;
    printf("  %s: first sync %.1f s, %llu frames, %d of 5 messages complete, Es/N0 %.1f dB, UW errors avg %.1f, %s\n", name, r.firstSyncSec,
           (unsigned long long)t.blocksOk, r.completeMatches, t.esn0Db, t.uwErrorsAvg, inmcSummary(t).c_str());
    CHECK(r.firstSyncSec > 0 && r.firstSyncSec < 25, "%s: first sync after %.1f s", name, r.firstSyncSec);
    CHECK(t.state == 2 && t.frameLock && t.carrierLock, "%s: state %d", name, t.state);
    CHECK(t.blocksOk >= (uint64_t)(secs / 8.64) - 3 && t.blocksBad == 0, "%s: %llu good, %llu bad frames", name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    CHECK(r.completeMatches == 5, "%s: %d of 5 messages bit exact", name, r.completeMatches);
    CHECK(std::fabs(t.ebn0Db - ebn0) < 1.2, "%s: Eb/N0 read %.1f dB, set %.1f", name, t.ebn0Db, ebn0);
    CHECK(std::fabs(t.cfoHz) < 5, "%s: carrier offset %.1f Hz", name, t.cfoHz);
    CHECK(t.ncs.valid && t.ncs.sat == 3 && t.ncs.lesId == 44 && t.ncs.channelType == 1, "%s: NCS info", name);
    CHECK(t.dataValid, "%s: no data valid", name);
    CHECK(t.uwErrorsAvg < 40, "%s: unique word errors %.1f", name, t.uwErrorsAvg);
}

int main() {
    one("normal polarity, 10 dB", false, 10, 110);
    one("inverted polarity, 10 dB", true, 10, 110);
    printf(fails ? "FAILED\n" : "inmc_rx ok\n");
    return fails ? 1 : 0;
}
