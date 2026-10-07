// Input sample rates from 2.046 to 10 Msps (the radio's choice, not the receiver's): the receiver must lock the satellites, find the frame start and the
// time of every satellite, and measure pseudoranges that agree with the simulated truth. 20 s of signal each: the first subframe ends 14 s in.
// (20 Msps and the position fix are in test_gnss_rate20.)
#include "dect2/gnss_testkit.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::gnsstest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rates[] = {2.046e6, 3.2e6, 5e6, 10e6};
    for (double rate : rates) {
        GnssSimConfig cfg;
        GnssSim sim(cfg, rate);
        int nTx = 0;
        for (auto& s : sim.sats()) nTx += s.transmitted;
        Options o;
        o.rate = rate; o.secs = 20;
        Run r = run(sim, o);
        const GnssTelemetry& t = r.tel;
        int framed = 0, locked = 0;
        double rmsSum = 0, meanWorst = 0; int nr = 0;
        double cnErr = 0; int cnN = 0;
        for (auto& c : t.channels) {
            if (c.state >= GnssChLocked) { locked++; cnErr += c.cn0 - sim.sats()[(size_t)c.prn - 1].cn0; cnN++; }
            if (c.state >= GnssChFrameSync) framed++;
            const RangeStats st = rangeError(sim, r, c.prn, 14.5);
            if (st.n >= 3) { rmsSum += st.rms * st.rms; meanWorst = std::fmax(meanWorst, std::fabs(st.mean)); nr++; }
        }
        printf("%5.3f Msps: %d of %d satellites locked, %d with the frame found, C/N0 error %+.2f dB, pseudorange rms %.2f m (worst mean %.2f m, %d satellites), %.1fx real time\n", rate / 1e6, locked, nTx, framed,
               cnN ? cnErr / cnN : 0.0, nr ? std::sqrt(rmsSum / nr) : 0.0, meanWorst, nr, o.secs / r.cpuSecs);
        CHECK(locked >= nTx - 1, "%.3f Msps: only %d of %d locked", rate / 1e6, locked, nTx);
        CHECK(framed >= nTx - 2, "%.3f Msps: only %d with the frame", rate / 1e6, framed);
        CHECK(nr >= nTx - 2, "%.3f Msps: pseudorange statistics for %d satellites", rate / 1e6, nr);
        CHECK(nr && std::sqrt(rmsSum / nr) < 5.0 && meanWorst < 5.0, "%.3f Msps: pseudorange error rms %.2f, mean %.2f", rate / 1e6, nr ? std::sqrt(rmsSum / nr) : 0.0, meanWorst);
        CHECK(std::fabs(cnErr / std::max(cnN, 1)) < 2.0, "%.3f Msps: C/N0 off by %.2f dB", rate / 1e6, cnErr / std::max(cnN, 1));
        CHECK(t.blocksBad == 0, "%.3f Msps: %llu bad subframes", rate / 1e6, (unsigned long long)t.blocksBad);
        if (!GNSS_SANITIZED) CHECK(o.secs / r.cpuSecs > 2.5, "%.3f Msps: real-time factor %.1f", rate / 1e6, o.secs / r.cpuSecs);
    }
    // a rate below the minimum: the receiver says so and does not crash
    {
        GnssReceiver rx;
        rx.configure(1.5e6);
        std::vector<cf32> x(65536, cf32(0.1f, 0.f));
        rx.feed(x.data(), x.size());
        CHECK(!rx.ready(), "1.5 Msps should not be ready");
        GnssTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.channels.empty() && t.activeMask == 0, "no band should be active at 1.5 Msps");
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
