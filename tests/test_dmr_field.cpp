// DMR receiver, conditions of the field: other sample rates, dropouts of the radio's stream, a gain step, an adjacent channel as strong as or
// stronger than the wanted one, impulse noise and a reset in the middle. Compared with what the generator says it sent (dmr_eval.h). The basics are
// in tests/test_dmr_rx.cpp, handsets (direct mode, mobile station) in tests/test_dmr_handset.cpp.
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
    // ---- other sample rates (HackRF: 2, 4, 8, 10, 12.5, 16, 20 Msps; others 1, 2.048, 3.2 ...)
    for (double rate : {1.0e6, 2.048e6, 4e6, 10e6, 20e6}) {
        Scn s; s.cfg = base(25); s.cfg.rate = rate; s.secs = 8; s.skip = 1.0;
        const Res r = run(s);
        char w[64]; snprintf(w, sizeof w, "sample rate %.3f Msps", rate / 1e6);
        expectAll(w, r, s.secs);
    }

    // ---- a dropout of 6 ms (a stalled USB transfer) every 3 s, and one of 40 ms every 4 s
    {
        Scn s; s.cfg = base(25); s.secs = 14;
        s.mod = [](cf32* x, size_t n, size_t first, double rate) {
            for (size_t i = 0; i < n; i++) if (std::fmod((double)(first + i) / rate, 3.0) < 0.006) x[i] = cf32(0, 0);
        };
        const Res r = run(s);
        show("6 ms dropout every 3 s", r, s.secs);
        CHECK(r.tel.state == 2, "not locked after dropouts");
        CHECK(r.sc.voiceFound >= r.sc.voiceSent - 2 && r.sc.falseCalls <= 1 && r.sc.idsWrong <= 1, "dropouts: %d of %d calls, %d wrong, %d false\n%s", r.sc.voiceFound, r.sc.voiceSent, r.sc.idsWrong, r.sc.falseCalls, r.sc.report.c_str());
        Scn s2; s2.cfg = base(25); s2.secs = 14;
        s2.mod = [](cf32* x, size_t n, size_t first, double rate) {
            for (size_t i = 0; i < n; i++) if (std::fmod((double)(first + i) / rate, 4.0) < 0.040) x[i] = cf32(0, 0);
        };
        const Res r2 = run(s2);
        show("40 ms dropout every 4 s", r2, s2.secs);
        CHECK(r2.tel.state == 2 && r2.sc.falseCalls <= 1 && r2.sc.voiceFound >= r2.sc.voiceSent - 3, "after long dropouts: state %d, %d false, %d of %d calls", r2.tel.state, r2.sc.falseCalls, r2.sc.voiceFound, r2.sc.voiceSent);
    }

    // ---- a level step (the radio's gain changes): 20 dB down at 5 s
    {
        Scn s; s.cfg = base(25); s.secs = 12;
        s.mod = [](cf32* x, size_t n, size_t first, double rate) {
            for (size_t i = 0; i < n; i++) if ((double)(first + i) / rate > 5.0) x[i] *= 0.1f;
        };
        const Res r = run(s);
        show("gain step -20 dB at 5 s", r, s.secs);
        CHECK(r.sc.voiceFound >= r.sc.voiceSent - 1 && r.sc.falseCalls == 0, "gain step: %d of %d calls", r.sc.voiceFound, r.sc.voiceSent);
    }

    // ---- an adjacent channel (another base station 12.5 kHz up) as strong as the wanted signal, and 10 dB stronger
    for (double rel : {0.0, 10.0}) {
        Scn s; s.cfg = base(25); s.secs = 12;
        DmrGenConfig ic = s.cfg; ic.cfoHz = 12500; ic.seed = 77; ic.rms = 0.11 * std::pow(10.0, rel / 20.0); ic.snrDb = 40;
        auto interferer = std::make_shared<DmrSignal>(ic);
        auto tmp = std::make_shared<std::vector<cf32>>();
        s.mod = [interferer, tmp](cf32* x, size_t n, size_t, double) {
            tmp->resize(n);
            interferer->generate(tmp->data(), n);
            for (size_t i = 0; i < n; i++) x[i] = x[i] * 0.5f + (*tmp)[i];
        };
        const Res r = run(s);
        char w[64]; snprintf(w, sizeof w, "adjacent channel %+.0f dB", rel);
        show(w, r, s.secs);
        CHECK(r.sc.voiceFound >= r.sc.voiceSent - 1 && r.sc.idsWrong <= 1 && r.sc.falseCalls <= 1, "%s: %d of %d calls, %d wrong, %d false\n%s", w, r.sc.voiceFound, r.sc.voiceSent, r.sc.idsWrong, r.sc.falseCalls, r.sc.report.c_str());
    }

    // ---- impulse noise: 100 us bursts well above the signal, 20 per second
    {
        Scn s; s.cfg = base(25); s.secs = 12;
        auto rng = std::make_shared<std::mt19937>(5);
        auto left = std::make_shared<size_t>(0);
        s.mod = [rng, left](cf32* x, size_t n, size_t, double rate) {
            std::normal_distribution<float> nd(0.f, 0.35f);
            const size_t len = (size_t)(100e-6 * rate), gap = (size_t)(rate / 20.0);
            for (size_t i = 0; i < n; i++) {
                if (*left == 0 && (*rng)() % gap == 0) *left = len;
                if (*left) { x[i] += cf32(nd(*rng), nd(*rng)); (*left)--; }
            }
        };
        const Res r = run(s);
        show("impulse noise 20/s", r, s.secs);
        CHECK(r.sc.voiceFound >= r.sc.voiceSent - 2 && r.sc.falseCalls <= 1 && r.sc.idsWrong <= 1, "impulse noise: %d of %d calls, %d wrong, %d false\n%s", r.sc.voiceFound, r.sc.voiceSent, r.sc.idsWrong, r.sc.falseCalls, r.sc.report.c_str());
    }

    // ---- reset in the middle: forgets everything, finds the signal again; the sequence number only grows
    {
        Scn s; s.cfg = base(25); s.cfg.cfoHz = -7000; s.secs = 16; s.resetAt = 7.0; s.skip = 9.0;      // 7 kHz off: the carrier search has to find it again
        const Res r = run(s);
        show("reset at 7 s, carrier -7 kHz", r, s.secs);
        CHECK(std::fabs(r.tel.cfoHz + 7000) < 100, "carrier offset %.0f Hz after the reset", r.tel.cfoHz);
        CHECK(r.seqOk, "telemetry sequence restarted after reset");
        CHECK(r.tel.state == 2, "not locked after reset");
        CHECK(r.sc.voiceSent >= 2 && r.sc.voiceFound >= r.sc.voiceSent - 1 && r.sc.falseCalls == 0, "after the reset: %d of %d calls found\n%s", r.sc.voiceFound, r.sc.voiceSent, r.sc.report.c_str());
        CHECK(r.tel.calls < 40, "the call counter did not restart: %llu", (unsigned long long)r.tel.calls);
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
