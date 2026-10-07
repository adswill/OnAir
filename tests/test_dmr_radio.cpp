// DMR receiver and the radio in front of it: carrier offset (the sync tracking and the coarse carrier search, and the neighbouring channel that must
// not be taken for the wanted one), clock offset of the sample clock, 8 bit rounding, DC spike, IQ imbalance, a transmitter with the wrong deviation.
// Everything is compared with what the generator says it sent (dmr_eval.h). The other receiver tests are tests/test_dmr_rx.cpp and test_dmr_field.cpp.
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
    // ---- 3. carrier offset (a HackRF can be 20 ppm off: 9 kHz at 446 MHz). Up to about 6 kHz the sync tracking follows by itself; the coarse
    // carrier search takes it from there to 9.5 kHz. The neighbouring channel (12.5 kHz away) must not be taken for the wanted one.
    for (double cfo : {-9000.0, -5000.0, -3000.0, 800.0, 5000.0, 7500.0, 9500.0}) {
        Scn s; s.cfg = base(20); s.cfg.cfoHz = cfo; s.secs = 12;
        const Res r = run(s);
        char w[64]; snprintf(w, sizeof w, "carrier offset %+.0f Hz", cfo);
        expectAll(w, r, s.secs);
        CHECK(std::fabs(r.tel.cfoHz - cfo) < 100, "%s: reported offset %.0f Hz", w, r.tel.cfoHz);
    }

    {
        Scn s; s.cfg = base(25); s.cfg.cfoHz = 8000; s.cfg.snrDb = 10; s.secs = 12;       // far off and weak: the search works on the spectrum, not on the sync
        const Res r = run(s);
        show("carrier +8 kHz at 10 dB", r, s.secs);
        CHECK(r.sc.voiceFound >= r.sc.voiceSent - 1 && r.sc.falseCalls == 0 && std::fabs(r.tel.cfoHz - 8000) < 100, "+8 kHz at 10 dB: %d of %d calls, %d false, offset %.0f", r.sc.voiceFound, r.sc.voiceSent, r.sc.falseCalls, r.tel.cfoHz);
        for (double cfo : {12500.0, -12500.0}) {
            Scn n; n.cfg = base(30); n.cfg.cfoHz = cfo; n.secs = 8;
            const Res rn = run(n);
            char w[64]; snprintf(w, sizeof w, "neighbour channel %+.0f Hz", cfo);
            show(w, rn, n.secs);
            CHECK(rn.tel.state == 0 && rn.tel.calls == 0 && rn.tel.slotsLocked == 0 && rn.tel.blocksOk == 0, "%s was decoded: state %d, %llu calls", w, rn.tel.state, (unsigned long long)rn.tel.calls);
        }
    }

    // ---- 4. clock offset of the radio
    for (double ppm : {-100.0, 100.0}) {
        Scn s; s.cfg = base(20); s.cfg.sroPpm = ppm; s.secs = 12;
        const Res r = run(s);
        char w[64]; snprintf(w, sizeof w, "sample clock %+.0f ppm", ppm);
        expectAll(w, r, s.secs);
        CHECK(std::fabs(r.tel.symbolPpm - (-ppm)) < 30, "%s: reported symbol clock error %.1f ppm", w, r.tel.symbolPpm);
    }

    // ---- 5. the radio: DC spike in the middle of the channel, IQ imbalance, a transmitter with the wrong deviation, a weak signal
    {
        Scn s; s.cfg = base(25); s.cfg.dcOffset = 0.1; s.secs = 12;
        expectAll("DC offset 0.1 full scale", run(s), s.secs);
        Scn s2; s2.cfg = base(25); s2.cfg.iqImbalanceDb = 2; s2.secs = 12;
        expectAll("IQ imbalance 2 dB", run(s2), s2.secs);
        for (double dev : {0.8, 1.2}) {
            Scn s3; s3.cfg = base(25); s3.cfg.devScale = dev; s3.secs = 12;
            char w[64]; snprintf(w, sizeof w, "deviation x%.1f", dev);
            const Res r = run(s3);
            expectAll(w, r, s3.secs);
            CHECK(std::fabs(r.tel.devHz - 1944 * dev) < 120, "%s: measured %.0f Hz", w, r.tel.devHz);
        }
        // a signal far above the converter's range: the 8 bit samples are clipped at +-1 (a hard limited FM signal still carries its frequency)
        Scn s5; s5.cfg = base(25); s5.secs = 12;
        s5.mod = [](cf32* x, size_t n, size_t, double) { for (size_t i = 0; i < n; i++) x[i] *= 8.f; };
        const Res r5 = run(s5);
        show("clipped (+18 dB over full scale)", r5, s5.secs);
        CHECK(r5.sc.voiceFound >= r5.sc.voiceSent - 1 && r5.sc.falseCalls == 0 && r5.sc.idsWrong == 0, "clipped signal: %d of %d calls, %d false, %d wrong", r5.sc.voiceFound, r5.sc.voiceSent, r5.sc.falseCalls, r5.sc.idsWrong);
        Scn s4; s4.cfg = base(25); s4.cfg.rms = 0.012; s4.secs = 12;     // about -38 dBFS: the 8 bit rounding is a large part of what arrives
        const Res r4 = run(s4);
        show("-38 dBFS (8 bit rounding)", r4, s4.secs);
        CHECK(r4.sc.voiceFound >= r4.sc.voiceSent - 1 && r4.sc.falseCalls == 0, "weak signal: %d of %d calls", r4.sc.voiceFound, r4.sc.voiceSent);
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
