// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell buoy HF link: the synthetic signals through the receiver (datawell_gen.h, datawell_rx.h).
//  - 2FSK carries HXV vectors built from the documented layout: carrier detection, symbol clock, vector lock, displacements, system words and the
//    spectrum points are checked against the numbers that went in, under an offset, a clock error, inverted tones, a low SNR, the WR-SG vector.
//  - 4FSK carries a synthetic test frame of our own (the real over-the-air format is not public): only the modem and the frame finder are tested.
// Noise alone, a plain carrier, other sample rates, a wrongly forced format and the channel names.
#include "dect2/datawell/datawell_gen.h"
#include "dect2/datawell/datawell_rx.h"
#include "dect2/datawell/datawell_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static DatawellTelemetry run(DatawellGenConfig c, double secs, double signalOffsetHz = 0, double centerHz = 0, int fmt = 0) {
    DatawellGenerator g(c);
    DatawellReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(signalOffsetHz);
    r.setCenterHz(centerHz);
    r.setFormat(fmt);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.3) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    DatawellTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static void show(const char* name, const DatawellTelemetry& t) {
    printf("  %s: state %d %s bw %.0f Hz off %+.1f snr %.1f baud %.3f (%+.0f ppm) step %.1f err %.2f | hxv lock %d inv %d vectors %llu clean %llu repaired %llu lost %llu blocks %llu | period %d sync %s\n", name,
           t.state, t.format.c_str(), t.bwHz, t.offsetHz, t.snrDb, t.baud, t.clockPpm, t.stepHz, t.symErr, t.hxvLocked, t.hxvInverted, (unsigned long long)t.hxvVectors,
           (unsigned long long)t.hxvClean, (unsigned long long)t.hxvRepaired, (unsigned long long)t.hxvLost, (unsigned long long)t.hxvBlocks, t.periodBits, t.syncHex.c_str());
}

// what a 2FSK run of the given length must have decoded
static void expectHxv(const char* name, const DatawellTelemetry& t, double offset, bool inverted, bool wrsg, int minVectors, int minSpec) {
    show(name, t);
    CHECK(t.state == 2 && t.format == "2FSK", "%s: state %d format '%s'", name, t.state, t.format.c_str());
    CHECK(std::fabs(t.offsetHz - offset) < 30, "%s: centre %+.1f Hz (want %+.0f)", name, t.offsetHz, offset);
    CHECK(std::fabs(t.baud - 81.92) < 0.3, "%s: %.3f baud", name, t.baud);
    CHECK(t.hxvLocked && t.hxvInverted == inverted, "%s: lock %d inverted %d", name, t.hxvLocked, t.hxvInverted);
    CHECK((int)t.hxvVectors >= minVectors && t.hxvLost <= 3, "%s: %llu vectors, %llu lost", name, (unsigned long long)t.hxvVectors, (unsigned long long)t.hxvLost);
    CHECK(t.dataValid, "%s: no valid data", name);
    int agree = 0, total = 0;
    for (size_t i = 0; i + 1 < t.disp.size(); i++) {
        // consecutive vectors are 0.78 s apart: the heave of the test waves is smooth to within a few cm
        total++;
        if (std::abs(t.disp[i + 1].h - t.disp[i].h) < 110) agree++;
    }
    CHECK(total >= 10 && agree >= total - 2, "%s: displacements not smooth (%d of %d steps)", name, agree, total);
    if (!t.disp.empty()) CHECK(t.disp.back().wrsg == wrsg, "%s: WR-SG flag %d", name, t.disp.back().wrsg);
    CHECK(t.specCount >= minSpec, "%s: %d spectrum points", name, t.specCount);
    for (int n = 0; n < 64; n++) {
        if (!t.spec[n].valid) continue;
        const DwTestSpecRaw r = dwTestSpecRaw(n);
        CHECK(std::fabs(t.spec[n].dirDeg - r.dir * 360.0 / 256) < 1e-6 && std::fabs(t.spec[n].k - r.k / 100.0) < 1e-6, "%s: spectrum point %d wrong", name, n);
    }
    for (int i = 0; i < 16; i++) if (t.sys.have[i]) CHECK(i != 4 || std::fabs(t.sys.tr - 25) < 1e-9, "%s: Tr %.2f", name, t.sys.tr);
}

int main() {
    DatawellGenConfig c;
    c.rate = 250e3;
    c.snrDb = 24;
    {   // 2FSK / HXV: long enough for several blocks (18 vectors, 14 s each): the sea state words and the first spectrum points
        c.format = 0; c.offsetHz = 1500;
        const DatawellTelemetry t = run(c, 100);
        expectHxv("HXV", t, 1500, false, false, 100, 16);
        CHECK(t.sys.have[1] && std::fabs(t.sys.hs - 3.4) < 1e-9 && std::fabs(t.sys.tz - 400.0 / 56) < 1e-9, "Hs %.2f Tz %.2f", t.sys.hs, t.sys.tz);
        CHECK(t.sys.have[5] && std::fabs(t.sys.tw - 18.5) < 1e-9 && std::fabs(t.stepHz - 129) < 10, "Tw %.2f step %.1f", t.sys.tw, t.stepHz);
        CHECK(std::fabs(t.bwHz - 155) < 40, "width %.0f", t.bwHz);
    }
    {   // the radio's own offset, the channel name and the absolute frequency
        c.offsetHz = 20000;
        const DatawellTelemetry t = run(c, 30, 20000, 30.4750e6);
        expectHxv("offset", t, 20000, false, false, 30, 0);
        c.offsetHz = 1500;
    }
    {   // tone offset, inverted tones, WR-SG vectors, symbol clock off by 0.3 %
        c.offsetHz = -2300; c.invert = true; c.wrsg = true;
        DatawellTelemetry t = run(c, 40);
        expectHxv("inverted WR-SG at -2300 Hz", t, -2300, true, true, 40, 0);
        c.invert = false; c.wrsg = false; c.offsetHz = 1500; c.clockPpm = 3000;
        t = run(c, 40);
        expectHxv("clock +0.3%", t, 1500, false, false, 40, 0);
        CHECK(t.clockPpm > 1500 && t.clockPpm < 4500, "clock error %+.0f ppm", t.clockPpm);
        c.clockPpm = -3000;
        t = run(c, 40);
        expectHxv("clock -0.3%", t, 1500, false, false, 40, 0);
        c.clockPpm = 0;
    }
    {   // a lower SNR still decodes (repairs show up)
        c.snrDb = 15;
        const DatawellTelemetry t = run(c, 60);
        show("15 dB", t);
        CHECK(t.hxvLocked && t.hxvVectors >= 60 && t.hxvLost <= 8, "15 dB: lock %d, %llu vectors, %llu lost", t.hxvLocked, (unsigned long long)t.hxvVectors, (unsigned long long)t.hxvLost);
        c.snrDb = 24;
    }
    {   // 4FSK: our own test frame (the real format is not public): the modem and the frame finder
        DatawellGenConfig d = c; d.format = 1; d.offsetHz = -2300;
        const DatawellTelemetry t = run(d, 70);
        show("4FSK", t);
        CHECK(t.state == 2 && t.format == "4MFSK" && std::fabs(t.baud - 81.92) < 0.3 && std::fabs(t.stepHz - 78) < 10, "4FSK: state %d %s baud %.3f step %.1f", t.state, t.format.c_str(), t.baud, t.stepHz);
        CHECK(!t.hxvLocked && t.periodBits == kDwTestFrameBits && t.syncHex.compare(0, 4, "D391") == 0, "4FSK: period %d sync '%s'", t.periodBits, t.syncHex.c_str());
        CHECK(t.crcName == "CRC-16/CCITT-FALSE" && t.crcPass >= 3 && t.blocksOk >= 3, "4FSK: crc '%s' %d / %d, ok %llu", t.crcName.c_str(), t.crcPass, t.crcTotal, (unsigned long long)t.blocksOk);
    }
    {   // the format forced the wrong way round: 2FSK read as 4FSK finds no HXV vectors
        const DatawellTelemetry t = run(c, 40, 0, 0, 2);
        show("forced wrong", t);
        CHECK(t.format == "4MFSK" && t.formatForced && !t.hxvLocked && t.crcName.empty(), "forced format '%s' hxv %d crc '%s'", t.format.c_str(), t.hxvLocked, t.crcName.c_str());
    }
    {   // noise alone, and a plain carrier: nothing to report as data
        DatawellGenConfig n = c; n.noiseOnly = true;
        DatawellTelemetry t = run(n, 20);
        show("noise", t);
        CHECK(t.state == 0 && !t.hxvLocked && t.hxvVectors == 0, "noise: state %d", t.state);
        DatawellGenConfig k = c; k.carrierOnly = true;
        t = run(k, 20);
        show("carrier", t);
        CHECK(t.state <= 1 && !t.hxvLocked && t.bits.empty(), "carrier: state %d bits %zu", t.state, t.bits.size());
    }
    {   // other sample rates
        for (double fs : {48e3, 1e6}) {
            DatawellGenConfig d = c; d.rate = fs; d.offsetHz = 900;
            const DatawellTelemetry t = run(d, 40);
            show(fs < 100e3 ? "48 ksps" : "1 Msps", t);
            CHECK(t.hxvLocked && t.hxvVectors >= 40 && t.hxvLost <= 3, "%.0f sps: lock %d vectors %llu lost %llu", fs, t.hxvLocked, (unsigned long long)t.hxvVectors, (unsigned long long)t.hxvLost);
        }
    }
    {   // the listed channels
        CHECK(datawell::channelNear(30.4752) == "Default" && datawell::channelNear(35.3875) == "English Channel, UK" && datawell::channelNear(30.0).empty(), "channel names");
    }
    printf(fails ? "datawell rx: %d FAILED\n" : "datawell rx: all passed\n", fails);
    return fails ? 1 : 0;
}
