// DTMB in a 6 MHz channel (Cuba, the American 6 MHz raster): the same signal at 5.67 Msym/s instead of 7.56. The receiver set to 6 MHz
// decodes it at several radio rates, including 6 Msps that an 8 MHz channel could not use; set to 8 MHz it finds nothing, and the 8 MHz
// channel still works as before.
#include "dect2/dtmb_testkit.h"
#include <cstdio>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static kit::Result go(const char* name, double bwSignal, double bwRx, double rate, Header h, Mapping m, Rate rt) {
    kit::Scenario o;
    o.sc = kit::makeSignal(h, m, rt, false, rate, 30);
    o.sc.symbolRate = symbolRateFor(bwSignal);
    o.rxBwMhz = bwRx;
    o.seconds = 3.0;
    o.threads = 0;   // deterministic
    const kit::Result r = kit::run(o);
    printf("  %-40s first packet %.2f s, %llu good, %llu wrong, state %d, %.2f Mbit/s\n", name, r.firstGoodSec, (unsigned long long)r.good, (unsigned long long)r.wrong,
           r.tel.state, netBitrate(h, {m, rt, false}, o.sc.symbolRate) / 1e6);
    return r;
}

int main() {
    CHECK(std::fabs(symbolRateFor(6) - 5.67e6) < 1, "6 MHz: %.0f sym/s", symbolRateFor(6));
    CHECK(symbolRateFor(8) == kSymbolRate && symbolRateFor(0) == kSymbolRate, "8 MHz and unknown: 7.56 Msym/s");
    // a 6 MHz channel at 10, 8 and 6 Msps
    for (double rate : {10e6, 8e6, 6e6}) {
        char n[64]; snprintf(n, sizeof n, "6 MHz, PN945 64QAM 0.6 at %.0f Msps", rate / 1e6);
        const kit::Result r = go(n, 6, 6, rate, Header::Pn945, Mapping::Qam64, Rate::R06);
        CHECK(r.firstGoodSec >= 0 && r.firstGoodSec < 2.0 && r.good > 1000 && r.wrong == 0, "%s: first %.2f s, %llu good, %llu wrong", n, r.firstGoodSec, (unsigned long long)r.good, (unsigned long long)r.wrong);
    }
    {
        const kit::Result r = go("6 MHz, PN420 16QAM 0.8 at 10 Msps", 6, 6, 10e6, Header::Pn420, Mapping::Qam16, Rate::R08);
        CHECK(r.good > 1000 && r.wrong == 0, "PN420 16QAM in 6 MHz: %llu good, %llu wrong", (unsigned long long)r.good, (unsigned long long)r.wrong);
    }
    {   // the receiver left at 8 MHz does not decode a 6 MHz channel (what the user saw), nor the other way round
        const kit::Result r = go("6 MHz signal, receiver at 8 MHz", 6, 8, 10e6, Header::Pn945, Mapping::Qam64, Rate::R06);
        CHECK(r.good == 0, "a 6 MHz signal decoded at 8 MHz: %llu packets", (unsigned long long)r.good);
        const kit::Result r2 = go("8 MHz signal, receiver at 6 MHz", 8, 6, 10e6, Header::Pn945, Mapping::Qam64, Rate::R06);
        CHECK(r2.good == 0, "an 8 MHz signal decoded at 6 MHz: %llu packets", (unsigned long long)r2.good);
    }
    {
        const kit::Result r = go("8 MHz, PN945 64QAM 0.6 at 10 Msps", 8, 8, 10e6, Header::Pn945, Mapping::Qam64, Rate::R06);
        CHECK(r.good > 1000 && r.wrong == 0, "8 MHz: %llu good", (unsigned long long)r.good);
    }
    {   // 6 Msps is too slow for an 8 MHz channel
        DtmbReceiver rx;
        rx.configure(6e6, 8);
        CHECK(!rx.ready(), "an 8 MHz channel at 6 Msps should be refused");
        rx.configure(6e6, 6);
        CHECK(rx.ready(), "a 6 MHz channel at 6 Msps should work");
    }
    printf(failures ? "dtmb bw6: FAILED\n" : "dtmb bw6: ok\n");
    return failures ? 1 : 0;
}
