// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Zigbee / IEEE 802.15.4 monitor: the test signal through the receiver (zigbee_gen.h, zigbee_rx.h): clean frames, noise near the threshold, carrier
// offsets of +-100 kHz (+-40 ppm at 2.4 GHz), chip clock error and timing offsets, an adjacent-channel transmitter, back-to-back frames, a
// wrong FCS, another sample rate, and noise alone.
#include "dect2/dsp/gen_util.h"
#include "dect2/zigbee/zigbee_gen.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zigbee/zigbee_tel.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::zigbee;
#include "../common/test_util.h"

static ZigbeeTelemetry run(ZigbeeGenConfig c, double secs, double centerHz = 0) {
    ZigbeeGenerator g(c);
    ZigbeeReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dialOffsetHz);   // as the engine does: minus the tuning offset
    r.setCenterHz(centerHz);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(secs * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    ZigbeeTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static ZigbeeGenFrame dataFrame(uint8_t seq, size_t extra = 20) {
    MacBuild b; b.type = Data; b.destPan = 0x1A62; b.destShort = 0; b.srcShort = 0x1234; b.seq = seq;
    b.payload.assign(extra, 0);
    for (size_t i = 0; i < extra; i++) b.payload[i] = (uint8_t)(i * 37 + seq);
    ZigbeeGenFrame f;
    f.psdu = buildMac(b);
    f.gapSec = 0.001;
    return f;
}

// how many frames the schedule sends in this time
static int sentIn(const ZigbeeGenConfig& c, double secs) {
    double per = 0;
    for (const auto& f : c.frames) per += (double)(f.psdu.size() + 6) * 32e-6 + f.gapSec + 8 / 2e6;
    return (int)(secs / per * (double)c.frames.size());
}

static const ZbDevice* findShort(const ZigbeeTelemetry& t, const char* a) { for (const auto& d : t.devices) if (d.shortAddr == a) return &d; return nullptr; }

int main() {
    {   // clean frames, 4 Msps, the whole schedule: every layer shows up
        ZigbeeGenConfig c;
        c.snrDb = 30;
        c.dialOffsetHz = -zigbeeTuning().tuneOffsetHz;
        c.frames = zigbeeTestSchedule();
        const ZigbeeTelemetry t = run(c, 0.5, channelMhz(11) * 1e6 - c.dialOffsetHz);
        printf("  schedule: %llu good, %llu bad, %zu PANs, %zu devices, channel %d, SNR %.1f dB\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.pans.size(), t.devices.size(), t.channel, t.snrDb);
        CHECK(t.blocksBad == 0 && t.blocksOk > 100, "%llu good %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.channel == 11, "channel %d", t.channel);
        CHECK(t.pans.size() == 2, "%zu PANs", t.pans.size());
        const ZbPan* a = nullptr; const ZbPan* b = nullptr;
        for (const auto& p : t.pans) { if (p.pan == 0x1A62) a = &p; if (p.pan == 0x3C2D) b = &p; }
        CHECK(a && a->zigbee && a->extPan == "DD:DD:DD:DD:DD:DD:1A:62" && a->joinOpen && a->stackProfile == 2 && a->coordinator == "0x0000" && a->channel == 11, "PAN 1A62: %s", a ? a->extPan.c_str() : "-");
        CHECK(b && b->extPan == "A1:B2:C3:D4:E5:F6:07:18" && !b->joinOpen, "PAN 3C2D");
        const ZbDevice* hue = findShort(t, "0x1234");
        CHECK(hue && hue->extAddr == "00:17:88:01:0A:0B:0C:0D" && hue->vendor.find("Philips") != std::string::npos, "Hue bulb: %s / %s", hue ? hue->extAddr.c_str() : "-", hue ? hue->vendor.c_str() : "-");
        bool lumi = false, ti = false, ikea = false;
        for (const auto& d : t.devices) {
            if (d.vendor.find("Lumi") == 0) lumi = true;
            if (d.vendor == "Texas Instruments") ti = true;
            if (d.vendor.find("IKEA") != std::string::npos) ikea = true;
        }
        CHECK(lumi && ti && ikea, "vendors: Lumi %d TI %d IKEA %d", lumi, ti, ikea);
        bool beacon = false, assoc = false, annce = false, enc = false, req = false, link = false;
        for (const auto& f : t.log) {
            if (f.info.find("Zigbee beacon: PAN 0x1A62") == 0) beacon = true;
            if (f.info.find("Association response: address 0x1234, joined") == 0) assoc = true;
            if (f.info.find("cluster 0x0013 Device_annce") != std::string::npos) annce = true;
            if (f.info.find("payload encrypted (network key)") != std::string::npos && f.encrypted) enc = true;
            if (f.info == "Beacon request") req = true;
            if (f.info.find("Link status") != std::string::npos) link = true;
        }
        CHECK(beacon && assoc && annce && enc && req && link, "log: beacon %d assoc %d annce %d enc %d req %d link %d", beacon, assoc, annce, enc, req, link);
        CHECK(t.encryptedFrames > 0 && t.perType[Beacon] > 0 && t.perType[Data] > 0 && t.perType[Ack] > 0 && t.perType[Command] > 0, "frame types");
        CHECK(t.perChannel[0] == t.blocksOk, "per channel");
    }
    {   // the clock: a 127-byte frame with a 40 ppm clock error either way (the frame lasts 4 ms: a third of a chip of drift)
        ZigbeeGenConfig c;
        c.snrDb = 30; c.clockPpm = 40;
        c.frames = {dataFrame(1, 110)};
        const ZigbeeTelemetry t = run(c, 0.5);
        const int sent = sentIn(c, 0.5);
        printf("  127 bytes, +40 ppm: %llu of about %d, %llu bad\n", (unsigned long long)t.blocksOk, sent, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk >= (uint64_t)(sent * 0.9) && t.blocksBad == 0, "40 ppm: %llu good", (unsigned long long)t.blocksOk);
        c.clockPpm = -40; c.seed = 3;
        const ZigbeeTelemetry t2 = run(c, 0.5);
        CHECK(t2.blocksOk >= (uint64_t)(sent * 0.9) && t2.blocksBad == 0, "-40 ppm: %llu good", (unsigned long long)t2.blocksOk);
    }
    // carrier offset +-100 kHz (+-40 ppm of 2.4 GHz is 96 kHz) with noise, two sample rates
    for (double rate : {4e6, 10e6}) for (double cfo : {100e3, -100e3, 0.0}) {
        ZigbeeGenConfig c;
        c.rate = rate; c.snrDb = 15; c.cfoHz = cfo; c.clockPpm = cfo > 0 ? 25 : -25; c.seed = (uint32_t)(105 + cfo / 1e4);
        c.dialOffsetHz = -zigbeeTuning().tuneOffsetHz;
        c.frames = {dataFrame(2, 30), dataFrame(3, 12)};
        const ZigbeeTelemetry t = run(c, 0.5);
        const int sent = sentIn(c, 0.5);
        printf("  %.0f Msps, %+.0f kHz, 15 dB: %llu of about %d, %llu bad, SNR %.1f dB, offset %+.1f kHz\n", rate / 1e6, cfo / 1e3, (unsigned long long)t.blocksOk, sent, (unsigned long long)t.blocksBad, t.snrDb, t.cfoHz / 1e3);
        CHECK(t.blocksOk >= (uint64_t)(sent * 0.9) && t.blocksBad <= 2, "%.0f Msps %+.0f kHz: %llu good", rate / 1e6, cfo / 1e3, (unsigned long long)t.blocksOk);
    }
    {   // near the threshold: the spreading gain (32 chips for 4 bits) works down to a few dB
        ZigbeeGenConfig c;
        c.snrDb = 4; c.seed = 11;
        c.frames = {dataFrame(4, 20)};
        const ZigbeeTelemetry t = run(c, 1.0);
        const int sent = sentIn(c, 1.0);
        printf("  4 dB: %llu of about %d, %llu bad\n", (unsigned long long)t.blocksOk, sent, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk >= (uint64_t)(sent * 0.5), "4 dB: %llu good", (unsigned long long)t.blocksOk);
        for (const auto& f : t.log) CHECK(f.fcsOk, "FCS");
    }
    {   // an adjacent-channel transmitter (5 MHz away, 20 dB stronger) with 12 Msps
        ZigbeeGenConfig c;
        c.rate = 12e6; c.snrDb = 25; c.interfererDb = 20; c.interfererHz = 5e6;
        c.dialOffsetHz = -zigbeeTuning().tuneOffsetHz;
        c.frames = {dataFrame(5, 30)};
        const ZigbeeTelemetry t = run(c, 0.3);
        const int sent = sentIn(c, 0.3);
        printf("  adjacent channel +20 dB: %llu of about %d\n", (unsigned long long)t.blocksOk, sent);
        CHECK(t.blocksOk >= (uint64_t)(sent * 0.8), "adjacent channel: %llu good", (unsigned long long)t.blocksOk);
    }
    {   // back to back: the shortest gap, and frames of different length in a row
        ZigbeeGenConfig c;
        c.snrDb = 25; c.seed = 21;
        for (int i = 0; i < 5; i++) { ZigbeeGenFrame f = dataFrame((uint8_t)(10 + i), 5 + 20 * (size_t)i); f.gapSec = 0.00019; c.frames.push_back(f); }
        const ZigbeeTelemetry t = run(c, 0.5);
        const int sent = sentIn(c, 0.5);
        printf("  back to back: %llu of about %d, %llu bad\n", (unsigned long long)t.blocksOk, sent, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk >= (uint64_t)(sent * 0.9) && t.blocksBad == 0, "back to back: %llu good", (unsigned long long)t.blocksOk);
    }
    {   // a wrong FCS is rejected (counted, not shown), the good frames around it are not
        ZigbeeGenConfig c;
        c.snrDb = 99;
        ZigbeeGenFrame bad = dataFrame(6, 18);
        bad.badFcs = true;
        c.frames = {dataFrame(7, 18), bad};
        const ZigbeeTelemetry t = run(c, 0.25);
        printf("  bad FCS: %llu good, %llu bad, %zu in the log\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.log.size());
        CHECK(t.blocksBad >= 20 && t.blocksOk >= 20, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.log.size() == t.blocksOk && !t.log.empty(), "log has only good frames (%zu, %llu)", t.log.size(), (unsigned long long)t.blocksOk);
    }
    {   // noise alone: nothing
        ZigbeeReceiver r;
        r.configure(4e6);
        std::vector<cf32> buf(16384);
        genutil::NoiseSource ns(9);
        for (int i = 0; i < 4 * 4000000 / 16384; i++) { for (auto& v : buf) v = cf32(0, 0); ns.add(buf.data(), buf.size(), 0.1f); r.feed(buf.data(), buf.size()); }
        ZigbeeTelemetry t;
        r.telemetry(t, 0);
        printf("  noise: %llu preambles, %llu good, %llu bad\n", (unsigned long long)t.preambles, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.devices.empty() && t.blocksOk == 0 && t.log.empty() && t.pans.empty(), "noise gave %zu devices", t.devices.size());
    }
    {   // the results survive a save and a load
        ZigbeeGenConfig c;
        c.snrDb = 30;
        c.frames = zigbeeTestSchedule();
        ZigbeeGenerator g(c);
        ZigbeeReceiver r;
        r.configure(c.rate);
        std::vector<cf32> buf(16384);
        for (size_t d = 0; d < 1000000; d += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        ZigbeeTelemetry t;
        r.telemetry(t, 0);
        const std::string blob = r.saveResults();
        ZigbeeReceiver r2;
        r2.configure(4e6);
        CHECK(r2.loadResults(blob) && !r2.loadResults("garbage"), "load");
        ZigbeeTelemetry t2;
        r2.telemetry(t2, 0);
        CHECK(t2.pans.size() == t.pans.size() && t2.devices.size() == t.devices.size() && t2.log.size() == t.log.size() && t2.devices.size() >= 5, "restored %zu devices", t2.devices.size());
        r2.clearResults();
        ZigbeeTelemetry t3;
        r2.telemetry(t3, 0);
        CHECK(t3.pans.empty() && t3.devices.empty() && t3.log.empty(), "cleared");
    }
    printf(fails ? "zigbee rx: %d FAILED\n" : "zigbee rx: all passed\n", fails);
    return fails ? 1 : 0;
}
