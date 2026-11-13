// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu Fusion receiver against the generated C4FM signal: every data type, callsigns, slow data, noise, tuning error, a mirrored spectrum,
// a start in the middle of a transmission, two transmissions in a row, and noise alone.
#include "fusion_test_util.h"
#include <cstdio>
#include <string>
using namespace dect2;
using namespace dect2::fusion;
#include "../common/test_util.h"
using namespace fusiontest;

static GenParams base(int dt) {
    GenParams p;
    p.dt = dt; p.commFrames = 12; p.dest = "CQCQCQ"; p.src = "LY1ABC"; p.down = "DB0XYZ"; p.up = "DB0UPL";
    p.rem[0] = "11111"; p.rem[1] = "22222"; p.rem[2] = "33333"; p.rem[3] = "44444";
    p.slow = "55.0N 25.3E LY1ABC";
    p.cm = 3; p.mr = 1; p.voip = 1;
    return p;
}

static void expectCalls(const FusionTelemetry& t, const GenParams& p, const char* what) {
    CHECK(t.src == p.src, "%s: source '%s'", what, t.src.c_str());
    CHECK(t.dest == p.dest, "%s: destination '%s'", what, t.dest.c_str());
    CHECK(t.down == p.down, "%s: downlink '%s'", what, t.down.c_str());
    CHECK(t.up == p.up, "%s: uplink '%s'", what, t.up.c_str());
    CHECK(t.rem[0] == p.rem[0] && t.rem[3] == p.rem[3], "%s: rem '%s' '%s'", what, t.rem[0].c_str(), t.rem[3].c_str());
}

int main() {
    const char* names[4] = {"V/D mode 1", "data FR", "V/D mode 2", "voice FR"};
    for (int dt = 0; dt < 4; dt++) {
        const GenParams p = base(dt);
        FusionReceiver r;
        r.setSilent(true);
        run(r, signal({p}, 0, 25));
        const FusionTelemetry t = last(r);
        CHECK(t.headers == 1 && t.terminators == 1, "%s: headers %llu terminators %llu", names[dt], (unsigned long long)t.headers, (unsigned long long)t.terminators);
        CHECK(t.comms == 12, "%s: communication frames %llu", names[dt], (unsigned long long)t.comms);
        CHECK(t.blocksBad == 0, "%s: bad frames %llu", names[dt], (unsigned long long)t.blocksBad);
        CHECK(t.dataType == dt, "%s: data type %d", names[dt], t.dataType);
        CHECK(t.callMode == 3 && t.routing == 1 && t.voip == 1, "%s: call mode %d routing %d voip %d", names[dt], t.callMode, t.routing, t.voip);
        expectCalls(t, p, names[dt]);
        if (dt != 3) CHECK(t.slowText.find("55.0N 25.3E") != std::string::npos, "%s: slow text '%s'", names[dt], t.slowText.c_str());
        CHECK(!t.codecReady && t.codecText == "codec not installed", "%s: codec text '%s'", names[dt], t.codecText.c_str());
        CHECK(t.voiceDecoded == 0, "%s: voice decoded without a codec", names[dt]);
        if (dt != 1) CHECK(t.voiceSubFrames > 0, "%s: no voice sub-frames counted", names[dt]);
        CHECK(fusionSummary(t).find("LY1ABC") != std::string::npos, "%s: summary '%s'", names[dt], fusionSummary(t).c_str());
    }

    // real-world faults, V/D mode 1 and V/D mode 2
    struct Fault { const char* name; double cfo, snr; bool swap; double skip; };
    const Fault faults[] = {{"tuning +1.5 kHz", 1500, 25, false, 0}, {"tuning -1.5 kHz", -1500, 25, false, 0}, {"mirrored spectrum", 0, 25, true, 0},
                            {"noise 14 dB", 0, 14, false, 0}, {"tuning +1 kHz and noise 18 dB", 1000, 18, false, 0}};
    for (int dt : {0, 2}) for (const Fault& f : faults) {
        const GenParams p = base(dt);
        FusionReceiver r;
        r.setSilent(true);
        run(r, signal({p}, f.cfo, f.snr, f.swap, f.skip));
        const FusionTelemetry t = last(r);
        char w[96]; snprintf(w, sizeof w, "dt %d %s", dt, f.name);
        CHECK(t.blocksOk >= 12, "%s: only %llu good frames", w, (unsigned long long)t.blocksOk);
        expectCalls(t, p, w);
        if (f.swap) CHECK(t.inverted, "%s: not reported mirrored", w);
        if (f.cfo != 0) CHECK(std::fabs(t.cfoHz - f.cfo) < 300, "%s: measured offset %.0f", w, t.cfoHz);
    }

    // a start in the middle: V/D mode 1 header and the first callsign frames are missed, the later cycle gives them
    {
        GenParams p = base(0);
        p.commFrames = 24;
        FusionReceiver r;
        r.setSilent(true);
        run(r, signal({p}, 0, 25, false, 0.35));
        const FusionTelemetry t = last(r);
        CHECK(t.headers == 0 && t.comms >= 18, "late entry: headers %llu comms %llu", (unsigned long long)t.headers, (unsigned long long)t.comms);
        CHECK(t.src == p.src && t.dest == p.dest, "late entry callsigns '%s' -> '%s'", t.src.c_str(), t.dest.c_str());
    }

    // two transmissions in a row: the panel follows the second
    {
        GenParams a = base(0), b = base(2);
        b.src = "DL9XYZ"; b.dest = "LY1BWB"; b.down.clear(); b.up.clear(); b.commFrames = 14; b.slow = "hello";
        FusionReceiver r;
        r.setSilent(true);
        run(r, signal({a, b}, 0, 25));
        const FusionTelemetry t = last(r);
        CHECK(t.headers == 2 && t.terminators == 2, "two transmissions: headers %llu terminators %llu", (unsigned long long)t.headers, (unsigned long long)t.terminators);
        CHECK(t.src == "DL9XYZ" && t.dest == "LY1BWB" && t.down.empty(), "second callsigns '%s' -> '%s' down '%s'", t.src.c_str(), t.dest.c_str(), t.down.c_str());
        CHECK(t.slowText.find("hello") != std::string::npos && t.slowText.find("55.0N") == std::string::npos, "slow text '%s'", t.slowText.c_str());
    }

    // noise alone, and a signal that is not Fusion (a plain tone): no frames
    {
        std::vector<cf32> x(250000 * 2, cf32(0.3f, 0.f));
        impair::noise(x, 0.5, 4);
        FusionReceiver r;
        r.setSilent(true);
        run(r, x);
        const FusionTelemetry t = last(r);
        CHECK(t.blocksOk == 0 && t.headers == 0 && t.src.empty(), "noise: %llu frames", (unsigned long long)t.blocksOk);
    }
    // a transmission with a damaged middle: the receiver loses frames, finds the next and does not crash
    {
        GenParams p = base(0);
        p.commFrames = 20;
        std::vector<cf32> x = signal({p}, 0, 25);
        impair::noise(x, 25, 9);
        for (size_t i = x.size() / 3; i < x.size() / 3 + 40000 && i < x.size(); i++) x[i] = cf32(0, 0);
        FusionReceiver r;
        r.setSilent(true);
        run(r, x);
        const FusionTelemetry t = last(r);
        CHECK(t.blocksOk >= 16 && t.blocksOk < 22, "dropout: %llu good frames", (unsigned long long)t.blocksOk);
        CHECK(t.src == p.src, "dropout: source '%s'", t.src.c_str());
    }
    return testSummary("test_fusion_rx");
}
