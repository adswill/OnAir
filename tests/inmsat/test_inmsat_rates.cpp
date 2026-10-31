// Inmarsat all-carriers mode at the sample rates real radios give: Airspy R2 (10 / 2.5 Msps), Airspy Mini (6 / 3 Msps, 10 unofficial),
// RTL-SDR (2.4 Msps), HackRF. The same Aero test group goes through the manager at each rate, with the Airspy-style impairments
// (a DC spike, a mirror image from the IQ imbalance, a carrier offset). Synthetic only: a real Airspy capture is still wanted.
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/aero/aero_gen.h"
#include "dect2/aero/aero_rx.h"
#include "dect2/inmc/inmc_testutil.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::inmsat;
#include "../common/test_util.h"

namespace {
struct Impair {
    double dc, imageDb, offsetHz, fs, ph = 0;
    void apply(cf32* x, size_t n) {
        const double w = 2 * M_PI * offsetHz / fs;
        const float k = (float)std::pow(10.0, imageDb / 20.0);
        for (size_t i = 0; i < n; i++) {
            x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += w; if (ph > 2 * M_PI) ph -= 2 * M_PI;
            x[i] += cf32((float)dc, (float)dc) + k * std::conj(x[i]);
        }
    }
};

int run(double rate, double secs, double dc, double imageDb, double offsetHz) {
    SynthConfig a;
    a.mode = 20;
    a.modeOpt[0] = 7; a.modeVal[0] = 17;     // 600 + 1200 + 10500 bit/s
    AeroSynth s(a, rate);
    Impair imp{dc, imageDb, offsetHz, rate};
    ManagerConfig mc;
    mc.rate = rate; mc.centerHz = 1545.0e6;
    Manager mgr;
    mgr.configure(mc);
    const size_t chunk = 65536;
    std::vector<cf32> buf(chunk);
    Snapshot snap;
    uint64_t last = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += chunk) {
        s.generate(buf.data(), chunk);
        imp.apply(buf.data(), chunk);
        mgr.feed(buf.data(), chunk);
        if (mgr.snapshot(snap, last)) last = snap.seq;
    }
    int ok = 0;
    for (const auto& ch : s.channels()) {
        const double want = ch.offsetHz - aeroTuning().tuneOffsetHz + offsetHz;
        bool in = false;
        for (const auto& c : snap.carriers) in |= std::fabs(c.offsetHz - want) < 3000 && c.type == CarrierType::AeroP && c.bitRate == ch.bitRate && c.state >= 2;
        ok += in;
    }
    printf("rate %.2f Msps dc %.2f image %.0f dB offset %+.0f Hz: %d of %zu carriers in sync, windows %zu\n", rate / 1e6, dc, imageDb, offsetHz, ok, s.channels().size(), snap.windows.size());
    for (const auto& w : snap.windows) printf("   window %+9.0f Hz out %.0f sps aero %d\n", w.centerHz, w.outRate, w.aero);
    for (const auto& c : snap.carriers) printf("   carrier %+10.0f Hz width %6.0f snr %5.1f type %d rate %d state %d\n", c.offsetHz, c.widthHz, c.snrDb, (int)c.type, c.bitRate, c.state);
    return ok == (int)s.channels().size();
}

// an Inmarsat-C channel (with an extra one) 1 MHz above the centre, through the same chain
int runInmc(double rate, double secs) {
    InmcGenConfig g;
    g.rate = rate; g.ebn0Db = 17; g.seed = 5; g.extraChannels = 1;
    auto gen = makeInmcGenerator(g);
    Impair imp{0.2, -40, 1000, rate};
    ManagerConfig mc;
    mc.rate = rate; mc.centerHz = 1545.0e6;
    Manager mgr;
    mgr.configure(mc);
    const size_t chunk = 65536;
    std::vector<cf32> buf(chunk);
    double ph = 0; const double w = 2 * M_PI * 1.0e6 / rate;
    Snapshot snap;
    uint64_t last = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += chunk) {
        gen->generate(buf.data(), chunk);
        for (size_t i = 0; i < chunk; i++) { buf[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += w; if (ph > 2 * M_PI) ph -= 2 * M_PI; }
        imp.apply(buf.data(), chunk);
        mgr.feed(buf.data(), chunk);
        if (mgr.snapshot(snap, last)) last = snap.seq;
    }
    int ok = 0;
    for (const auto& c : snap.carriers) ok += c.type == CarrierType::InmarsatC && c.state >= 2;
    printf("rate %.2f Msps Inmarsat-C: %d channels in sync, %zu messages\n", rate / 1e6, ok, snap.egc.size());
    return ok >= 2;
}
}

int main() {
    for (double rate : {2.5e6, 6e6, 10e6}) CHECK(runInmc(rate, 60), "Inmarsat-C at %.2f Msps", rate / 1e6);
    for (double rate : {2.4e6, 2.5e6, 3e6, 6e6, 10e6}) {
        CHECK(run(rate, 40, 0, -200, 0), "clean at %.2f Msps", rate / 1e6);
        CHECK(run(rate, 40, 0.3, -40, 1500), "DC spike, image and offset at %.2f Msps", rate / 1e6);
    }
    return testSummary("inmsat_rates");
}
