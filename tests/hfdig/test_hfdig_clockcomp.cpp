// The automatic clock correction of the slot decoders (dect2/hfdig/clock_comp.h): stations whose DT says the computer clock is wrong, fed through the
// real FT8 decoder with a simulated system clock. Simulated signals only: the proof on air needs a real band and a wrong clock.
#include "dect2/hfdig/hfdig_rx.h"
#include "hfdig_ftx_testutil.h"
#include <memory>

using namespace ftxtest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static HfdigFtxTelemetry run(double stationDt, double clockOffset, int slots) {
    std::vector<Station> st = {{"CQ DL1ABC JO62", 1000, 0, stationDt}, {"CQ LY2XY KO24", 1500, -3, stationDt}, {"CQ G4XYZ IO91", 2000, 2, stationDt}};
    std::vector<float> a = audio(0, st, slots, Faults{});
    auto rx = makeFtxDecoder();
    for (int m = 0; m < kFtxModes; m++) rx->setEnabled(m, m == 0);
    size_t fed = 0;
    rx->setWallClock([&] { return kT0 + clockOffset + (double)fed / kHfdigAudioRate; });
    rx->reset();
    for (size_t i = 0; i < a.size(); i += 4000) {
        const size_t n = std::min<size_t>(4000, a.size() - i);
        fed += n;   // as in real time: when a block is handed over, the clock is the time of its last sample
        rx->feedAudio(&a[i], n);
        rx->waitIdle();   // as in real time: a slot is decoded before the next audio comes, so a correction is applied while the signal runs
    }
    rx->waitIdle();
    HfdigFtxTelemetry t;
    rx->telemetry(t);
    return t;
}

static void dump(const HfdigFtxTelemetry& t) {   // every decode: slot, DT, early or full (says where a correction went when a check fails)
    for (const auto& d : t.decodes) printf("    slot %.0f dt %+.2f%s %s\n", std::fmod(d.slotUtc, 3600.0), d.dt, d.early ? " early" : "", d.msg.c_str());
}

static int lateDecodes(const HfdigFtxTelemetry& t) { int n = 0; for (const auto& d : t.decodes) if (std::fabs(d.dt) < 0.6) n++; return n; }

int main() {
    {   // stations late by 1.5 s: inside the normal search, the correction removes it
        const HfdigFtxTelemetry t = run(1.5, 0, 8);
        printf("late 1.5 s: trim %+.2f s, %d decodes, %d near 0\n", t.clockTrim, (int)t.decodes.size(), lateDecodes(t));
        if (std::fabs(t.clockTrim + 1.5) >= 0.4) dump(t);
        CHECK(std::fabs(t.clockTrim + 1.5) < 0.4, "trim %.2f", t.clockTrim);
        CHECK(lateDecodes(t) >= 6, "decodes near DT 0 after the correction: %d", lateDecodes(t));
    }
    {   // the computer clock 1.2 s fast: the same seen from the other side
        const HfdigFtxTelemetry t = run(0, 1.2, 8);
        printf("clock +1.2 s: trim %+.2f s, %d decodes\n", t.clockTrim, (int)t.decodes.size());
        CHECK(std::fabs(t.clockTrim + 1.2) < 0.4, "trim %.2f", t.clockTrim);
    }
    {   // right clock, stations on time: nothing moves
        const HfdigFtxTelemetry t = run(0.1, 0, 6);
        printf("on time: trim %+.2f s, %d decodes\n", t.clockTrim, (int)t.decodes.size());
        CHECK(t.clockTrim == 0.f, "no trim expected: %.2f", t.clockTrim);
        CHECK(t.decodes.size() >= 12, "decodes %d", (int)t.decodes.size());
    }
    {   // 4 s out, beyond the decoder's normal +-2.5 s: nothing decodes until the one wider search finds the slots
        const HfdigFtxTelemetry t = run(4.0, 0, 16);
        printf("out by 4 s: trim %+.2f s, %d decodes, %d near 0\n", t.clockTrim, (int)t.decodes.size(), lateDecodes(t));
        CHECK(std::fabs(t.clockTrim + 4.0) < 0.5, "trim %.2f", t.clockTrim);
        CHECK(lateDecodes(t) >= 3, "decodes near 0: %d", lateDecodes(t));
    }
    if (!fails) printf("clockcomp: all passed\n");
    return fails ? 1 : 0;
}
