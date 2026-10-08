// ACARS at 20 Msps (the HackRF's widest exact rate): three channels, 8-bit samples, a clock offset. Slow: the generator alone needs most of the time.
#include "dect2/acars_sim.h"
#include <cstdio>
using namespace dect2;
int main() {
    AcarsSimCfg c;
    c.rate = 20e6;
    c.secs = 20;
    c.syn.snrDb = 30;
    c.syn.sroPpm = 30;
    c.gen.rateFactor = 4;
    c.quant8 = true;
    const auto r = runAcarsSim(c);
    printf("  20 Msps: sent %d, decoded %d, wrong %d, %.1f x real time\n", r.sent, r.decoded, r.wrong, r.rtf);
    int fails = 0;
    if (r.sent < 30) { printf("FAIL: only %d blocks sent\n", r.sent); fails++; }
    if (r.decoded * 100 < r.sent * 98) { printf("FAIL: %d of %d decoded\n", r.decoded, r.sent); fails++; }
    if (r.wrong) { printf("FAIL: %d wrong messages\n", r.wrong); fails++; }
    printf(fails ? "acars rate20: %d FAILED\n" : "acars rate20: all passed\n", fails);
    return fails ? 1 : 0;
}
