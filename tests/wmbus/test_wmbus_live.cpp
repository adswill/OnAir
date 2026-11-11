// Wireless M-Bus receiver against the test signal: every mode, offsets, noise, sample rates, the 169 MHz plan, and noise alone.
#include "dect2/wmbus/wmbus_gen.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/wmbus/wmbus_tel.h"
#include <cstdio>
#include <set>
#include <vector>
using namespace dect2;
using namespace dect2::wmbus;
#include "../common/test_util.h"

static WmbusTelemetry run(const WmbusGenConfig& c, double seconds, double centre = -1) {
    WmbusGenerator g(c);
    WmbusLiveReceiver rx;
    rx.configure(c.rate);
    rx.setCenterHz(centre < 0 ? c.centreHz : centre);
    std::vector<cf32> b(20000);
    for (size_t done = 0; done < (size_t)(seconds * c.rate); done += b.size()) { g.generate(b.data(), b.size()); rx.feed(b.data(), b.size()); }
    WmbusTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static std::set<int> phys(const WmbusTelemetry& t) { std::set<int> s; for (auto& l : t.log) if (l.crcOk) s.insert(l.phy); return s; }

int main() {
    const ModeTuning mt = wmbusTuning();
    CHECK(mt.stdMode == 88 && modeTuningById("wmbus") && modeTuning(88) && modeTuning(88)->stdMode == 88, "tuning table");
    {   // every mode of the 868 MHz plan, clean
        WmbusGenConfig c; c.snrDb = 99; c.gapSec = 0.05;
        const WmbusTelemetry t = run(c, 8);
        const auto p = phys(t);
        printf("  clean 868: %llu ok %llu bad, devices %zu, modes %zu\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.devices.size(), p.size());
        for (Phy ph : {Phy::S1, Phy::T1, Phy::C1A, Phy::C1B, Phy::R2}) CHECK(p.count((int)ph), "mode %s not decoded", phyName(ph));
        CHECK(t.blocksBad == 0, "bad frames %llu", (unsigned long long)t.blocksBad);
        CHECK(t.devices.size() == 6, "devices %zu", t.devices.size());
        bool enc = false, water = false;
        for (auto& d : t.devices) { if (d.encrypted) enc = d.last.empty() || d.records.empty(); if (d.id == "12345678" && !d.records.empty() && d.mfr == "KAM") water = true; }
        CHECK(enc && water, "encrypted shown %d, water meter read %d", (int)enc, (int)water);
        CHECK(t.channels.size() == 5, "channels %zu", t.channels.size());
    }
    {   // transmitter frequency error and noise
        for (double cfo : {-12000.0, 9000.0}) {
            WmbusGenConfig c; c.snrDb = 25; c.cfoHz = cfo; c.gapSec = 0.05;
            const WmbusTelemetry t = run(c, 10);
            const auto p = phys(t);
            printf("  cfo %+.0f Hz, 25 dB: %llu ok %llu bad, modes %zu\n", cfo, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, p.size());
            CHECK(p.size() == 5, "cfo %.0f: modes %zu", cfo, p.size());
        }
    }
    {   // weak signals: some frames still decode, none is wrong (a CRC failure is never taken as a meter)
        for (double snr : {18.0, 14.0}) {
            WmbusGenConfig c; c.snrDb = snr; c.gapSec = 0.05;
            const WmbusTelemetry t = run(c, 12);
            printf("  %.0f dB: %llu ok %llu bad\n", snr, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
            CHECK(t.blocksOk > 0, "no frame at %.0f dB", snr);
            CHECK(t.devices.size() <= 6, "invented meters at %.0f dB: %zu", snr, t.devices.size());
        }
    }
    {   // 3.9 sensitivity (docs/SENSITIVITY-3.9-E.md): T1, C1 and S1 alone at 10 dB in 250 kHz (14 dB in 100 kHz), with and without a
        // 15 kHz carrier error; 3.8 decoded no T1 frame and a few C1 frames here
        struct G { Phy ph; double cfo, share; };
        for (const G g : {G{Phy::T1, 0, 0.8}, G{Phy::C1A, 0, 0.6}, G{Phy::S1, 0, 0.9}, G{Phy::T1, 15000, 0.6}, G{Phy::C1A, -15000, 0.4}}) {
            WmbusGenConfig c; c.snrDb = 99; c.gapSec = 0.05; c.onlyPhy = (int)g.ph; c.cfoHz = g.cfo;
            const double clean = (double)run(c, 3).blocksOk;
            c.snrDb = 10;
            const WmbusTelemetry t = run(c, 3);
            printf("  %s alone, cfo %+.0f Hz, 10 dB: %llu of %.0f frames, %llu bad\n", phyName(g.ph), g.cfo, (unsigned long long)t.blocksOk, clean, (unsigned long long)t.blocksBad);
            CHECK(clean > 10 && (double)t.blocksOk >= g.share * clean, "%s at 10 dB, cfo %.0f: %llu of %.0f frames", phyName(g.ph), g.cfo, (unsigned long long)t.blocksOk, clean);
            CHECK(t.devices.size() <= 6, "invented meters: %zu", t.devices.size());
        }
    }
    {   // sample rates
        for (double r : {1.2e6, 2.4e6, 2.56e6}) {
            WmbusGenConfig c; c.rate = r; c.snrDb = 30; c.gapSec = 0.05;
            const WmbusTelemetry t = run(c, 8);
            printf("  rate %.2f Msps: %llu ok %llu bad, modes %zu, channels %zu\n", r / 1e6, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, phys(t).size(), t.channels.size());
            CHECK(phys(t).count((int)Phy::S1) && phys(t).count((int)Phy::T1), "rate %.0f: modes %zu", r, phys(t).size());
        }
    }
    {   // the 169 MHz plan
        WmbusGenConfig c; c.plan = 1; c.centreHz = 169.4375e6; c.snrDb = 30; c.gapSec = 0.05;
        const WmbusTelemetry t = run(c, 6);
        printf("  169 MHz: %llu ok %llu bad, channels %zu\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.channels.size());
        CHECK(phys(t).count((int)Phy::N1) && t.channels.size() == 6, "N1 not decoded");
    }
    {   // noise alone and a bare channel give nothing
        WmbusGenConfig c; c.snrDb = 10; c.silent = true;
        const WmbusTelemetry t = run(c, 6);
        CHECK(t.blocksOk == 0 && t.devices.empty(), "noise gave %llu frames", (unsigned long long)t.blocksOk);
        CHECK(t.syncs <= 1, "false syncs %llu", (unsigned long long)t.syncs);
    }
    return testSummary("wmbus live");
}
