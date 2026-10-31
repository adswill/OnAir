// Automatic gain on bursty signals (3.2 quality pass): a weak wanted signal with a strong transmitter that keys up now and then, against the
// simulated radio of autogain_sim.h. Per case: seconds of clipping per burst, gain changes per burst (pumping), the gain lost after the bursts
// against before, and the seconds from the end of a burst until the gain is back within 3 dB of where it was. Prints the table; the limits are
// the ones the 3.2 changes reach (before: the gain after six bursts was 12 to 44 dB below where it started, 4 to 7 changes per burst). Any argument: print only.
#include "autogain_sim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dect2;
using namespace dect2::ag;
using namespace agsim;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { const char* radio; const char* name; double burstDbm, onSec, periodSec; double maxClip, maxChanges, maxRecover; };

struct Res { double clipPerBurst = 0, changesPerBurst = 0, recover = 0, lostDb = 0, decodable = 0; };

static Res runCase(const Case& k) {
    const RadioProfile* rp = radioProfileById(k.radio);
    if (!rp) { printf("no profile %s\n", k.radio); exit(1); }
    SimRadio sim(*rp, 30);
    sim.rateHz = 2.4e6; sim.chanBw = 12.5e3; sim.chanOff = 100e3; sim.sigDbm = -108;
    const ModeTarget tg = modeTargetFor(11, 12.5e3, 2.4e6, 446e6);
    AutoGainController c;
    AgSettings s; s.mode = AgMode::Auto; s.v2 = getenv("AGV2") != nullptr;   // the legacy loop by default (the checks below are its); AGV2=1 runs the same bursts through Auto v2
    c.configure(*rp, tg, s);
    double now = 0;
    c.start(now, sim.live.totalDb, false, &sim.live);
    run(c, sim, now, 40);
    const double settled = c.current().totalDb;
    if (getenv("AGLOG")) printf("settled snr %.1f dB (decoder locks at %.0f)\n", sim.snrDb(), sim.lockAt);
    const int bursts = 6;
    Res r;
    double prev = settled, recoverSum = 0, okSec = 0, gapSec = 0;
    int recovered = 0;
    for (int b = 0; b < bursts; b++) {
        sim.blockers = {{500e3, k.burstDbm}};
        run(c, sim, now, k.onSec, [&](double) {
            if (sim.lastClip > tg.clipLimit) r.clipPerBurst += 0.05;
            if (std::fabs(c.current().totalDb - prev) > 0.01) { prev = c.current().totalDb; r.changesPerBurst++; }
        });
        sim.blockers.clear();
        const double t0 = now;
        double back = -1;
        run(c, sim, now, k.periodSec - k.onSec, [&](double t) {
            if (std::fabs(c.current().totalDb - prev) > 0.01) { prev = c.current().totalDb; r.changesPerBurst++; }
            if (back < 0 && sim.snrDb() >= sim.lockAt) back = t - t0;
            if (sim.snrDb() >= sim.lockAt) okSec += 0.05;
            gapSec += 0.05;
        });
        if (back >= 0) { recoverSum += back; recovered++; } else recoverSum += k.periodSec - k.onSec;
    }
    r.clipPerBurst /= bursts; r.changesPerBurst /= bursts; r.recover = recoverSum / bursts;
    r.lostDb = settled - c.current().totalDb;
    if (getenv("AGLOG")) { printf("settled %.1f\n", settled); for (const auto& e : c.log()) printf("   %.1f %.1f -> %.1f %s\n", e.t, e.fromDb, e.toDb, e.why.c_str()); }
    r.decodable = gapSec > 0 ? okSec / gapSec : 0;
    (void)recovered;
    return r;
}

int main(int argc, char** argv) {
    const bool printOnly = argc > 1;
    const Case cases[] = {
        {"hackrf", "short strong burst 0.3 s -30 dBm / 6 s", -30, 0.3, 6, 0.8, 3.5, 40},
        {"hackrf", "burst 1.5 s -28 dBm / 20 s", -28, 1.5, 20, 1.0, 6.0, 40},
        {"hackrf", "burst 4 s -35 dBm / 30 s", -35, 4, 30, 1.0, 6.5, 40},
        {"hackrf", "burst 0.5 s -20 dBm / 5 s (bursty)", -20, 0.5, 5, 0.8, 5.0, 40},
        {"rtl-r820t", "burst 1.5 s -30 dBm / 20 s", -30, 1.5, 20, 1.0, 5.0, 40},
    };
    printf("%-10s %-38s %9s %9s %10s %9s\n", "radio", "case", "clip s", "changes", "relock s", "lost dB");
    for (const Case& k : cases) {
        if (argc > 2 && !strstr(k.name, argv[2])) continue;
        const Res r = runCase(k);
        printf("%-10s %-38s %9.2f %9.1f %10.1f %9.1f\n", k.radio, k.name, r.clipPerBurst, r.changesPerBurst, r.recover, r.lostDb);
        if (!printOnly) {
            CHECK(r.clipPerBurst <= k.maxClip, "%s: clipped %.2f s per burst", k.name, r.clipPerBurst);
            CHECK(r.changesPerBurst <= k.maxChanges, "%s: %.1f gain changes per burst", k.name, r.changesPerBurst);
            CHECK(r.recover <= k.maxRecover, "%s: recovery %.1f s", k.name, r.recover);
            CHECK(r.lostDb <= 16, "%s: ended %.1f dB below the settled gain (a burst every few seconds used to ratchet the gain down by 32 dB)", k.name, r.lostDb);
        }
    }
    printf(fails ? "%d FAILED\n" : "done\n", fails);
    return fails ? 1 : 0;
}
