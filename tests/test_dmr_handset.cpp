// DMR receiver, handsets: direct mode (a handset talking to another, one time slot at a time, silence in between, a new timing at every start) and
// a mobile station talking to a repeater (the same with the mobile station sync patterns). Compared with what the generator says it sent (dmr_eval.h).
#include "dect2/dmr_eval.h"
#include "dect2/dmr_gen.h"
#include "dect2/dmr_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef DmrScenario Scn;
typedef DmrRunResult Res;
static Res run(const Scn& s) { return dmrRun(s); }

static void show(const char* what, const Res& r, double secs) {
    printf("%-36s voice %2d/%2d alias %d/%d msg %d/%d ctrl %d/%d wrong %d false %d frames %.2f | SNR %4.1f CFO %+5.0f BER %.4f | lock %.2f s | %.0fx real time\n", what, r.sc.voiceFound,
           r.sc.voiceSent, r.sc.aliasFound, r.sc.aliasSent, r.sc.messagesFound, r.sc.messagesSent, r.sc.controlFound, r.sc.controlSent, r.sc.idsWrong, r.sc.falseCalls, r.sc.frameRatio,
           r.tel.snrDb, r.tel.cfoHz, r.tel.ber, r.lockedAt, secs / std::max(r.cpu, 1e-9));
}

// everything the generator sent must be found, nothing invented
static void expectAll(const char* what, const Res& r, double secs, bool messages = true) {
    show(what, r, secs);
    CHECK(r.sc.voiceSent >= 3, "%s: the test signal held only %d voice calls", what, r.sc.voiceSent);
    CHECK(r.sc.voiceFound == r.sc.voiceSent, "%s: %d of %d voice calls found\n%s", what, r.sc.voiceFound, r.sc.voiceSent, r.sc.report.c_str());
    CHECK(r.sc.idsWrong == 0, "%s: %d calls with wrong IDs\n%s", what, r.sc.idsWrong, r.sc.report.c_str());
    CHECK(r.sc.aliasFound == r.sc.aliasSent, "%s: talker alias %d of %d\n%s", what, r.sc.aliasFound, r.sc.aliasSent, r.sc.report.c_str());
    if (messages) CHECK(r.sc.messagesFound == r.sc.messagesSent, "%s: %d of %d text messages\n%s", what, r.sc.messagesFound, r.sc.messagesSent, r.sc.report.c_str());
    CHECK(r.sc.controlFound == r.sc.controlSent, "%s: %d of %d control messages\n%s", what, r.sc.controlFound, r.sc.controlSent, r.sc.report.c_str());
    CHECK(r.sc.falseCalls == 0, "%s: %d invented log entries\n%s", what, r.sc.falseCalls, r.sc.report.c_str());
    CHECK(r.sc.frameRatio > 0.97, "%s: voice frames counted %.3f of expected", what, r.sc.frameRatio);
    CHECK(r.seqOk, "%s: telemetry sequence went backwards", what);
}

static DmrGenConfig base(double snr = 30) {
    DmrGenConfig c;
    c.snrDb = snr;
    c.traffic = 2;      // busy: plenty of calls in a short run
    return c;
}

int main() {
    // ---- direct mode: either time slot, intermittent
    {
        Scn s; s.cfg = base(22); s.cfg.direct = true; s.secs = 30;
        const Res r = run(s);
        expectAll("direct mode", r, s.secs, false);
        CHECK(r.tel.link == "direct mode", "link '%s'", r.tel.link.c_str());
        CHECK(r.tel.syncCount[5] + r.tel.syncCount[6] > 5 && r.tel.syncCount[7] + r.tel.syncCount[8] > 5, "direct mode syncs of slot 1: %llu, slot 2: %llu", (unsigned long long)(r.tel.syncCount[5] + r.tel.syncCount[6]),
              (unsigned long long)(r.tel.syncCount[7] + r.tel.syncCount[8]));
        CHECK(r.tel.syncCount[0] + r.tel.syncCount[1] + r.tel.syncCount[2] + r.tel.syncCount[3] == 0, "base station or mobile syncs in a direct mode signal");
        bool s1 = false, s2 = false;
        for (const DmrCall& c : r.tel.callLog) { if (c.slot == 1) s1 = true; if (c.slot == 2) s2 = true; }
        CHECK(s1 && s2, "direct mode calls should show up in both slots");
        Scn s2c = s; s2c.secs = 24; s2c.cfg.cfoHz = 2500; s2c.cfg.sroPpm = 40; s2c.cfg.snrDb = 15;
        expectAll("direct mode, +2.5 kHz, 40 ppm, 15 dB", run(s2c), s2c.secs, false);
        Scn s3 = s; s3.secs = 24; s3.cfg.seed = 5; s3.cfg.snrDb = 30; s3.cfg.cc = 7; s3.cfg.traffic = 0;
        const Res r3 = run(s3);
        expectAll("direct mode, colour code 7", r3, s3.secs, false);
        CHECK(r3.tel.cc == 7, "colour code %d", r3.tel.cc);
    }

    // ---- a handset talking to a repeater (the uplink): the mobile station sync patterns, one time slot at a time
    {
        Scn s; s.cfg = base(22); s.cfg.direct = true; s.cfg.mobile = true; s.secs = 30;
        const Res r = run(s);
        expectAll("mobile station (uplink)", r, s.secs, false);
        CHECK(r.tel.link == "mobile", "link '%s'", r.tel.link.c_str());
        CHECK(r.tel.syncCount[2] + r.tel.syncCount[3] > 10, "mobile syncs %llu", (unsigned long long)(r.tel.syncCount[2] + r.tel.syncCount[3]));
        CHECK(r.tel.syncCount[0] + r.tel.syncCount[1] + r.tel.syncCount[5] + r.tel.syncCount[6] + r.tel.syncCount[7] + r.tel.syncCount[8] == 0, "base station or direct mode syncs in a mobile signal");
        Scn s2 = s; s2.secs = 24; s2.cfg.cfoHz = -4000; s2.cfg.sroPpm = -30; s2.cfg.snrDb = 15; s2.cfg.seed = 9;
        expectAll("mobile station, -4 kHz, -30 ppm, 15 dB", run(s2), s2.secs, false);
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
