// Low-SNR sensitivity of the whole chain (detector + demodulator + frame): bursts found and bit-exact per Es/N0 and detection threshold.
#include "dect2/iridium/iridium_testkit.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::iridiumtest;
#include "../common/test_util.h"
#include "../common/test_long.h"

int main(int argc, char** argv) {
    const bool report = argc > 1;
    for (double thr : {13.0, 10.0, 8.0}) {
        printf("threshold %.0f dB\n", thr);
        // by default each threshold at the ends of the Es/N0 range (12 and 0 dB); the long run (ONAIR_TEST_LONG=1) and the report mode step by 2 dB
        for (double snr = 12; snr >= 0; snr -= (report || testLong()) ? 2 : 12) {
            SceneOpts so; so.rate = 4e6; so.secs = 3.2; so.perSec = 125; so.esn0Db = snr; so.seed = 700 + (uint32_t)snr;
            RunOpts ro;
            const auto bursts = makeScene(so, ro.noiseSigma);
            IridiumReceiver rx; rx.setThresholdDb(thr);
            RunResult r = runScene(bursts, so.rate, so.secs, ro, &rx);
            const Match m = match(bursts, r.got, ro.centerMhz * 1e6, 0, 1e9, 0);
            printf("  Es/N0 %4.1f: %4d sent, found %5.1f %%, exact %5.1f %%, extra %d\n", snr, m.sent, 100.0 * m.found / std::max(1, m.sent),
                   100.0 * m.exact / std::max(1, m.sent), m.extra);
            if (!report && thr == 13.0 && snr >= 12) CHECK(m.exact >= 0.95 * m.sent, "baseline exact at %.0f dB", snr);
        }
    }
    return testSummary("iridium sens");
}
