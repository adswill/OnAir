// nRF24L01 through the engine (setStandard(211)): the synthetic source plays Enhanced ShockBurst frames, the receiver reports through latestRx().
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = nrf24Tuning();
    CHECK(mt.stdMode == 211 && modeTuningById("nrf24") && modeTuningById("nrf24")->stdMode == 211 && modeTuning(211), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 211;
    t.synth.snrDb = 16;
    t.synth.modeOpt[0] = 3;   // all three data rates
    t.synth.modeOpt[1] = 0;
    FileOptions fo;
    e.setStandard(211);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 210, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 210) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const Nrf24Telemetry& s = rx.nrf24;
    printf("  %d reports: %s\n", reports, nrf24Summary(s).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    for (const auto& d : s.devices) printf("    %s (%d bytes) %d k crc%d x%llu\n", d.address.c_str(), d.addrLen, d.rateKbps, d.crcBits, (unsigned long long)d.count);
    for (const auto& f : s.frames) if (f.crcBits == 8) printf("    crc8 frame %s len %d pid %d payload %s rate %d x%d snr %.0f\n", f.address.c_str(), f.payloadLen, f.pid, f.payloadHex.c_str(), f.rateKbps, f.repeats, f.snrDb);
    size_t sure = 0;
    for (const auto& d : s.devices) if (d.crcBits == 16) sure++;
    CHECK(s.blocksOk >= 10 && sure == 4 && s.devices.size() <= 5, "%llu frames, %zu addresses", (unsigned long long)s.blocksOk, s.devices.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("nrf24 engine");
}
