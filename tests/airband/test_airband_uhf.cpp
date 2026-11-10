// The military UHF air band (225 - 400 MHz AM, 25 kHz channels) in the airband mode: channel names and the raster, the known channels
// (243.000 guard, 282.800 SAR), and the receiver on the test generator around 243.000 MHz: every keyed channel opens, the empty neighbours
// 25 kHz away stay shut, through a radio tuning error of up to 30 ppm (7.3 kHz at 243 MHz), a noise sweep, a sample clock error and 8-bit
// clipping; the band watch puts unlisted carriers on the 25 kHz raster with UHF names. Simulated signals only.
#include "dect2/airband/airband_gen.h"
#include "dect2/airband/airband_mil.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/airband/airband_tel.h"
#include "dect2/fastscan/plan.h"
#include "dect2/fastscan/profile.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 1e6, kDial = 243.000e6;
static const double kOff = airbandTuning().tuneOffsetHz;

static AirbandTelemetry run(AirbandGenConfig g, double secs, const std::vector<AirbandChannel>& list,
                            const std::function<void(std::vector<cf32>&)>& fault = nullptr, double tuneGuess = 0) {
    g.rate = kRate; g.dialOffsetHz = -kOff;
    AirbandGenerator gen(g);
    std::vector<cf32> x((size_t)(secs * kRate));
    for (size_t o = 0; o < x.size(); o += 65536) gen.generate(x.data() + o, std::min<size_t>(65536, x.size() - o));
    if (fault) fault(x);
    AirbandReceiver rx;
    rx.setSilent(true);
    rx.configure(kRate);
    rx.setCenterHz(kDial + kOff);
    if (tuneGuess != 0) rx.setTuneErrorHz(tuneGuess);
    rx.setChannels(list);
    AirbandTelemetry t;
    for (size_t o = 0; o < x.size(); o += 8192) rx.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
    rx.telemetry(t, 0);
    return t;
}

static int openings(const AirbandTelemetry& t, int chan) { int n = 0; for (const auto& e : t.activity) n += e.chan == chan; return n; }

int main() {
    // ---- names and the raster
    {
        double f; bool n;
        CHECK(airbandParse("243.000", f, n) && !n && std::fabs(f - 243e6) < 1, "243.000");
        CHECK(airbandParse("243", f, n) && !n && std::fabs(f - 243e6) < 1, "243");
        CHECK(airbandParse("282.8", f, n) && !n && std::fabs(f - 282.8e6) < 1, "282.8");
        CHECK(airbandParse("225.000", f, n) && airbandParse("399.975", f, n) && !n && std::fabs(f - 399.975e6) < 1, "band edges");
        CHECK(airbandParse("243000000", f, n) && std::fabs(f - 243e6) < 1, "in Hz");
        CHECK(!airbandParse("224.975", f, n) && !airbandParse("400.000", f, n), "outside the band");
        CHECK(!airbandParse("243.005", f, n) && !airbandParse("243.010", f, n) && !airbandParse("243.0125", f, n) && !airbandParse("243.0083", f, n), "no 8.33 kHz raster on UHF");
        CHECK(!airbandParse("243.020", f, n) && !airbandParse("150.000", f, n) && !airbandParse("200.000", f, n), "off the raster or between the bands");
        CHECK(airbandName(243e6, false) == "243.000" && airbandName(282.8e6, false) == "282.800" && airbandName(399.975e6, false) == "399.975", "UHF names");
        int bad = 0;
        for (double fr = 225e6; fr <= 399.975e6 + 1; fr += 25000) { double g; bool m; if (!airbandParse(airbandName(fr, false), g, m) || m || std::fabs(g - fr) > 1) bad++; }
        CHECK(bad == 0, "%d of the 7000 UHF channels do not round-trip", bad);
        CHECK(airbandMilUhfChannelCount() == 7000, "channel count %d", airbandMilUhfChannelCount());
        // the VHF band is unchanged
        CHECK(airbandParse("118.005", f, n) && n && std::fabs(f - 118.000e6) < 1, "VHF 8.33 name");
        CHECK(airbandParse("121.5", f, n) && !n, "VHF 121.5");
    }
    // ---- the known channels
    {
        const auto ch = airbandMilUhfChannels();
        bool guard = false, sar = false;
        for (const auto& c : ch) {
            CHECK(airbandOnRaster(c.freqHz, false) && !c.is833 && airbandIsMilUhf(c.freqHz), "%.3f on the UHF raster", c.freqHz / 1e6);
            if (std::fabs(c.freqHz - 243e6) < 1) guard = c.priority;
            if (std::fabs(c.freqHz - 282.8e6) < 1) sar = true;
        }
        CHECK(guard && sar, "243.000 guard (priority) and 282.800 listed");
        CHECK(airbandTuning().maxMhz >= 400, "the mode's band reaches 400 MHz: %.0f", airbandTuning().maxMhz);
        bool prof = false;
        for (const auto& p : fastscan::builtinProfiles()) if (p.bands == "225-400" && p.channelKhz == 25 && p.priorityBands.find("243.0") != std::string::npos) prof = true;
        CHECK(prof, "fast scan profile for the military UHF band");
        CHECK(fastscan::guessMode(243e6, 25e3) == "am", "fast scan hits on UHF air are AM");
    }
    // ---- the receiver around 243.000: guard at the dial, a channel 25 kHz up, one 150 kHz down; empty listed neighbours at -25 and +50 kHz
    std::vector<AirbandGenChannel> lay(3);
    lay[0].offsetHz = 0;       lay[0].label = "guard"; lay[0].levelDb = 0;   lay[0].period = 5; lay[0].on = 2;   lay[0].phase = 0.5;
    lay[1].offsetHz = 25000;   lay[1].label = "up";    lay[1].levelDb = 6;   lay[1].period = 5; lay[1].on = 1.5; lay[1].phase = 1.0;
    lay[2].offsetHz = -150000; lay[2].label = "down";  lay[2].levelDb = -6;  lay[2].period = 4; lay[2].on = 1.5; lay[2].phase = 0.3;
    std::vector<AirbandChannel> list;
    for (double o : {0.0, 25000.0, -150000.0, -25000.0, 50000.0}) { AirbandChannel c; c.freqHz = kDial + o; c.is833 = false; list.push_back(c); }
    struct Case { double cfo, snr, ppm; bool clip; };
    const Case cases[] = {{0, 30, 0, false}, {3000, 30, 0, false}, {-7000, 30, 0, false}, {7000, 25, 0, false}, {0, 18, 0, false}, {0, 12, 0, false},
                          {2000, 25, 40, false}, {-2000, 30, 0, true}};
    for (const auto& c : cases) {
        AirbandGenConfig g; g.chans = lay; g.cfoHz = c.cfo; g.snrDb = c.snr; g.seed = 5;
        std::function<void(std::vector<cf32>&)> fault;
        if (c.ppm != 0 || c.clip) fault = [c](std::vector<cf32>& x) { if (c.ppm != 0) x = impair::clock(x, c.ppm); if (c.clip) impair::clip8(x, 3.0); };
        const AirbandTelemetry t = run(g, 10.0, list, fault);
        char what[96];
        snprintf(what, sizeof what, "cfo %+.0f snr %.0f ppm %.0f%s", c.cfo, c.snr, c.ppm, c.clip ? " clipped" : "");
        // the "down" channel is 6 dB below the guard: at 12 dB that leaves it at 6 dB, under the squelch (it must then stay shut, not chatter)
        const bool downAudible = c.snr - 6 >= 10;
        CHECK(openings(t, 0) >= 2 && openings(t, 1) >= 2 && (!downAudible || openings(t, 2) >= 2), "%s: openings %d %d %d", what, openings(t, 0), openings(t, 1), openings(t, 2));
        CHECK(openings(t, 3) == 0 && openings(t, 4) == 0, "%s: empty neighbours opened %d %d", what, openings(t, 3), openings(t, 4));
        CHECK(std::fabs(t.cfoHz - c.cfo) < 600 || c.ppm != 0, "%s: tuning error measured %.0f Hz", what, t.cfoHz);
        if (!t.channels.empty()) CHECK(t.channels[0].name == "243.000", "%s: name %s", what, t.channels[0].name.c_str());
    }
    // ---- band watch: only the guard listed; the other two carriers are found and named on the 25 kHz raster
    {
        AirbandGenConfig g; g.chans = lay; g.cfoHz = 1500; g.snrDb = 30; g.seed = 9;
        for (auto& ch : g.chans) { ch.continuous = true; }
        std::vector<AirbandChannel> one(1, list[0]);
        const AirbandTelemetry t = run(g, 6.0, one);
        bool up = false, down = false, wrong = false;
        for (const auto& s : t.seen) {
            if (s.name == "243.025") up = true;
            else if (s.name == "242.850") down = true;
            else wrong = true;
            CHECK(!s.is833, "seen %s marked 8.33", s.name.c_str());
        }
        CHECK(up && down && !wrong, "band watch: 243.025 %d, 242.850 %d, others %d (%zu seen)", (int)up, (int)down, (int)wrong, t.seen.size());
    }
    return testSummary("airband_uhf");
}
