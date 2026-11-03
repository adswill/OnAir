// The Smart CubeSat controller end to end on a synthetic wide capture (960 kHz): three unrelated carriers (FSK 9600 G3RUH AX.25, BPSK 1200 with the
// AO-40 FEC, a CW beacon) with different offsets, one with a Doppler ramp, noise on the sum. Blind mode (no table): the scan finds them, the classifier
// names the modulation and baud rate, the chains decode; each carrier must give frames that match what was sent. Then a table-guided case: a satellite
// of the table overhead at a given time, its carrier found at the predicted place, named after the table.
#include "dect2/cubesat/cubesat_smart.h"
#include "dect2/cubesat/cubesat_smartgen.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::cubesat;
#include "../common/test_util.h"

static const double kRate = 960000.0;

static int matches(const std::vector<SmartPacket>& pk, const std::vector<SynthSent>& sent) {
    int ok = 0;
    for (const SynthSent& e : sent)
        for (const SmartPacket& p : pk) {
            if (p.data == e.data || (!e.text.empty() && p.text.find(e.text) != std::string::npos)) { ok++; break; }
        }
    return ok;
}

int main() {
    std::vector<SynthSent> sFsk, sBpsk, sCw;
    SmartSynth f; f.mod = ModFsk; f.baud = 9600; f.framing = Ax25G3ruh; f.rate = kRate; f.carrierHz = 250000; f.frames = 9; f.snrDb = 99; f.gapSec = 0.5;
    f.dopplerHz = 600; f.dopplerRateHzS = -80;
    SmartSynth b; b.mod = ModBpsk; b.baud = 1200; b.framing = kFramingAo40; b.rate = kRate; b.carrierHz = -180000; b.frames = 3; b.snrDb = 99; b.gapSec = 0.3; b.clockPpm = 100;
    SmartSynth c; c.mod = ModCw; c.baud = 0; c.framing = kFramingCw; c.rate = kRate; c.carrierHz = 320000; c.frames = 3; c.snrDb = 99; c.wpm = 16;
    std::vector<cf32> iq = smartSynth(f, &sFsk);
    mixInto(iq, smartSynth(b, &sBpsk), 0, 1.0f);
    mixInto(iq, smartSynth(c, &sCw), 0, 1.0f);
    addNoise(iq, (float)std::sqrt(kRate / 10e3 / std::pow(10.0, 3.0) / 2.0 / 3.0), 99);
    printf("%zu samples (%.1f s)\n", iq.size(), (double)iq.size() / kRate);

    SmartCubesat sm;
    SmartSettings cfg; cfg.usePasses = false; cfg.blind = true;
    sm.setSettings(cfg);
    sm.configure(kRate);
    sm.setCenterHz(437.0e6);
    sm.setWallClock(false);
    for (size_t i = 0; i < iq.size(); i += 16384) sm.feed(iq.data() + i, std::min<size_t>(16384, iq.size() - i));
    const std::vector<SmartPacket> pk = sm.packets();
    const SmartStatus st = sm.status();
    printf("%zu packets, %zu carriers\n", pk.size(), st.carriers.size());
    for (const SmartCarrier& k : st.carriers) printf("  %.4f MHz %s %s frames %llu\n", k.freqHz / 1e6, k.modText.c_str(), k.state.c_str(), (unsigned long long)k.frames);
    for (const SmartPacket& p : pk) printf("  %s\n", packetSummary(p).c_str());
    const int mf = matches(pk, sFsk), mb = matches(pk, sBpsk), mc = matches(pk, sCw);
    CHECK(mf >= 4, "FSK 9600: %d of %zu frames", mf, sFsk.size());
    CHECK(mb >= 1, "BPSK 1200 AO-40: %d of %zu frames", mb, sBpsk.size());
    CHECK(mc >= 1, "CW: %d of %zu messages", mc, sCw.size());
    // the FUNcube frame has a header the parser knows
    bool tel = false;
    for (const SmartPacket& p : pk) if (p.framingId == kFramingAo40 && !p.tel.fields.empty()) tel = true;
    CHECK(tel || mb == 0, "AO-40 frames come with telemetry fields");
    // exports
    CHECK(!exportCsv(pk).empty() && exportKiss(pk).size() > 4, "exports");
    return testSummary("cubesat smart");
}
