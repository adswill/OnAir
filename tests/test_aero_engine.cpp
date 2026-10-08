// Inmarsat Aero through the engine as the interface and dect2cli use it: the synthetic source plays the test signal (8-bit samples, tuned
// tuneOffsetHz above the user's frequency), the engine must find both P channels, decode SUs, logons and ACARS messages, keep every sample
// and report through latestRx(). A retune empties the tables and the report numbers go on growing.
#include "dect2/engine.h"
#include "dect2/aero_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static size_t telemetryBytes(const AeroTelemetry& t) {
    size_t b = sizeof t + t.channels.size() * sizeof(AeroChannelInfo) + t.logons.size() * sizeof(AeroLogonEntry);
    for (const auto& s : t.suTypes) b += sizeof s + s.name.size();
    for (const auto& m : t.messages) b += sizeof m + m.text.size() + m.labelText.size() + m.registration.size() + m.flight.size();
    for (const auto& a : t.aircraft) b += sizeof a + a.lastText.size() + a.registration.size() + a.flight.size();
    return b;
}

int main() {
    const double rate = 2e6;
    const double pace = SANITIZED ? 0.3 : std::getenv("CI") ? 1.0 : 2.0;
    const double signalSecs = SANITIZED ? 10 : 24;
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    const ModeTuning* mt = modeTuning(20);
    CHECK(mt && mt->tuneOffsetHz == 50000, "tuning entry");
    t.centerHz = mt->defMhz * 1e6;
    t.bandwidthMhz = mt->bandwidthMhz;
    t.sampleRate = rate;
    t.synth.mode = 20;
    t.synth.pace = pace;
    FileOptions fo;
    e.setStandard(20);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 19, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true;
    double firstDecode = -1;
    int reports = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const double wall = signalSecs / pace;
    size_t maxBytes = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < wall) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            CHECK(rx.standard == 19, "report standard %d", rx.standard);
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * pace;
            if (rx.state == 2 && firstDecode < 0) firstDecode = now;
            maxBytes = std::max(maxBytes, telemetryBytes(rx.aero));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.latestRx(rx, 0);
    const AeroTelemetry& a = rx.aero;
    printf("engine, 2 Msps, %.0f s of signal (pace %.1f): %d reports, decoding after %.1f s\n  %s\n  dropped samples %llu, report size up to %zu bytes\n", signalSecs, pace,
           reports, firstDecode, aeroSummary(a).c_str(), (unsigned long long)e.droppedSamples(), maxBytes);
    for (const auto& c : a.channels)
        printf("  channel %+9.1f Hz %5d bit/s state %d Eb/N0 %.1f dB frames %llu SUs %llu/%llu\n", c.offsetHz, c.bitRate, c.state, c.ebn0Db, (unsigned long long)c.frames,
               (unsigned long long)c.susOk, (unsigned long long)c.susBad);
    CHECK(seqOk && reports > signalSecs * 2, "report numbers: %d reports", reports);   // 4 a second of signal
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    CHECK(rx.state == 2 && rx.dataValid && firstDecode > 0 && firstDecode < 8, "decoding after %.1f s, state %d", firstDecode, rx.state);
    CHECK(rx.blocksOk == a.blocksOk && rx.blocksBad == a.blocksBad && a.blocksOk > 0 && a.blocksBad == 0, "SUs %llu good %llu bad", (unsigned long long)a.blocksOk,
          (unsigned long long)a.blocksBad);
    // the default test signal: 10500 bit/s at -50 kHz and 1200 bit/s at +150 kHz from the user's frequency, each a few kHz off
    int found = 0;
    for (const auto& c : a.channels) {
        if (c.state == 3 && c.bitRate == 10500 && std::fabs(c.offsetHz + 50000) < 4000) found++;
        if (c.state == 3 && c.bitRate == 1200 && std::fabs(c.offsetHz - 150000) < 4000) found++;
    }
    CHECK(found == 2, "%d of 2 channels", found);
    CHECK(std::fabs(rx.cfoHz - a.cfoHz) < 1e-9 && std::fabs(a.cfoHz + 50000) < 4000 && a.snrDb > 10 && a.snrDb < 14, "carrier %.0f Hz, Eb/N0 %.1f", a.cfoHz, a.snrDb);
    if (!SANITIZED) {
        CHECK(a.messagesTotal >= 4 && a.logonsTotal >= 6 && a.aircraft.size() == 6, "messages %llu logons %llu aircraft %zu", (unsigned long long)a.messagesTotal,
              (unsigned long long)a.logonsTotal, a.aircraft.size());
        for (const auto& m : a.messages) CHECK(m.crcOk && !m.registration.empty() && !m.label.empty(), "message fields");
    }
    CHECK(maxBytes < 100000, "report size %zu", maxBytes);
    // a retune empties the tables, and the report numbers go on growing
    TuneSettings t2 = t;
    t2.centerHz += 1e6;
    const uint64_t before = rx.seq, okBefore = a.blocksOk;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(800 / pace)));
    e.latestRx(rx, 0);
    CHECK(rx.seq > before, "report numbers after the retune: %llu after %llu", (unsigned long long)rx.seq, (unsigned long long)before);
    CHECK(rx.aero.messages.empty() && rx.aero.blocksOk < okBefore, "tables after the retune: %zu messages, %llu SUs", rx.aero.messages.size(),
          (unsigned long long)rx.aero.blocksOk);
    e.stop();
    printf(fails ? "aero engine: %d FAILED\n" : "aero engine: all passed\n", fails);
    return fails ? 1 : 0;
}
