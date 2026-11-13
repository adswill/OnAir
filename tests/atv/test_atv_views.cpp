// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The analog TV standards forced on the receiver (AtvReceiver::setStandard, the System and Colour choices of the Analog TV screen).
// The test signal of each standard (atv_gen: colour bars card, 40 dB, carrier error of up to +-50 kHz, 10 Msps, 8-bit samples) goes into a receiver told exactly what the entry tells it: line and field lock,
// the standard named right, 625 or 525 lines, the field rate of the standard, the carrier offset found, and the colour bars of the decoded picture close to the EBU bars. Simulated signals only (no recording of any of these standards was used; the wrong-standard case is not tested).
#include "dect2/atv/atv_rx.h"
#include "dect2/atv/atv_testkit.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
using namespace dect2;
using namespace dect2::atvkit;
#include "../common/test_util.h"

static double barError(const AtvFrame& fr, const AtvFormat& f) {
    AtvCard card(f);
    double c[8][3];
    barColours(fr, card, c);
    double worst = 0;
    for (int i = 0; i < 8; i++) { float rgb[3]; atvEbuBar(i, rgb); for (int k = 0; k < 3; k++) worst = std::max(worst, std::fabs(c[i][k] - rgb[k])); }
    return worst;
}

static Run runView(int sys, int colour, int fedSys, int fedColour, double cfo, double secs) {
    AtvGenConfig c; c.sys = fedSys; c.colour = fedColour; c.rate = 10e6; c.cnrDb = 40; c.cfoHz = cfo; c.sound = 4;
    Options o;
    o.onTime = [=](AtvReceiver& rx, double s) { if (s < 0.01 + 65536 / 10e6) rx.setStandard(sys, colour); };
    return run(c, secs, o);
}

int main() {
    struct V { const char* id; const char* name; int sys, colour; };   // the filters the Analog TV screen offers
    static const V views[] = {
        {"atvpalbg", "Analog TV: PAL B/G", kAtvG, kAtvPal},
        {"atvpali", "Analog TV: PAL I", kAtvI, kAtvPal},
        {"atvpaldk", "Analog TV: PAL D/K", kAtvDK, kAtvPal},
        {"atvsecamdk", "Analog TV: SECAM D/K", kAtvDK, kAtvSecam},
        {"atvsecaml", "Analog TV: SECAM L", kAtvL, kAtvSecam},
        {"atvntscm", "Analog TV: NTSC M", kAtvM, kAtvNtsc},
        {"atvpalm", "Analog TV: PAL M", kAtvM, kAtvPal},
        {"atvpaln", "Analog TV: PAL N", kAtvN, kAtvPal},
    };
    const int nv = (int)(sizeof views / sizeof *views);

    for (int i = 0; i < nv; i++) {
        const V& v = views[i];
        AtvFormat f; atvMakeFormat(v.sys, v.colour, f);
        for (double cfo : {0.0, i % 2 ? 50e3 : -50e3}) {
            const Run r = runView(v.sys, v.colour, v.sys, v.colour, cfo, 2.5);
            const AtvTelemetry& t = r.tel;
            double bars = 9;
            if (r.frame && r.frame->width == f.picW && r.frame->height == f.picH) bars = barError(*r.frame, f);
            printf("  %-24s cfo %+6.0f: state %d, '%s', %d lines, field %.3f Hz, offset %+.0f Hz, fields ok %llu bad %llu, bars %.3f\n", v.name, cfo, t.state, t.system.c_str(), t.lines, t.fieldHz, t.cfoHz, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, bars);
            CHECK(t.state == 2 && t.dataValid, "%s cfo %.0f: not locked (state %d)", v.name, cfo, t.state);
            CHECK(t.system == f.name && t.lines == f.lines, "%s: found '%s' with %d lines, expected '%s'", v.name, t.system.c_str(), t.lines, f.name.c_str());
            CHECK(std::fabs(t.fieldHz - f.fieldHz) < 0.02, "%s: field rate %.4f", v.name, t.fieldHz);
            CHECK(t.blocksOk > 20 && t.blocksBad <= 3, "%s: fields ok %llu bad %llu", v.name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
            CHECK(std::fabs(t.cfoHz - cfo) < 1500, "%s: offset %.0f Hz, sent %.0f", v.name, t.cfoHz, cfo);
            CHECK(bars < 0.14, "%s: colour bars off by %.3f", v.name, bars);
        }
    }
    return testSummary("atv views");
}
