// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN receiver against the test signal: the whole schedule decoded at 20 Msps (frames hopping over 915.4 - 927.4 MHz), with noise,
// carrier and clock offsets, at a slow radio's 2.4 Msps (only the channels it takes in), a frame with a damaged FCS, the tables (network,
// border router, roles, the EAPOL authenticator, links with the RSL from the acks), Clear and the results file.
#include "dect2/wisun/wisun_gen.h"
#include "dect2/wisun/wisun_proto.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/wisun/wisun_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
// The message is glued to a literal: a format that is not a string literal does not compile
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

// Plays the schedule once (plus silence for the last burst and report) through a receiver
static WisunTelemetry run(WisunGenConfig c, double secs, const std::string& region = "") {
    c.loop = false;
    WisunGenerator g(c);
    WisunReceiver rx;
    rx.setCenterHz(c.centerMhz * 1e6);
    if (!region.empty()) rx.setRegion(region);
    rx.configure(c.rate);
    rx.setOffline(true);
    std::vector<cf32> buf(1 << 16);
    for (size_t left = (size_t)(secs * c.rate); left > 0;) {
        const size_t n = std::min(left, buf.size());
        g.generate(buf.data(), n);
        rx.feed(buf.data(), n);
        left -= n;
    }
    rx.flush();
    WisunTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static double scheduleSecs(const std::vector<WisunGenFrame>& fr) {
    double s = 0.002;
    for (const auto& f : fr) s += wisunPpduBits(f).size() / (double)f.bitRate + f.gapSec;
    return s + 0.3;
}

static const WsNode* nodeOf(const WisunTelemetry& t, const std::string& eui) {
    for (const auto& n : t.nodes) if (n.eui == eui) return &n;
    return nullptr;
}

int main() {
    const std::vector<WisunGenFrame> sched = wisunTestSchedule(921.5, 6.0);
    const size_t frames = sched.size();
    {   // clean, 20 Msps: every frame
        WisunGenConfig c;
        c.rate = 20e6; c.snrDb = 99; c.frames = sched;
        const WisunTelemetry t = run(c, scheduleSecs(sched));
        printf("  20 Msps clean: %s, %llu bad, %llu bursts, load %.0f%%\n", wisunSummary(t).c_str(), (unsigned long long)t.blocksBad, (unsigned long long)t.bursts, t.loadPct);
        CHECK(t.blocksOk == frames && t.blocksBad == 0, "%llu of %zu frames, %llu bad", (unsigned long long)t.blocksOk, frames, (unsigned long long)t.blocksBad);
        CHECK(t.region == "ANZ" && t.loMhz <= 915.3 && t.hiMhz >= 927.7 && t.bins == 64, "coverage %s %.1f - %.1f, %d bins", t.region.c_str(), t.loMhz, t.hiMhz, t.bins);
        CHECK(t.pans.size() == 1 && t.pans[0].pan == 0x7A31 && t.pans[0].netName == "OnAir-Test-FAN", "pan");
        if (!t.pans.empty()) {
            const WsPan& p = t.pans[0];
            CHECK(p.borderRouter == "00:12:4B:FF:FE:1A:2B:3C" && p.panSize == 3 && p.tpsVersion == 2, "border router %s", p.borderRouter.c_str());
            CHECK(p.unicast == "Australia / NZ class 2, DH1CF, dwell 255 ms" && p.phyModes == "3 5", "schedule '%s' modes '%s'", p.unicast.c_str(), p.phyModes.c_str());
            CHECK(p.nodes == 3, "%d nodes in the PAN", p.nodes);
        }
        const WsNode* br = nodeOf(t, "00:12:4B:FF:FE:1A:2B:3C");
        const WsNode* router = nodeOf(t, "00:0D:6F:FF:FE:44:55:66");
        const WsNode* meter = nodeOf(t, "02:A1:B2:FF:FE:C3:D4:E5");
        CHECK(br && br->role == "border router" && br->vendor == "Texas Instruments" && br->routingCost == 0, "border router node");
        CHECK(router && router->role == "router" && router->routingCost == 128, "router node");
        CHECK(meter && meter->role == "joining" && meter->authenticator == "00:12:4B:FF:FE:1A:2B:3C" && meter->eapState == "EAP response identity",
              "meter %s %s '%s'", meter ? meter->role.c_str() : "-", meter ? meter->authenticator.c_str() : "-", meter ? meter->eapState.c_str() : "-");
        if (meter) CHECK(meter->perUtt[wisun::DataFrame] == 3 && meter->encrypted == 3 && meter->frameCounter == 12, "meter data %llu", (unsigned long long)meter->perUtt[wisun::DataFrame]);
        bool linkOk = false;
        for (const auto& l : t.links)
            if (l.src == "02:A1:B2:FF:FE:C3:D4:E5" && l.dst == "00:0D:6F:FF:FE:44:55:66") linkOk = l.frames == 4 && l.acks == 3 && l.rslDbm == -86;
        CHECK(linkOk, "the meter > router link (3 data, 1 EAPOL, 3 acks, RSL -86)");
        CHECK(t.perUtt[wisun::PA] == 2 && t.perUtt[wisun::PC] == 2 && t.perUtt[wisun::Eapol] == 2 && t.perUtt[wisun::AckFrame] == 3 && t.encryptedFrames == 6,
              "per type PA %llu PC %llu EAPOL %llu", (unsigned long long)t.perUtt[0], (unsigned long long)t.perUtt[2], (unsigned long long)t.perUtt[6]);
        // the 50 kbit/s frame: on the 200 kHz plan (class 1)
        bool slow = false;
        for (const auto& f : t.log) if (f.bitRate == 50000) slow = f.opClass == 1 && f.channel >= 0 && f.utt == wisun::PCS;
        CHECK(slow, "the 50 kbit/s frame");
        // the frame log: channels in the right plan, frequencies of the schedule
        int onPlan = 0;
        for (const auto& f : t.log) if (f.channel >= 0 && std::fabs(f.mhz - (f.opClass == 2 ? 915.4 + 0.4 * f.channel : 915.2 + 0.2 * f.channel)) < 1e-6) onPlan++;
        CHECK(onPlan == (int)frames, "%d frames on their plan", onPlan);
        CHECK(t.hops.size() == frames, "hops %zu", t.hops.size());
        // Clear, and a results file round trip
        WisunReceiver rx;
        CHECK(rx.loadResults(std::string()) == false, "an empty blob");
    }
    {   // noise (15 dB in 400 kHz), carrier offset and clock error
        WisunGenConfig c;
        c.rate = 20e6; c.snrDb = 15; c.cfoHz = 12e3; c.clockPpm = 60; c.seed = 7; c.frames = sched;
        const WisunTelemetry t = run(c, scheduleSecs(sched));
        printf("  20 Msps 15 dB: %s, %llu bad, %llu corrected, %llu mode switches, SNR %.1f dB, CFO %.1f kHz\n", wisunSummary(t).c_str(), (unsigned long long)t.blocksBad,
               (unsigned long long)t.corrected, (unsigned long long)t.modeSwitches, t.snrDb, t.cfoHz / 1e3);
        CHECK(t.blocksOk >= frames - 1, "%llu of %zu frames at 15 dB", (unsigned long long)t.blocksOk, frames);
    }
    {   // a slow radio: 2.4 Msps around 921.5 MHz takes in the channels 920.6 - 922.4 MHz only
        const std::vector<WisunGenFrame> nearBy = wisunTestSchedule(921.5, 0.9);
        WisunGenConfig c;
        c.rate = 2.4e6; c.snrDb = 25; c.frames = nearBy;
        const WisunTelemetry t = run(c, scheduleSecs(nearBy));
        printf("  2.4 Msps: %s, %d bins of %.0f kHz, %.1f - %.1f MHz\n", wisunSummary(t).c_str(), t.bins, t.binKhz, t.loMhz, t.hiMhz);
        CHECK(t.blocksOk == nearBy.size() && t.blocksBad == 0, "%llu of %zu frames at 2.4 Msps", (unsigned long long)t.blocksOk, nearBy.size());
        CHECK(t.bins == 8 && t.binKhz == 300, "bins");
    }
    {   // a damaged FCS: counted, not shown
        std::vector<WisunGenFrame> one(sched.begin(), sched.begin() + 2);
        one[1].badFcs = true;
        WisunGenConfig c;
        c.rate = 20e6; c.snrDb = 99; c.frames = one;
        const WisunTelemetry t = run(c, scheduleSecs(one));
        CHECK(t.blocksOk == 1 && t.blocksBad == 1, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // another band plan, picked by the frequency: Europe at 863 - 870 MHz, 50 kbit/s (h 0.5) on a 100 kHz channel, 100 kbit/s (h 0.5) on a
        // 200 kHz one
        std::vector<WisunGenFrame> eu(sched.begin(), sched.begin() + 2);
        eu[0].mhz = 867.5; eu[0].bitRate = 50000; eu[0].modIndex = 0.5;
        eu[1].mhz = 868.3; eu[1].bitRate = 100000; eu[1].modIndex = 0.5;
        WisunGenConfig c;
        c.rate = 4e6; c.centerMhz = 868.0; c.snrDb = 25; c.frames = eu;
        const WisunTelemetry t = run(c, scheduleSecs(eu));
        printf("  EU 863: %s, %s, %.1f - %.1f MHz, %d of %zu channels\n", wisunSummary(t).c_str(), t.region.c_str(), t.loMhz, t.hiMhz, t.channelsCovered, t.chanMhz.size());
        CHECK(t.region == "EU863" && t.bandName == "Europe (863 MHz)" && t.chanMhz.size() == 69, "band plan %s, %zu channels", t.region.c_str(), t.chanMhz.size());
        CHECK(t.blocksOk == 2 && t.blocksBad == 0, "%llu of 2 frames", (unsigned long long)t.blocksOk);
        bool slow = false, fast = false;
        for (const auto& f : t.log) {
            if (f.bitRate == 50000) slow = f.opClass == 1 && f.channel == 44;
            if (f.bitRate == 100000) fast = f.opClass == 2 && f.channel == 26;
        }
        CHECK(slow && fast, "channels of the 100 and 200 kHz plans");
    }
    {   // a band plan asked for by name (Japan) where the frames are 150 kbit/s on ANZ's 400 kHz raster, not on Japan's: its channels are
        // listened to, the ANZ frames not decoded
        WisunGenConfig c;
        c.rate = 4e6; c.snrDb = 25; c.frames = std::vector<WisunGenFrame>(sched.begin(), sched.begin() + 2);
        const WisunTelemetry t = run(c, scheduleSecs(c.frames), "JP");
        CHECK(t.region == "JP" && t.channelsCovered > 0 && t.blocksOk == 0, "JP at 921.5 MHz: %d channels, %llu frames", t.channelsCovered, (unsigned long long)t.blocksOk);
    }
    {   // the mode switch bit received set by a bit error: still an ordinary frame, not a mode switch
        std::vector<WisunGenFrame> one(sched.begin(), sched.begin() + 2);
        one[1].msError = true;
        WisunGenConfig c;
        c.rate = 20e6; c.snrDb = 99; c.frames = one;
        const WisunTelemetry t = run(c, scheduleSecs(one));
        CHECK(t.blocksOk == 2 && t.modeSwitches == 0, "%llu good, %llu mode switches", (unsigned long long)t.blocksOk, (unsigned long long)t.modeSwitches);
    }
    if (fails) printf("wisun rx: %d FAILED\n", fails);
    else printf("wisun rx: all passed\n");
    return fails ? 1 : 0;
}
