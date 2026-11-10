// DTMB packet timing: the seconds the receiver hands over with its packets (the clock the UDP output and the recordings pace on) must add
// up to the signal time, for every header, mapping, code rate, interleaver mode and both channel widths, so that an outside player is fed
// at the stream's own rate (#88, #254).
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dtmb/dtmb_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include "../common/test_long.h"

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Case { Header h; Mapping m; Rate r; bool mode2; double bw; };

static void runCase(const Case& c) {
    SignalConfig sc;
    sc.rate = 10e6; sc.snrDb = 30;
    sc.symbolRate = symbolRateFor(c.bw);
    sc.tx.header = c.h; sc.tx.profile.map = c.m; sc.tx.profile.rate = c.r; sc.tx.profile.mode2 = c.mode2;
    sc.tx.phaseRotate = c.h != Header::Pn595;
    if (!profileValid(sc.tx.profile)) return;
    Signal sig(sc, testPacketSource(1));
    DtmbReceiver rx;
    rx.configure(sc.rate, c.bw);
    rx.setDecoderThreads(0);
    double secs = 0, now = 0, firstAt = -1;
    uint64_t pk = 0, calls = 0;
    double maxCall = 0;
    rx.setPacketCallback([&](const uint8_t*, size_t n, double s) {
        if (firstAt < 0) { firstAt = now; return; }   // the first call carries the time since the lock, not that of its packets
        secs += s; pk += n; calls++; maxCall = std::max(maxCall, s);
    });
    const double seconds = c.mode2 ? 3.0 : 2.0;
    const size_t total = (size_t)(seconds * sc.rate), block = 16384;
    std::vector<cf32> x(block);
    double tFirst = -1;
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(block, total - done);
        sig.generate(x.data(), n);
        now = (double)done / sc.rate;
        rx.feed(x.data(), n);
        if (firstAt >= 0 && tFirst < 0) tFirst = (double)(done + n) / sc.rate;
        done += n;
    }
    const double net = netBitrate(c.h, sc.tx.profile, sc.symbolRate);
    char name[96];
    snprintf(name, sizeof name, "%s %s %s mode %d %.0f MHz", headerInfo(c.h).name, mappingName(c.m), rateName(c.r), c.mode2 ? 2 : 1, c.bw);
    CHECK(pk > 1000, "%s: %llu packets", name, (unsigned long long)pk);
    if (pk <= 1000) return;
    // the bit rate the timing implies is the net rate of the mode
    const double implied = (double)pk * 1504.0 / secs;
    CHECK(std::fabs(implied / net - 1.0) < 0.01, "%s: packets over the seconds handed over give %.4f Mbit/s, the mode carries %.4f", name, implied / 1e6, net / 1e6);
    // and the seconds follow the signal (within a few codewords of what is still in the decoder)
    const double span = seconds - tFirst;
    CHECK(secs > span - 0.05 && secs < span + 0.05, "%s: %.3f s handed over for %.3f s of signal", name, secs, span);
    CHECK(maxCall < 0.01, "%s: one call carried %.3f s", name, maxCall);
    printf("  %-34s %8.4f Mbit/s (mode %.4f), %.3f s for %.3f s, %llu calls\n", name, implied / 1e6, net / 1e6, secs, span, (unsigned long long)calls);
}

int main() {
    if (!testLong()) {
        // the default run: each header, mapping, code rate, interleaver mode and width once, spread over the headers; ONAIR_TEST_LONG=1 the selection below
        const Case quick[] = {
            {Header::Pn420, Mapping::Qam4, Rate::R08, false, 8.0},   {Header::Pn595, Mapping::Qam16, Rate::R04, false, 8.0}, {Header::Pn945, Mapping::Qam64, Rate::R08, false, 8.0},
            {Header::Pn420, Mapping::Qam32, Rate::R08, false, 8.0},  {Header::Pn595, Mapping::Qam4Nr, Rate::R08, false, 8.0}, {Header::Pn945, Mapping::Qam16, Rate::R06, false, 8.0},
            {Header::Pn945, Mapping::Qam4, Rate::R08, true, 8.0},    {Header::Pn420, Mapping::Qam16, Rate::R08, false, 6.0}, {Header::Pn595, Mapping::Qam16, Rate::R08, true, 6.0},
        };
        for (const Case& c : quick) runCase(c);
        printf(failures ? "%d FAILED\n" : "all passed\n", failures);
        return failures ? 1 : 0;
    }
    const Header hs[] = {Header::Pn420, Header::Pn595, Header::Pn945};
    const Mapping ms[] = {Mapping::Qam4, Mapping::Qam16, Mapping::Qam64, Mapping::Qam32, Mapping::Qam4Nr};
    const Rate rs[] = {Rate::R04, Rate::R06, Rate::R08};
    for (double bw : {8.0, 6.0})
        for (Header h : hs)
            for (Mapping m : ms)
                for (Rate r : rs) {
                    // every mapping per header at 8 MHz (each code rate on 16QAM), interleaver mode 2 and 6 MHz on a selection: the seconds per
                    // codeword depend on the header, the mapping (codewords per frame group), the channel width, and the code rate only through
                    // the packets per codeword (a full sweep of every combination passed too; it takes minutes)
                    if (bw == 6.0 && !(m == Mapping::Qam16 && r == Rate::R08)) continue;
                    if (bw == 8.0 && r != Rate::R08 && m != Mapping::Qam16) continue;
                    runCase({h, m, r, false, bw});
                    if ((m == Mapping::Qam4 || m == Mapping::Qam16) && r == Rate::R08) runCase({h, m, r, true, bw});
                }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
