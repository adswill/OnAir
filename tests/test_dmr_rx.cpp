// DMR receiver: generator -> receiver on simulated radio signals. This file: a clean signal in detail, noise, odd chunk sizes, no signal at all,
// damaged input. tests/test_dmr_radio.cpp: carrier offset, clock offset, 8 bit rounding, DC spike, IQ imbalance, wrong deviation.
// tests/test_dmr_field.cpp: other sample rates, direct mode, dropouts, level steps, adjacent channels, impulse noise, reset.
// Everything is compared with what the generator says it sent (dmr_eval.h).
#include "dect2/dmr_eval.h"
#include "dect2/dmr_gen.h"
#include "dect2/dmr_rx.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// The speed checks mean nothing under a sanitiser (the code runs several times slower there)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
static const bool kSanitised = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
static const bool kSanitised = true;
#else
static const bool kSanitised = false;
#endif
#else
static const bool kSanitised = false;
#endif

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

// ---- 9. the interface reads the telemetry and sets the controls and callbacks from its own thread while feed() runs (also run alone with the
// argument "threads", for the thread sanitiser)
static void testThreads() {
    {
        DmrReceiver rx;
        rx.setSilent(true);
        rx.configure(2.4e6);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> voiceCalls{0}, logLines{0}, polls{0}, newer{0};
        std::thread ui([&]() {
            uint64_t seq = 0;
            for (int k = 0; !stop; k++) {
                DmrTelemetry t;
                if (rx.telemetry(t, seq)) { seq = t.seq; newer++; }
                rx.setVolume((float)(k % 10) / 10.f);
                rx.setMuted(k & 1);
                rx.setSilent(true);
                if (k % 7 == 0) rx.setVoiceCallback([&](int, int, const uint8_t*) { voiceCalls++; });
                if (k % 11 == 0) rx.setLogCallback([&](const std::string&) { logLines++; });
                if (k % 13 == 0) rx.setAudioTap([](const float*, const float*, size_t) {});
                polls++;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
        DmrSignal sig(base(25));
        std::vector<cf32> g(65536);
        for (int blk = 0; blk < 110; blk++) { sig.generate(g.data(), g.size()); rx.feed(g.data(), g.size()); }      // 3 s
        stop = true;
        ui.join();
        DmrTelemetry t;
        rx.telemetry(t, 0);
        printf("%-36s %llu polls, %llu new reports, %llu voice bursts handed on, %llu log lines, state %d\n", "controls from another thread", (unsigned long long)polls.load(), (unsigned long long)newer.load(),
               (unsigned long long)voiceCalls.load(), (unsigned long long)logLines.load(), t.state);
        // (reports come at intervals of real time: a fast machine feeds the 3 s of signal in a few of them, a Windows runner gave exactly 3)
        CHECK(t.state == 2 && newer >= 2 && voiceCalls > 10 && logLines > 0, "state %d, %llu reports, %llu voice bursts, %llu log lines", t.state, (unsigned long long)newer.load(), (unsigned long long)voiceCalls.load(), (unsigned long long)logLines.load());
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "threads")) {
        testThreads();
        if (fails) { printf("%d check(s) failed\n", fails); return 1; }
        printf("OK\n");
        return 0;
    }
    // ---- 1. a clean base station: everything, in detail
    {
        Scn s; s.cfg = base(30); s.secs = 16;
        const Res r = run(s);
        expectAll("clean 2.4 Msps", r, s.secs);
        const DmrTelemetry& t = r.tel;
        CHECK(t.state == 2 && t.dataValid, "state %d", t.state);
        CHECK(t.cc == 1, "colour code %d", t.cc);
        CHECK(t.link == "base station", "link '%s'", t.link.c_str());
        CHECK(t.slotsLocked == 2, "%d slots locked", t.slotsLocked);
        CHECK(t.blocksOk > 100 && t.blocksBad == 0, "FEC blocks ok %llu bad %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.idleOk > 10, "idle bursts verified %llu", (unsigned long long)t.idleOk);
        CHECK(t.syncCount[0] > 10 && t.syncCount[1] > 10 && t.syncCount[2] == 0 && t.syncCount[3] == 0 && t.syncCount[4] == 0 && t.syncCount[5] == 0 && t.syncCount[6] == 0,
              "sync counts BS voice %llu data %llu, others %llu %llu %llu", (unsigned long long)t.syncCount[0], (unsigned long long)t.syncCount[1], (unsigned long long)t.syncCount[2],
              (unsigned long long)t.syncCount[3], (unsigned long long)t.syncCount[5]);
        CHECK(t.burstCount[1] > 0 && t.burstCount[2] > 0 && t.burstCount[3] > 0 && t.burstCount[6] > 0 && t.burstCount[7] + t.burstCount[8] + t.burstCount[10] > 0, "data burst types seen");
        CHECK(t.rsOk > 0 && t.embLcOk > 0 && t.trellisOk > 0 && t.crcOk > 0, "FEC counters rs %llu embLc %llu trellis %llu crc %llu", (unsigned long long)t.rsOk, (unsigned long long)t.embLcOk,
              (unsigned long long)t.trellisOk, (unsigned long long)t.crcOk);
        CHECK(std::fabs(t.devHz - 1944) < 60, "deviation %.0f Hz (sent 1944)", t.devHz);
        CHECK(std::fabs(t.symbolPpm) < 20, "symbol clock %.1f ppm", t.symbolPpm);
        CHECK(t.snrDb > 25 && t.ber < 1e-4, "SNR %.1f BER %.5f", t.snrDb, t.ber);
        CHECK(t.eye.size() == 800 && t.spectrumDb.size() == 128, "eye %zu spectrum %zu", t.eye.size(), t.spectrumDb.size());
        if (t.spectrumDb.size() == 128) {   // bin b covers -12 + 0.1875 b kHz: the signal's power sits in the middle, inside the 12.5 kHz channel
            double sp = 0, sf = 0, in = 0;
            for (int b = 0; b < 128; b++) {
                const double f = -12.0 + 0.1875 * (b + 0.5), p = std::pow(10.0, t.spectrumDb[(size_t)b] / 10);
                sp += p; sf += p * f; if (std::fabs(f) < 6.25) in += p;
            }
            CHECK(std::fabs(sf / sp) < 0.5 && in > 0.95 * sp, "channel spectrum centred at %+.2f kHz, %.0f%% inside the channel", sf / sp, 100 * in / sp);
        }
        CHECK(t.levelDbfs > -30 && t.levelDbfs < -5, "level %.1f dBFS", t.levelDbfs);
        CHECK(t.cnrDb > 25, "CNR %.1f dB (30 dB sent)", t.cnrDb);
        CHECK(!t.cachInfo.empty(), "no CACH short LC");
        CHECK(t.callLog.size() >= 10 && t.callLog.size() <= 100, "call log %zu entries", t.callLog.size());
        CHECK(t.messages.size() >= 2, "%zu text messages", t.messages.size());
        // the four levels of the eye sit at -3, -1, 1, 3
        double m[4] = {}; int cnt[4] = {};
        for (float v : t.eye) { const int k = v < -2 ? 0 : v < 0 ? 1 : v < 2 ? 2 : 3; m[k] += v; cnt[k]++; }
        const double want[4] = {-3, -1, 1, 3};
        for (int k = 0; k < 4; k++) CHECK(cnt[k] > 50 && std::fabs(m[k] / cnt[k] - want[k]) < 0.15, "eye level %d: mean %.2f over %d symbols", k, cnt[k] ? m[k] / cnt[k] : 0.0, cnt[k]);
        bool s1 = false, s2 = false;
        for (const DmrCall& c : t.callLog) { if (c.slot == 1) s1 = true; if (c.slot == 2) s2 = true; }
        CHECK(s1 && s2, "calls on both slots expected");
        CHECK(kSanitised || r.cpu < s.secs / 5, "CPU %.2f s for %.0f s of signal (needs 5x real time)", r.cpu, s.secs);
        int err = 0;
        for (const DmrCall& c : t.callLog) err += c.fecErrors;
        CHECK(err == 0, "clean signal, but the call log counts %d FEC errors", err);
    }

    // ---- 1b. a base station that leaves the sync pattern out of its voice headers (clause 4.3 allows it): the header must still be read
    {
        Scn s; s.cfg = base(25); s.cfg.headerSync = false; s.secs = 14;
        const Res r = run(s);
        expectAll("voice headers without sync", r, s.secs);
        int late = 0, known = 0;
        for (const DmrCall& c : r.tel.callLog) if (c.kind <= 2 && c.startSec > 2.5) { known++; if (c.lateEntry) late++; }
        CHECK(r.tel.burstCount[1] >= 5 && r.tel.rsOk >= 5, "voice headers read: %llu, Reed-Solomon ok %llu", (unsigned long long)r.tel.burstCount[1], (unsigned long long)r.tel.rsOk);
        CHECK(late == 0 && known >= 4, "%d of %d calls were late entries although their headers were sent", late, known);
        CHECK(r.tel.bptcFail == 0 && r.tel.blocksBad == 0, "damaged blocks: BPTC %llu, all %llu", (unsigned long long)r.tel.bptcFail, (unsigned long long)r.tel.blocksBad);
    }

    // ---- 1c. reverse channel signalling in place of the sync pattern in every second data block (clause 5.1.5): the messages must still arrive whole
    {
        Scn a; a.cfg = base(25); a.secs = 14;
        Scn b = a; b.cfg.blockSync = false;
        const Res ra = run(a), rb = run(b);
        expectAll("data blocks without sync", rb, b.secs);
        CHECK(rb.sc.messagesSent >= 4, "only %d text messages in the run", rb.sc.messagesSent);
        CHECK(rb.tel.syncCount[1] + 3 <= ra.tel.syncCount[1], "the signal still has its data syncs: %llu against %llu", (unsigned long long)rb.tel.syncCount[1], (unsigned long long)ra.tel.syncCount[1]);
    }

    // ---- 1d. hang time: a base station repeats the terminator for seconds after a call; that is one entry in the log, not forty
    {
        Scn s; s.cfg = base(25); s.cfg.hangBursts = 40; s.secs = 24;
        const Res r = run(s);
        expectAll("hang time (40 terminators)", r, s.secs);
        int voiceEntries = 0, allCalls = 0;
        for (const DmrCall& c : r.tel.callLog) if (c.kind <= 2) voiceEntries++;
        for (const DmrTruth& t : r.truth) if (t.kind <= 2 && t.startSec < s.secs) allCalls++;
        CHECK(voiceEntries <= allCalls + 1, "%d voice entries in the log for %d calls sent", voiceEntries, allCalls);
        CHECK(r.tel.calls <= (uint64_t)(r.sc.voiceSent + r.sc.messagesSent + r.sc.controlSent) * 2 + 8, "call counter %llu", (unsigned long long)r.tel.calls);
    }

    // ---- 2. noise: the success rate falls with the signal to noise ratio (in 12.5 kHz)
    {
        double prevBer = -1;
        for (double snr : {20.0, 15.0, 12.0, 10.0, 8.0}) {
            Scn s; s.cfg = base(snr); s.secs = 14;
            const Res r = run(s);
            char w[64]; snprintf(w, sizeof w, "SNR %.0f dB", snr);
            if (snr >= 15) expectAll(w, r, s.secs);
            else {
                show(w, r, s.secs);
                CHECK(r.sc.falseCalls == 0, "%s: %d invented log entries", w, r.sc.falseCalls);
                CHECK(r.sc.idsWrong <= 1, "%s: %d wrong IDs", w, r.sc.idsWrong);
                if (snr >= 10) CHECK(r.sc.voiceFound >= r.sc.voiceSent - 1, "%s: %d of %d voice calls", w, r.sc.voiceFound, r.sc.voiceSent);
                if (snr >= 12) CHECK(r.sc.messagesFound >= r.sc.messagesSent - 1, "%s: %d of %d messages", w, r.sc.messagesFound, r.sc.messagesSent);
            }
            if (snr <= 8) {
                int err = 0;
                for (const DmrCall& c : r.tel.callLog) err += c.fecErrors;
                CHECK(err > 0, "%s: the call log counts no FEC errors although the BER estimate is %.4f", w, r.tel.ber);
            }
            CHECK(r.tel.ber >= prevBer - 1e-4, "BER estimate %.5f fell below the previous %.5f at lower SNR", r.tel.ber, prevBer);
            prevBer = r.tel.ber;
        }
    }

    // ---- 6. no signal: nothing may be reported, and the CPU stays low
    {
        std::mt19937 rng(8);
        std::normal_distribution<float> nd(0.f, 0.15f);
        DmrReceiver rx;
        rx.setSilent(true);
        rx.configure(2.4e6);
        std::vector<cf32> buf(65536);
        double cpu = 0;
        for (int blk = 0; blk < 150; blk++) {         // 4 s
            for (auto& v : buf) v = cf32(std::round(nd(rng) * 128) / 128, std::round(nd(rng) * 128) / 128);
            const auto t0 = std::chrono::steady_clock::now();
            rx.feed(buf.data(), buf.size());
            cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        DmrTelemetry t;
        CHECK(rx.telemetry(t, 0), "no telemetry from a receiver with no signal");
        uint64_t syncs = 0;
        for (int i = 0; i < 9; i++) syncs += t.syncCount[i];
        CHECK(t.state == 0 && !t.dataValid && t.calls == 0 && t.callLog.empty() && t.slotsLocked == 0, "noise reported as a signal: state %d calls %llu", t.state, (unsigned long long)t.calls);
        printf("%-36s state %d, syncs accepted %llu, CPU %.2f s for 4 s of noise\n", "noise only", t.state, (unsigned long long)syncs, cpu);
        CHECK(t.blocksOk == 0 && syncs == 0, "noise decoded: %llu blocks, %llu syncs", (unsigned long long)t.blocksOk, (unsigned long long)syncs);
        CHECK(kSanitised || cpu < 4.0 / 5, "idle search costs %.2f s for 4 s", cpu);
    }

    // ---- 7. chunk sizes: the result does not depend on how the samples arrive (a run of 10 s at 1 Msps; every count is compared with the 65536 case)
    {
        DmrTelemetry ref;
        const size_t chunks[5] = {65536, 1, 7, 4096, 12345};
        for (size_t chunk : chunks) {
            Scn s; s.cfg = base(20); s.cfg.rate = 1.0e6; s.secs = 10; s.chunk = chunk; s.skip = 1.0;
            const Res r = run(s);
            char w[64]; snprintf(w, sizeof w, "chunks of %zu samples", chunk);
            show(w, r, s.secs);
            CHECK(r.sc.voiceFound == r.sc.voiceSent && r.sc.falseCalls == 0 && r.tel.blocksBad == 0, "%s: %d of %d calls, %d false, %llu bad blocks", w, r.sc.voiceFound, r.sc.voiceSent, r.sc.falseCalls, (unsigned long long)r.tel.blocksBad);
            CHECK(r.sc.voiceSent >= 2, "%s: only %d calls in the run", w, r.sc.voiceSent);
            if (chunk == 65536) ref = r.tel;
            else
                CHECK(ref.blocksOk == r.tel.blocksOk && ref.calls == r.tel.calls && ref.voiceBursts == r.tel.voiceBursts && ref.seq == r.tel.seq && ref.callLog.size() == r.tel.callLog.size() && ref.bptcOk == r.tel.bptcOk,
                      "%s differs from 65536: blocks %llu/%llu, calls %llu/%llu, voice bursts %llu/%llu, seq %llu/%llu", w, (unsigned long long)r.tel.blocksOk, (unsigned long long)ref.blocksOk,
                      (unsigned long long)r.tel.calls, (unsigned long long)ref.calls, (unsigned long long)r.tel.voiceBursts, (unsigned long long)ref.voiceBursts, (unsigned long long)r.tel.seq, (unsigned long long)ref.seq);
        }
    }

    // ---- 8. input that is not a signal: huge values and not-a-number must not break anything
    {
        DmrReceiver rx;
        rx.setSilent(true);
        rx.configure(2.4e6);
        std::vector<cf32> buf(65536, cf32(0, 0));
        buf[100] = cf32(NAN, 0); buf[200] = cf32(INFINITY, 1e30f); buf[300] = cf32(-1e30f, NAN);
        for (int i = 0; i < 4; i++) rx.feed(buf.data(), buf.size());
        DmrSignal sig(base(25));
        std::vector<cf32> g(65536);
        for (int blk = 0; blk < 120; blk++) { sig.generate(g.data(), g.size()); rx.feed(g.data(), g.size()); }
        DmrTelemetry t;
        CHECK(rx.telemetry(t, 0) && t.state == 2 && std::isfinite(t.snrDb) && std::isfinite(t.cfoHz), "receiver did not recover from not-a-number input: state %d", t.state);
        printf("%-36s state %d after NaN and infinite samples\n", "garbage input", t.state);
        // a rate that is too low is refused, not crashed on
        DmrReceiver low;
        low.configure(500e3);
        CHECK(!low.ready(), "500 ksps accepted");
        low.feed(g.data(), 1000);
    }

    testThreads();

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
