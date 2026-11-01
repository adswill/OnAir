// The "why bad" counters of the HFDL receiver: every failed burst is counted under exactly one reason, a clean burst under none.
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 48000, kCentre = 8.935e6, kCh = 8.927e6;

static HfdlTelemetry run(int m1, double snrDb, uint32_t seed) {
    HfdlReceiver rx;
    rx.configure(kRate); rx.setCenterHz(kCentre); rx.setChannels({kCh});
    const std::vector<uint8_t> pdu = hfdlMpduDown(1, 23, {hfdlUnnumbered(hfdlAcarsHfnpdu("N1UA", "H1", '2', "WHY TEST", true))});
    HfdlWave w = hfdlMakeWave(m1, pdu, {}, seed);
    w.start = 0.3 * kRate; w.carHz = kCh + kHfdlCarrierHz - kCentre; w.amp = 0.1f; w.phase = 0.3;
    PortableRng rng(seed);
    const double sigma = std::sqrt(0.01 / std::pow(10.0, snrDb / 10) * (kRate / kHfdlBaud) / 2);
    std::vector<cf32> buf(4096);
    for (int64_t pos = 0; pos < (int64_t)((0.3 + hfdlBurstSeconds(m1) + 0.8) * kRate); pos += 4096) {
        for (auto& v : buf) v = cf32((float)rng.gauss(sigma), (float)rng.gauss(sigma));
        hfdlRender(w, buf.data(), pos, 4096, kRate);
        rx.feed(buf.data(), 4096);
    }
    HfdlTelemetry t; rx.telemetry(t, ~0ull);
    return t;
}

int main() {
    const HfdlTelemetry c = run(1, 20, 5);
    CHECK(c.blocksOk == 1 && c.blocksBad == 0, "clean burst: %llu ok %llu bad", (unsigned long long)c.blocksOk, (unsigned long long)c.blocksBad);
    CHECK(c.whySpduFcs + c.whyMpduFcs + c.whyShort + c.whyLpduFcs == 0, "clean burst has a failure reason");
    uint64_t bad = 0, reasons = 0;
    for (uint32_t i = 0; i < 10; i++) {
        const HfdlTelemetry t = run(3, 2.0, 20 + i);   // 1800 bit/s at 2 dB: most bursts fail
        bad += t.blocksBad; reasons += t.whySpduFcs + t.whyMpduFcs + t.whyShort;
    }
    printf("  1800 bit/s at 2 dB: %llu bad bursts, %llu reasons\n", (unsigned long long)bad, (unsigned long long)reasons);
    CHECK(bad > 0, "no failed burst to count");
    CHECK(bad == reasons, "%llu bad bursts but %llu reasons", (unsigned long long)bad, (unsigned long long)reasons);
    return testSummary("hfdl why");
}
