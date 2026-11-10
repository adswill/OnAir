// TETRAPOL channel filter control (#408): the Baseband filter setting "tetrapol" reaches the receiver at its next start or retune. The
// control channel test signal must decode with the mode's own filter and with a custom one at several widths, shapes, carrier offsets and
// noise levels; a filter far too narrow for 8 kbit/s GMSK must stop it (the setting is really used); a neighbouring carrier 12.5 kHz away,
// 10 dB stronger, must be cut by a narrow custom filter. Simulated signals only.
#include "dect2/dsp/baseband_filter.h"
#include "dect2/tetrapol/tetrapol_gen.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/engine/modes.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static bool cellOk(const TetrapolTelemetry& t) {
    for (const auto& c : t.cells)
        if (c.hasSysinfo && c.country == kTetrapolTestCountry && c.cellId == kTetrapolTestCell && c.scramble == kTetrapolTestScramble) return true;
    return false;
}

// neighbour: 0 none, otherwise its offset in Hz (10 dB above the wanted carrier)
static TetrapolTelemetry run(double snr, double cfo, double neighbourHz, double secs = 5.0) {
    const double rate = 250000;
    SynthConfig sc;
    sc.mode = 81; sc.snrDb = snr; sc.cfoHz = cfo;
    auto syn = makeTetrapolSynth(sc, rate);
    std::unique_ptr<ModeSynth> nb;
    if (neighbourHz != 0) { SynthConfig n2 = sc; n2.snrDb = 60; n2.cfoHz = cfo + neighbourHz; n2.modeOpt[0] = 9; nb = makeTetrapolSynth(n2, rate); }
    TetrapolReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetrapolTestCentreHz);
    rx.reset();
    const size_t chunk = 12500;
    std::vector<cf32> buf(chunk), b2(chunk);
    for (double done = 0; done < secs; done += (double)chunk / rate) {
        syn->generate(buf.data(), chunk);
        if (nb) { nb->generate(b2.data(), chunk); for (size_t i = 0; i < chunk; i++) buf[i] += b2[i] * 3.162f; }
        rx.feed(buf.data(), chunk);
    }
    TetrapolTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static BasebandFilterCfg cfg(double bw, BbShape sh, double tr) { BasebandFilterCfg c; c.enabled = true; c.bwHz = bw; c.shape = sh; c.transHz = tr; return c; }

int main() {
    CHECK(std::string(bbf::keyForFamily(tetrapolTuning().stdMode - 1)) == "tetrapol", "TETRAPOL has a Baseband filter key");
    bbf::set("tetrapol", BasebandFilterCfg());
    for (double cfo : {0.0, 700.0, -1500.0}) {
        const TetrapolTelemetry t = run(25, cfo, 0);
        CHECK(cellOk(t), "own filter, CFO %+.0f Hz: %llu blocks ok", cfo, (unsigned long long)t.blocksOk);
    }
    struct Case { double bw; BbShape sh; double tr; double snr, cfo; };
    const Case ok[] = {{13000, BbShape::Blackman, 0, 25, 0}, {11000, BbShape::Kaiser, 3000, 25, 400}, {16000, BbShape::BlackmanHarris, 4000, 20, -800},
                       {10000, BbShape::Hann, 3000, 25, 0}, {12000, BbShape::Butterworth, 3000, 22, 300}, {20000, BbShape::Kaiser, 6000, 25, 0}};
    for (const Case& c : ok) {
        bbf::set("tetrapol", cfg(c.bw, c.sh, c.tr));
        const TetrapolTelemetry t = run(c.snr, c.cfo, 0);
        CHECK(cellOk(t), "custom %.0f Hz shape %d, SNR %.0f, CFO %+.0f: %llu blocks ok", c.bw, (int)c.sh, c.snr, c.cfo, (unsigned long long)t.blocksOk);
    }
    // far too narrow: the setting must be the one in use
    bbf::set("tetrapol", cfg(1500, BbShape::Kaiser, 500));
    { const TetrapolTelemetry t = run(25, 0, 0); CHECK(!cellOk(t) && t.blocksOk == 0, "a 1.5 kHz filter still decoded %llu blocks: the setting is not used", (unsigned long long)t.blocksOk); }
    // a strong neighbour 12.5 kHz away: a narrow custom filter (10 kHz, Kaiser) keeps the wanted carrier
    bbf::set("tetrapol", cfg(10000, BbShape::Kaiser, 2500));
    { const TetrapolTelemetry t = run(25, 0, 12500); CHECK(cellOk(t), "neighbour at +12.5 kHz with the 10 kHz filter: %llu blocks ok", (unsigned long long)t.blocksOk); }
    bbf::set("tetrapol", BasebandFilterCfg());
    { const TetrapolTelemetry t = run(25, 0, 0); CHECK(cellOk(t), "back to the own filter"); }
    return testSummary("tetrapol_filter");
}
