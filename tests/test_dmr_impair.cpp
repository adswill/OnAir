// DMR under real-radio faults (REAL_WORLD_CHECKLIST.md): a radio 50 ppm off at 446 MHz (22 kHz), a mirrored spectrum (I and Q swapped), and the
// combined case (offset, +80 ppm clock, an echo, 8-bit clipping) through the real receiver, base station and direct mode. A lone carrier on the
// 12.5 kHz raster stays the neighbouring channel (test_dmr_radio).
#include "dect2/dmr_eval.h"
#include "impair.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
static int fails = 0;

static void check(const char* what, const DmrScenario& s) {
    const DmrRunResult r = dmrRun(s);
    printf("%-44s voice %d/%d msg %d/%d ctrl %d/%d wrong %d false %d | CFO %+6.0f BER %.4f lock %.2f s\n", what, r.sc.voiceFound, r.sc.voiceSent, r.sc.messagesFound,
           r.sc.messagesSent, r.sc.controlFound, r.sc.controlSent, r.sc.idsWrong, r.sc.falseCalls, r.tel.cfoHz, r.tel.ber, r.lockedAt);
    const bool ok = r.sc.voiceFound == r.sc.voiceSent && r.sc.messagesFound == r.sc.messagesSent && r.sc.controlFound == r.sc.controlSent && r.sc.idsWrong == 0 &&
                    r.sc.falseCalls == 0;
    if (!ok) { printf("FAIL: %s\n", what); fails++; }
}

// the sample hook sees one chunk at a time: a phase that runs on from chunk to chunk
static void turn(cf32* x, size_t n, size_t first, double rate, double hz) {
    for (size_t i = 0; i < n; i++) x[i] *= std::polar(1.f, (float)std::fmod(2 * M_PI * hz * (double)(first + i) / rate, 2 * M_PI));
}

int main() {
    for (int direct = 0; direct < 2; direct++) {
        DmrScenario b;
        b.cfg.snrDb = 25; b.cfg.direct = direct; b.secs = 14;
        const char* k = direct ? "direct" : "base";
        char name[96];
        for (double hz : {-15000.0, 22000.0, -22000.0}) {
            DmrScenario s = b;
            s.mod = [hz](cf32* x, size_t n, size_t first, double rate) { turn(x, n, first, rate, hz); };
            snprintf(name, sizeof name, "%s, carrier %+.0f Hz", k, hz);
            check(name, s);
        }
        {
            DmrScenario s = b;
            s.mod = [](cf32* x, size_t n, size_t, double) { std::vector<cf32> v(x, x + n); impair::swapIq(v); std::copy(v.begin(), v.end(), x); };
            snprintf(name, sizeof name, "%s, I/Q swapped", k);
            check(name, s);
        }
        {
            DmrScenario s = b;
            s.cfg.sroPpm = 80;
            s.mod = [](cf32* x, size_t n, size_t first, double rate) {
                turn(x, n, first, rate, -22000);
                std::vector<cf32> v(x, x + n);
                impair::echo(v, 40, -8, 2.0);
                impair::clip8(v, 4);
                std::copy(v.begin(), v.end(), x);
            };
            snprintf(name, sizeof name, "%s, combined -22 kHz +80 ppm echo 8 bit", k);
            check(name, s);
        }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
