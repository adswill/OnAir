// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave receiver on the generator's signal: the whole schedule at R1 / R2 / R3, with a carrier offset, a bit clock error, a lower SNR, another
// region; encrypted frames counted but not decoded; noise alone decodes nothing.
#include "dect2/zwave/zwave_gen.h"
#include "dect2/zwave/zwave_rx.h"
#include "dect2/zwave/zwave_tel.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static ZwaveTelemetry run(const char* region, double centreMhz, double snr, double cfo, double ppm, double secs, bool noFrames = false) {
    ZwGenConfig g;
    g.rate = 2e6; g.centerMhz = centreMhz; g.snrDb = snr; g.cfoHz = cfo; g.clockPpm = ppm;
    if (!noFrames) g.frames = zwaveTestSchedule(region);
    ZwaveGenerator gen(g);
    ZwaveReceiver rx;
    rx.configure(2e6);
    rx.setCenterHz(centreMhz * 1e6);
    rx.setDialHz(centreMhz * 1e6);
    rx.setRegion(region);
    std::vector<cf32> buf(8192);
    for (size_t done = 0; done < (size_t)(secs * 2e6); done += buf.size()) { gen.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    ZwaveTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

int main() {
    const size_t nFrames = zwaveTestSchedule("EU").size();
    {
        const ZwaveTelemetry t = run("EU", 869.13, 28, 0, 0, 1.3);
        printf("  EU clean: %s, bad %llu, bursts %llu, R %llu/%llu/%llu\n", zwaveSummary(t).c_str(), (unsigned long long)t.blocksBad, (unsigned long long)t.bursts,
               (unsigned long long)t.perRate[0], (unsigned long long)t.perRate[1], (unsigned long long)t.perRate[2]);
        CHECK(t.blocksOk >= nFrames && t.blocksBad == 0, "%llu frames of %zu", (unsigned long long)t.blocksOk, nFrames);
        CHECK(t.perRate[0] >= 3 && t.perRate[1] >= 8 && t.perRate[2] >= 2, "rates");
        CHECK(t.networks.size() == 1 && t.networks[0].home == "C0FFEE01", "network");
        CHECK(t.nodes.size() >= 6, "%zu nodes", t.nodes.size());
        CHECK(t.encS0 >= 1 && t.encS2 >= 1 && t.acks >= 3, "S0 %llu S2 %llu acks %llu", (unsigned long long)t.encS0, (unsigned long long)t.encS2, (unsigned long long)t.acks);
        bool sw = false, enc = false;
        for (const auto& f : t.log) {
            if (f.text == "SWITCH_BINARY Set on" && f.src == "1" && f.dst == "2") sw = true;
            if (f.encryption == 1 && f.text.find("not decoded") != std::string::npos) enc = true;
        }
        CHECK(sw && enc, "switch frame %d, encrypted text %d", sw, enc);
    }
    for (double cfo : {-6000.0, 6000.0}) {
        const ZwaveTelemetry t = run("EU", 869.13, 28, cfo, 0, 1.3);
        CHECK(t.blocksOk >= nFrames && t.blocksBad == 0, "cfo %.0f: %llu frames, %llu bad", cfo, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {
        const ZwaveTelemetry t = run("EU", 869.13, 28, 0, 80, 1.3);
        CHECK(t.blocksOk >= nFrames, "clock +80 ppm: %llu", (unsigned long long)t.blocksOk);
        const ZwaveTelemetry u = run("EU", 869.13, 28, 0, -80, 1.3);
        CHECK(u.blocksOk >= nFrames, "clock -80 ppm: %llu", (unsigned long long)u.blocksOk);
    }
    {
        const ZwaveTelemetry t = run("EU", 869.13, 18, 0, 0, 2.6);
        printf("  EU 18 dB: %llu frames of %zu, %llu bad\n", (unsigned long long)t.blocksOk, 2 * nFrames, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk >= nFrames, "18 dB: %llu", (unsigned long long)t.blocksOk);
    }
    {   // the capture off the region's own centre: the channels sit elsewhere in the band
        const ZwaveTelemetry t = run("EU", 868.90, 28, 0, 0, 1.3);
        CHECK(t.blocksOk >= nFrames - 3, "off-centre: %llu (R3 at +950 kHz is inside)", (unsigned long long)t.blocksOk);
    }
    {   // the US: R1 / R2 at 908.4 and 916 apart: 2 Msps takes only the first two
        const ZwaveTelemetry t = run("US", 908.00, 28, 0, 0, 1.3);
        CHECK(t.blocksOk >= 12 && t.perRate[2] == 0 && t.channelsCovered == 2, "US: %llu frames, %d channels", (unsigned long long)t.blocksOk, t.channelsCovered);
    }
    {   // noise alone: nothing decoded
        const ZwaveTelemetry t = run("EU", 869.13, 20, 0, 0, 1.0, true);
        CHECK(t.blocksOk == 0 && t.blocksBad == 0, "noise: %llu ok %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    if (fails) printf("zwave rx: %d FAILED\n", fails); else printf("zwave rx: all passed\n");
    return fails ? 1 : 0;
}
