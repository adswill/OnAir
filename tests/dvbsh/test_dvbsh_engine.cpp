// DVB-SH engine pieces: the test signal (engine standard 748) at radio rates, played into the engine receiver with the automatic channel
// width, its telemetry and summary, and the tuning row. Simulated signals only.
#include "dect2/dvbsh/dvbsh_engine.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/source.h"
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static DvbshTelemetry play(int width, double rate, double seconds, double cfo, long& good, long& bad, int form = 0) {
    SynthConfig c;
    c.snrDb = 18; c.cfoHz = cfo; c.sroPpm = 8;
    c.modeOpt[0] = width;
    c.modeOpt[5] = form;
    auto syn = makeDvbshSynth(c, rate);
    DvbshTelemetry t;
    if (!syn) { CHECK(false, "no synth at %.1f Msps", rate / 1e6); return t; }
    DvbshEngineReceiver rx;
    rx.configure(rate);
    good = bad = 0;
    double secs = 0;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double s) {
        secs += s;
        for (size_t i = 0; i < n; i++) ((pk[i * 188] == 0x47 && !(pk[i * 188 + 1] & 0x80)) ? good : bad)++;
    });
    std::vector<cf32> buf(65536);
    for (long done = 0; done < (long)(seconds * rate); done += (long)buf.size()) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    rx.telemetry(t, 0);
    std::printf("width %.1f MHz at %.2f Msps: state %d, %s, %ld good %ld bad packets\n", t.bwMhz, rate / 1e6, t.state, dvbshSummary(t).c_str(), good, bad);
    CHECK(secs > 0, "packet callback without signal time");
    return t;
}

int main() {
    const ModeTuning tu = dvbshTuning();
    CHECK(tu.stdMode == 748 && !strcmp(tu.id, "dvbsh") && tu.defMhz > 2170 && tu.defMhz < 2200, "tuning row");
    long good = 0, bad = 0;
    DvbshTelemetry t = play(0, 8e6, 1.2, 20000, good, bad);   // 5 MHz channel: the first width tried
    CHECK(t.state == 4 && t.bwMhz == 5 && good > 300 && bad == 0 && t.blocksBad == 0, "5 MHz at 8 Msps");
    t = play(1, 10e6, 1.6, -15000, good, bad);                // 8 MHz: found after the 5 MHz try
    CHECK(t.state == 4 && t.bwMhz == 8 && good > 300 && bad == 0, "8 MHz at 10 Msps");
    t = play(4, 2.4e6, 3.5, 3000, good, bad);                 // 1.7 MHz: the only width that fits 2.4 Msps
    CHECK(t.state == 4 && t.bwMhz == 1.7 && good > 100 && bad == 0, "1.7 MHz at 2.4 Msps");
    // SH-B (TDM) through the automatic search: every width as OFDM first, then TDM, whose symbol rate is found from table 5.13
    t = play(0, 8e6, 3.5, 6000, good, bad, 2);
    CHECK(t.state == 4 && t.tdm && t.tdmMod == 1 && t.bwMhz == 5 && good > 300 && bad == 0 && t.blocksBad == 0, "TDM 8PSK 5 MHz at 8 Msps (automatic)");
    std::printf("  TDM clock %+.1f ppm, carrier offset %+.0f Hz\n", t.clockPpm, t.cfoHz);
    CHECK(std::fabs(t.clockPpm - 8) < 5 && std::fabs(t.cfoHz - 6000) < 300, "TDM clock and carrier offset");
    CHECK(!makeDvbshSynth(SynthConfig{}, 1e6), "synth below the 1.7 MHz rate");
    std::printf(fails ? "dvbsh_engine: %d failures\n" : "dvbsh_engine: all passed\n", fails);
    return fails ? 1 : 0;
}
