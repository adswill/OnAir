// DMR through the engine, as the GUI and dect2cli use it: the built-in synthetic source plays the DMR test signal (SynthConfig::mode = 11), the engine
// runs the receiver on its analysis thread and publishes RxTelemetry. A base station for 12 s and direct mode for 10 s; the telemetry is read from this
// thread while the analysis thread feeds, a retune in between must reset the receiver without the sequence number going back, and no samples may be
// dropped.
#include "dect2/engine.h"
#include "dect2/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static TuneSettings makeTune(bool direct) {
    TuneSettings tune;
    const ModeTuning* mt = modeTuningById("dmr");
    tune.centerHz = mt->defMhz * 1e6;
    tune.sampleRate = mt->sampleRate;
    tune.bandwidthMhz = mt->bandwidthMhz;
    tune.basebandFilterHz = mt->basebandHz;
    tune.synth.mode = mt->stdMode;            // the synthetic source plays this mode's test signal
    tune.synth.snrDb = 25;
    tune.synth.modeOpt[2] = direct ? 1 : 0;
    tune.synth.modeOpt[3] = 2;                // busy
    tune.synth.cfoHz = 1500;
    return tune;
}

int main() {
    const ModeTuning* mt = modeTuningById("dmr");
    CHECK(mt && mt->stdMode == 11 && modeTuning(11) == mt, "tuning table");
    if (!mt) return 1;
    CHECK(std::string(mt->name) == "DMR" && mt->minSampleRate <= mt->sampleRate && mt->bandwidthMhz > 0.012 && mt->bandwidthMhz < 0.013, "tuning table values");

    for (int direct = 0; direct < 2; direct++) {
        Engine e;
        DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
        TuneSettings tune = makeTune(direct != 0);
        FileOptions fo;
        e.setStandard(11);
        CHECK(e.start(dev, tune, fo), "engine started");
        CHECK(e.activeStandard() == 10, "DMR is the active standard (%d)", e.activeStandard());
        const double secs = direct ? 10 : 12;
        RxTelemetry t, last;
        uint64_t seq = 0, calls = 0, maxCalls = 0;
        bool locked = false, seqOk = true, retuned = false;
        uint64_t lastSeq = 0;
        size_t maxLog = 0, maxMsg = 0;
        float maxSnr = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (e.latestRx(t, seq)) {
                seq = t.seq;
                if (t.standard == 10) {
                    if (t.dmr.seq <= lastSeq && lastSeq != 0) seqOk = false;
                    lastSeq = t.dmr.seq;
                    last = t;
                    locked |= t.dmr.state == 2;
                    maxSnr = std::max(maxSnr, t.dataSnrDb);
                    calls = t.dmr.calls;
                    maxCalls = std::max(maxCalls, calls);
                    maxLog = std::max(maxLog, t.dmr.callLog.size());
                    maxMsg = std::max(maxMsg, t.dmr.messages.size());
                }
            }
            // the interface also reads the receiver directly, from this thread while the analysis thread feeds
            DmrTelemetry d;
            e.dmr().telemetry(d, 0);
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!direct && !retuned && el > 7.0) {
                CHECK(e.retuneReset(tune), "retune");
                retuned = true;
                maxCalls = 0;
            }
        }
        e.latestRx(t, 0);
        const uint64_t dropped = e.droppedSamples();
        size_t total = 0;
        const auto log = e.logSnapshot(total);
        bool sawCall = false, sawSignal = false;
        for (const auto& l : log) {
            if (l.find("call from") != std::string::npos) sawCall = true;
            if (l.find("signal:") != std::string::npos) sawSignal = true;
        }
        e.stop();
        printf("%s: state %d, SNR %.1f dB, CFO %+.0f Hz, calls %llu (peak %llu), log %zu entries, messages %zu, blocks %llu ok / %llu bad, dropped %llu samples\n", direct ? "direct mode" : "base station",
               last.dmr.state, last.dataSnrDb, last.cfoHz, (unsigned long long)calls, (unsigned long long)maxCalls, maxLog, maxMsg, (unsigned long long)last.blocksOk, (unsigned long long)last.blocksBad,
               (unsigned long long)dropped);
        printf("  %s\n", dmrSummary(last.dmr).c_str());
        CHECK(locked, "the receiver never locked");
        CHECK(last.standard == 10 && last.rateOk, "standard %d rateOk %d", last.standard, (int)last.rateOk);
        // a handset in direct mode is silent between its calls, so the end state of that run says nothing: it must have locked at some point
        if (!direct) CHECK(last.dmr.state == 2 && last.dataValid, "state %d dataValid %d at the end", last.dmr.state, (int)last.dataValid);
        CHECK(last.state == last.dmr.state && last.blocksOk == last.dmr.blocksOk && last.blocksBad == last.dmr.blocksBad, "the engine's summary fields do not match the telemetry");
        CHECK(std::fabs(last.cfoHz - 1500) < 100, "carrier offset %.0f Hz (sent 1500)", last.cfoHz);
        CHECK(maxSnr > 20 && maxSnr < 40, "SNR %.1f dB at best (the symbol SNR of a 25 dB signal)", maxSnr);
        if (!direct) CHECK(last.dataSnrDb > 20, "SNR %.1f dB at the end", last.dataSnrDb);
        CHECK(last.dmr.cc == 1, "colour code %d", last.dmr.cc);
        CHECK(last.blocksOk > 60, "only %llu blocks decoded", (unsigned long long)last.blocksOk);
        CHECK(maxCalls >= (direct ? 3u : 4u), "only %llu calls seen", (unsigned long long)maxCalls);
        CHECK(maxLog >= (direct ? 3u : 4u), "call log of %zu entries", maxLog);
        CHECK(seqOk, "telemetry sequence went backwards");
        CHECK(dropped == 0, "%llu samples dropped", (unsigned long long)dropped);
        CHECK(sawSignal && sawCall, "the engine log has no signal / call lines (%zu lines)", total);
        CHECK(last.dmr.link == (direct ? "direct mode" : "base station"), "link '%s'", last.dmr.link.c_str());
        if (!direct) CHECK(maxMsg >= 1, "no text message received");
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
