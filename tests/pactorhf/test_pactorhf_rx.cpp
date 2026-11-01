// PACTOR-II / -III / -IV receivers through the radio front end: the generator's signal as the upper sideband of a radio at 250 ksps (the 20 kHz
// tuning offset mixed out), one receiver per mode. Each must report its own signal with the right parameters, report what it sees when the signal
// is another PACTOR (the PACTOR-II mode in front of PACTOR-III), and nothing for noise.
#include "dect2/pactorhf/pactorhf_gen.h"
#include "dect2/pactorhf/pactorhf_rx.h"
#include "dect2/pactorhf/pactorhf_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 250000;

static PactorHfTelemetry run(int rxVariant, PactorHfGenConfig c, double sec, bool noiseOnly = false, double cfoRadioHz = 0) {
    c.rate = kRate; c.analytic = true; c.dialOffsetHz = -20000 + cfoRadioHz;
    PactorHfGenerator g(c);
    PactorHfReceiver r(rxVariant);
    r.configure(kRate);
    r.setSignalOffset(-20000);
    r.setAnalysisInterval(8);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 0.02f);
    std::vector<cf32> buf(25000);
    const size_t tot = (size_t)(sec * kRate);
    for (size_t i = 0; i < tot; i += buf.size()) {
        if (noiseOnly) for (auto& v : buf) v = cf32(nd(rng), nd(rng)); else g.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
    }
    r.flush();
    PactorHfTelemetry t;
    r.telemetry(t, 0);
    return t;
}

int main() {
    {   // PACTOR-II in its own mode
        PactorHfGenConfig c; c.variant = 2; c.snrDb = 20;
        const PactorHfTelemetry t = run(2, c, 40);
        CHECK(t.variant == 2 && t.nCarriers == 2 && t.state == 2 && t.dataValid, "PACTOR-II: variant %d carriers %d state %d", t.variant, t.nCarriers, t.state);
        CHECK(t.framePeriod == 362 && std::fabs(t.baud - 100) < 0.1, "PACTOR-II: frame %d baud %.3f", t.framePeriod, t.baud);
        CHECK(t.rows.size() == 2 && !t.rows[0].empty(), "PACTOR-II: %zu symbol rows", t.rows.size());
        CHECK(!t.events.empty() && t.events.front().text.find("PACTOR-II") != std::string::npos, "PACTOR-II event");
        printf("  %s\n", pactorHfSummary(t).c_str());
    }
    {   // the radio tuned 40 Hz off
        PactorHfGenConfig c; c.variant = 2; c.snrDb = 20;
        const PactorHfTelemetry t = run(2, c, 40, false, 40);
        CHECK(t.variant == 2 && t.nCarriers == 2 && t.framePeriod == 362, "PACTOR-II tuned 40 Hz off: carriers %d frame %d", t.nCarriers, t.framePeriod);
        CHECK(std::fabs(t.cfoHz - 40) < 6, "tuning error reported as %.1f Hz", t.cfoHz);
    }
    {   // PACTOR-III level 6 in its mode; the same signal in PACTOR-II mode is named, not mistaken
        PactorHfGenConfig c; c.variant = 3; c.level = 6; c.snrDb = 20; c.amp = 0.08;
        const PactorHfTelemetry t = run(3, c, 40);
        CHECK(t.variant == 3 && t.nCarriers == 18 && t.level == 6 && t.framePeriod == 362, "PACTOR-III: variant %d carriers %d level %d frame %d", t.variant, t.nCarriers, t.level, t.framePeriod);
        const PactorHfTelemetry u = run(2, c, 40);
        CHECK(u.variant == 3 && u.wanted == 2 && pactorHfSummary(u).find("looks like PACTOR-III") != std::string::npos, "PACTOR-II mode on PACTOR-III: %s", pactorHfSummary(u).c_str());
    }
    {   // PACTOR-IV
        PactorHfGenConfig c; c.variant = 4; c.order = 4; c.snrDb = 25; c.amp = 0.3;
        const PactorHfTelemetry t = run(4, c, 25);
        CHECK(t.variant == 4 && t.constellation == 4 && std::fabs(t.baud - 1800) < 1, "PACTOR-IV: variant %d constellation %d baud %.1f", t.variant, t.constellation, t.baud);
    }
    {   // noise alone
        for (int v = 2; v <= 4; v++) {
            PactorHfGenConfig c;
            const PactorHfTelemetry t = run(v, c, 30, true);
            CHECK(t.variant == 0 && t.state == 0 && t.events.empty(), "noise in mode %d: variant %d", v, t.variant);
        }
    }
    {   // too slow a radio
        PactorHfReceiver r(2);
        r.configure(100000);
        CHECK(!r.ready(), "100 ksps is below the minimum");
    }
    return testSummary("pactorhf rx");
}
