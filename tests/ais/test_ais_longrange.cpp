// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Long-range AIS: message 27 encode / decode with its CRC (ITU-R M.1371-5 table 79), the channel plan, and the receiver decoding all four channels
// (AIS 1, AIS 2, 75, 76) from one 6 Msps capture at 159.4 MHz, or the long-range pair alone from a 2 Msps radio at 156.8 MHz, with time stamps.
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_longrange.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/ais/ais_testutil.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static ais::LongRangeReport randomReport(std::mt19937& rng) {
    ais::LongRangeReport r;
    r.repeat = (int)(rng() % 4);
    r.mmsi = aistest::randomMmsi(rng());
    r.posAccuracy = rng() & 1; r.raim = rng() & 1;
    r.navStatus = (int)(rng() % 16);
    r.lat = std::round(((double)(rng() % 1800000) / 10000.0 - 90.0) * 600.0) / 600.0;
    r.lon = std::round(((double)(rng() % 3600000) / 10000.0 - 180.0) * 600.0) / 600.0;
    r.sogKn = (int)(rng() % 64);
    r.cogDeg = (int)(rng() % 360);
    r.latencyOver5s = rng() & 1;
    return r;
}

static void roundTrip() {
    std::mt19937 rng(27);
    int ok = 0;
    for (int i = 0; i < 500; i++) {
        const ais::LongRangeReport r = randomReport(rng);
        ais::Bits b = ais::encodeLongRange(r);
        CHECK(b.size() == 96, "message 27 is 96 bits (%zu)", b.size());
        CHECK(ais::getU(b, 0, 6) == 27, "type field");
        // the frame on the air: FCS, stuffing, flags, NRZI; back through the HDLC layer
        const ais::Bits line = ais::burstLineBits(b);
        const ais::HdlcResult h = ais::hdlcFrames(ais::nrziDecode(ais::Bits(line.begin() + 24, line.end()), line[23]));
        CHECK(h.good.size() == 1 && h.good[0] == b, "HDLC round trip with a good FCS (%d)", i);
        ais::Bits withFcs = b; ais::reverseOctets(withFcs); ais::appendFcs(withFcs);
        CHECK(ais::fcsOk(withFcs), "FCS good");
        withFcs[40] ^= 1;
        CHECK(!ais::fcsOk(withFcs), "a flipped bit fails the FCS");
        ais::LongRangeReport d;
        if (!ais::decodeLongRange(b, d)) { CHECK(false, "decodeLongRange %d", i); continue; }
        ais::AisMsg m;
        CHECK(ais::decodeMessage(b, m) && m.type == 27 && ais::acceptableAis(m, true), "decodeMessage accepts it");
        const bool same = d.mmsi == r.mmsi && d.repeat == r.repeat && d.navStatus == r.navStatus && d.posAccuracy == r.posAccuracy && d.raim == r.raim &&
                          std::fabs(d.lat - r.lat) < 1e-9 && std::fabs(d.lon - r.lon) < 1e-9 && d.cogDeg == r.cogDeg &&
                          d.sogKn == r.sogKn && d.latencyOver5s == r.latencyOver5s;
        CHECK(same, "fields %d: mmsi %u/%u lat %.5f/%.5f lon %.5f/%.5f sog %d/%d cog %d/%d nav %d/%d lat5 %d/%d", i, d.mmsi, r.mmsi, d.lat, r.lat, d.lon, r.lon,
              d.sogKn, r.sogKn, d.cogDeg, r.cogDeg, d.navStatus, r.navStatus, (int)d.latencyOver5s, (int)r.latencyOver5s);
        ok += same;
    }
    CHECK(ok == 500, "round trips %d / 500", ok);
    // resolution 1/10 minute, "not available" values
    ais::LongRangeReport r; r.mmsi = 244123456; r.lat = 54.6872; r.lon = 25.2797; r.sogKn = 80; r.cogDeg = 400; r.latencyOver5s = false;
    ais::LongRangeReport d;
    CHECK(ais::decodeLongRange(ais::encodeLongRange(r), d), "decode");
    CHECK(std::fabs(d.lat - 54.6872) <= 1.0 / 1200 + 1e-9 && std::fabs(d.lon - 25.2797) <= 1.0 / 1200 + 1e-9, "position to 1/10 minute: %.5f %.5f", d.lat, d.lon);
    CHECK(d.sogKn == 63 && d.cogDeg == 511, "out of range speed / course become not available: %d %d", d.sogKn, d.cogDeg);
    CHECK(!d.latencyOver5s, "latency flag 0");
    ais::LongRangeReport na; na.mmsi = 244123456;
    ais::AisMsg m;
    CHECK(ais::decodeMessage(ais::encodeLongRange(na), m) && !m.hasPos && m.sog < 0 && m.cog < 0 && m.posLatency == 1, "all not available");
    ais::Bits shortB = ais::encodeLongRange(r); shortB.resize(90);
    CHECK(!ais::decodeLongRange(shortB, d), "a short message 27 is refused");
}

static void plan() {
    CHECK(ais::aisChannelsCovered(162.0e6, 2e6) == 3, "2 Msps at 162: AIS 1 + 2");
    CHECK(ais::aisChannelsCovered(156.8e6, 2e6) == 12, "2 Msps at 156.8: 75 + 76");
    CHECK(ais::aisChannelsCovered(ais::kAisWideCentreHz, 6e6) == 15, "6 Msps at 159.4: all four");
    CHECK(ais::aisChannelsCovered(ais::kAisWideCentreHz, 5e6) == 0, "5 Msps at 159.4: neither pair");
    CHECK(ais::aisWideRateHz() < 5.5e6 && ais::aisWideRateHz() > 5.3e6, "wide rate %.0f", ais::aisWideRateHz());
    const auto w = ais::aisTunePlan(8e6);
    CHECK(w.size() == 1 && w[0].channelMask == 15, "8 Msps: one step");
    const auto n = ais::aisTunePlan(2.4e6);
    CHECK(n.size() == 2 && n[0].channelMask == 3 && n[1].channelMask == 12 && n[1].dwellSec >= ais::kLongRangeIntervalSec, "2.4 Msps: hop between the pairs");
}

struct Sent { char ch; ais::Bits b; double t; };

static void fourChannels() {
    std::mt19937 rng(4);
    std::vector<AisBurstSpec> specs;
    std::vector<Sent> sent;
    const char chans[4] = {'A', 'B', 'C', 'D'};
    double t = 0.12;   // after the first noise-floor estimate (three blocks of 20 ms)
    for (int i = 0; i < 16; i++) {
        const char c = chans[i % 4];
        ais::Bits b;
        if (c == 'C' || c == 'D') b = ais::encodeLongRange(randomReport(rng));
        else b = aistest::randomPosition(rng);
        AisBurstSpec s; s.payload = b; s.channel = c; s.startSec = t; s.cfoHz = (double)(rng() % 600) - 300;
        specs.push_back(s); sent.push_back({c, b, t});
        t += 0.031 + 0.01 * (i % 3);
    }
    AisRenderConfig rc; rc.rate = 6e6; rc.durationSec = t + 0.3; rc.snrDb = 25;
    rc.tuneOffsetHz = ais::kAisWideCentreHz - ais::kAisMidHz;   // the capture's centre at 159.4 MHz: AIS 1 at +2.575 MHz, channel 75 at -2.625 MHz
    const std::vector<cf32> x = renderAisBursts(specs, rc);
    AisReceiver rx;
    rx.setCenterHz(ais::kAisWideCentreHz);
    rx.setTimeBase(1.8e9);
    rx.configure(rc.rate);
    CHECK(rx.ready(), "ready at 6 Msps");
    aistest::Collector col;
    const AisTelemetry tl = aistest::runReceiver(rx, x, 65536, &col);
    CHECK(tl.channelMask == 15, "all four channels covered (mask %d)", tl.channelMask);
    int found = 0, perCh[4] = {};
    for (const Sent& s : sent) {
        bool hit = false;
        for (const AisRxRecord& r : tl.recent) {
            if (r.channel != s.ch || r.mmsi != ais::getU(s.b, 8, 30)) continue;
            hit = true;
            CHECK(std::fabs(r.utc - (1.8e9 + s.t)) < 0.005, "time stamp of %c: %.4f vs %.4f", s.ch, r.utc - 1.8e9, s.t);
            if (s.ch >= 'C') CHECK(r.type == 27, "type 27 on the long-range channel");
        }
        found += hit;
        if (hit) perCh[aisChannelIndex(s.ch)]++;
    }
    CHECK(found == (int)sent.size(), "decoded %d / %zu (A %d B %d 75 %d 76 %d)", found, sent.size(), perCh[0], perCh[1], perCh[2], perCh[3]);
    CHECK(tl.typeCount[2][27] + tl.typeCount[3][27] == 8, "type 27 counted on 75 / 76: %llu %llu", (unsigned long long)tl.typeCount[2][27], (unsigned long long)tl.typeCount[3][27]);
    CHECK(tl.longRangeVessels == 8, "eight long-range (new-type) vessels: %u", tl.longRangeVessels);
    int lrPos = 0;
    for (const AisVessel& v : tl.vessels) if (v.longRange) { lrPos += v.lrPosition && v.hasPos; CHECK(v.channel == 'C' || v.channel == 'D', "channel letter"); CHECK(v.lastUtc > 1.8e9, "last time"); }
    CHECK(lrPos == 8, "long-range positions shown: %d", lrPos);
    bool nmeaC = false;
    for (const std::string& s : tl.nmea) nmeaC |= s.find(",C,") != std::string::npos;
    CHECK(nmeaC, "!AIVDM sentences of channel 75 carry the letter C");
}

static void longRangeOnly() {
    std::mt19937 rng(76);
    std::vector<AisBurstSpec> specs;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        AisBurstSpec s; s.payload = ais::encodeLongRange(randomReport(rng)); s.channel = i & 1 ? 'D' : 'C'; s.startSec = 0.05 + 0.04 * i;
        specs.push_back(s); n++;
        AisBurstSpec a; a.payload = aistest::randomPosition(rng); a.channel = 'A'; a.startSec = 0.06 + 0.04 * i;   // AIS 1: outside this capture
        specs.push_back(a);
    }
    AisRenderConfig rc; rc.rate = 2e6; rc.durationSec = 0.7; rc.snrDb = 25;
    rc.tuneOffsetHz = ais::kLongRangeMidHz - ais::kAisMidHz;   // a 2 Msps radio at 156.8 MHz
    const std::vector<cf32> x = renderAisBursts(specs, rc);
    AisReceiver rx;
    rx.setCenterHz(ais::kLongRangeMidHz);
    rx.configure(rc.rate);
    const AisTelemetry tl = aistest::runReceiver(rx, x);
    CHECK(tl.channelMask == 12, "only 75 + 76 (mask %d)", tl.channelMask);
    CHECK(tl.typeCount[2][27] + tl.typeCount[3][27] == (uint64_t)n, "long-range messages %llu / %d", (unsigned long long)(tl.typeCount[2][27] + tl.typeCount[3][27]), n);
    CHECK(tl.channelOk[0] == 0 && tl.channelOk[1] == 0, "nothing on AIS 1 / 2");
    // the old placement still works: no centre, AIS 1 and 2 at -+25 kHz
    AisRenderConfig r2; r2.rate = 2e6; r2.durationSec = 0.7; r2.snrDb = 25;
    const std::vector<cf32> y = renderAisBursts(specs, r2);
    AisReceiver rx2;
    rx2.configure(r2.rate);
    const AisTelemetry t2 = aistest::runReceiver(rx2, y);
    CHECK(t2.channelOk[0] == 8 && t2.channelMask == 3, "legacy placement: %llu on AIS 1 (mask %d)", (unsigned long long)t2.channelOk[0], t2.channelMask);
}

int main() {
    roundTrip();
    plan();
    fourChannels();
    longRangeOnly();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
