// CDR through the engine (setStandard(24)): the synthetic source plays the default test signal (transmission mode 1, spectrum mode 1,
// QPSK 3/4) at 25 dB; the telemetry that latestRx() reports must show the multiplex: network, three services, text, no LDPC failures.
// Real time, at most about 6 s.
#include "dect2/engine.h"
#include "dect2/cdr_gen.h"
#include "dect2/cdr_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning mt = cdrTuning();
    CHECK(mt.stdMode == 24 && modeTuningById("cdr") && modeTuningById("cdr")->stdMode == 24, "tuning table");
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 24;
    t.synth.snrDb = 25;
    t.synth.cfoHz = 800;
    FileOptions fo;
    e.setStandard(24);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 23, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            reports++;
            if (rx.standard != 23) wrong++;
            if (rx.cdr.services.size() == 3 && !rx.cdr.services[2].text.empty() && rx.cdr.muxOk >= 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.stop();
    const CdrTelemetry& c = rx.cdr;
    printf("CDR: %d reports, %s\n", reports, cdrSummary(c).c_str());
    CHECK(wrong == 0, "%d reports with another standard", wrong);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(c.state == 3 && c.tm == 1 && c.sm == 1 && c.rate == 3 && c.msdMod == 0, "state %d mode %d/%d rate %d", c.state, c.tm, c.sm, c.rate);
    CHECK(c.network == "OnAir CDR" && c.services.size() == 3, "network '%s', %zu services", c.network.c_str(), c.services.size());
    if (c.services.size() == 3) {
        CHECK(c.services[0].id == 0x1001 && c.services[1].id == 0x1002 && c.services[2].id == 0x1003, "service ids");
        CHECK(c.services[2].text == CdrTxConfig().text, "text '%s'", c.services[2].text.c_str());
    }
    CHECK(c.blocksOk > 0 && c.blocksBad == 0, "LDPC %llu ok %llu bad", (unsigned long long)c.blocksOk, (unsigned long long)c.blocksBad);
    CHECK(rx.blocksOk == c.blocksOk && rx.state == c.state, "generic members copied");
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
