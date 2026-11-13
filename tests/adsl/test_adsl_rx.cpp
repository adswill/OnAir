// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-L receiver against the generator (adsl_gen.h) with real impairments: every channel and network parameter combination, carrier offset and clock error sweeps,
// a noise sweep, overlapping packets (same channel at different levels, adjacent channels at once), a garbled length byte, the capture centre and sample rate, an
// inverted spectrum, noise alone, and FLARM in the same capture. Loopback against our own generator, not on-air captures: no public ADS-L recording was found.
#include "dect2/adsl/adsl_gen.h"
#include "dect2/adsl/adsl_rx.h"
#include "dect2/adsl/adsl_tel.h"
#include "dect2/flarm/flarm_gen.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/flarm/flarm_tel.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

using namespace dect2;
using namespace dect2::adsl;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static constexpr double kCentre = 868.875e6, kRate = 2e6;

static Traffic ac(uint32_t id, int cat = 4, double lat = 54.7, double lon = 25.3) {
    Traffic t; t.amt = AmtFlarm; t.addr = id; t.category = cat; t.flightState = 2; t.emergency = 1;
    t.hasPos = true; t.lat = lat; t.lon = lon; t.hasAlt = true; t.altM = 1234; t.hasSpeed = true; t.speedMs = 31.5; t.hasClimb = true; t.climbMs = -1.25;
    t.hasTrack = true; t.trackDeg = 247.5; t.hAcc = 6; t.vAcc = 2; t.velAcc = 2;
    return t;
}

static AdslTx tx(double time, Band band, const std::vector<uint8_t>& net, double cfo = 0, double gainDb = 0) {
    AdslTx t; t.timeSec = time; t.band = band; t.net = net; t.cfoHz = cfo; t.gainDb = gainDb;
    return t;
}

struct Result { AdslTelemetry t; std::map<uint32_t, AdslAircraft> byId; };

static Result run(const std::vector<cf32>& iq, double rate = kRate, double centre = kCentre, size_t block = 16384) {
    AdslReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(centre);
    for (size_t i = 0; i < iq.size(); i += block) rx.feed(iq.data() + i, std::min(block, iq.size() - i));
    const std::vector<cf32> tail((size_t)(rate * 0.3), cf32(0, 0));
    rx.feed(tail.data(), tail.size());
    Result r;
    CHECK(rx.telemetry(r.t, 0), "no telemetry");
    for (const auto& a : r.t.aircraft) r.byId[a.id] = a;
    return r;
}

static int goodCount(Band band, double snr, double cfo, double ppm, int n, unsigned seed, int version = 1, int key = 0, bool fec = false, int pre = -1, double rate = kRate, double centre = kCentre) {
    std::vector<AdslTx> v;
    for (int i = 0; i < n; i++) { auto t = tx(0.05 + i * 0.05, band, adslTrafficNet(ac(0x400000u + (uint32_t)i), version, key, fec), cfo); t.preamble = pre; v.push_back(t); }
    Result r = run(adslScene(rate, centre, 0.1 + n * 0.05, v, snr, ppm, seed), rate, centre);
    return (int)r.byId.size();
}

int main() {
    const Band kBands[4] = {BandM0, BandM1, BandLdr, BandHdr};
    // Fields end to end
    {
        Result r = run(adslScene(kRate, kCentre, 0.3, {tx(0.05, BandM0, adslTrafficNet(ac(0xDD4711)))}, 30));
        CHECK(r.byId.count(0xDD4711), "not decoded");
        const AdslAircraft& a = r.byId[0xDD4711];
        CHECK(a.amt == AmtFlarm && a.category == 4 && a.flightState == 2 && a.hasPos && std::fabs(a.lat - 54.7) < 2e-5 && std::fabs(a.lon - 25.3) < 4e-5, "fields");
        CHECK(a.hasAlt && std::abs(a.altM - 1234) <= 2 && a.hasSpeed && std::fabs(a.speedKmh - 113.4f) < 0.6f && a.hasClimb && std::fabs(a.climbMs + 1.25f) < 0.07f && a.hasTrack && std::fabs(a.trackDeg - 247.5f) < 0.5f, "kinematics");
        CHECK(r.t.perBand[BandM0] == 1 && r.t.traffic == 1 && r.t.blocksOk == 1, "counters");
    }
    // Every band x key index x parity x protocol version, default and shortest preamble
    for (Band b : kBands) for (int key : {0, 3}) for (bool fec : {false, true}) for (int version : {0, 1}) for (int pre : {-1, b == BandLdr ? 20 : b == BandHdr ? 6 : 5}) {
        if (b == BandLdr && fec) continue;                    // not allowed on this channel
        const int got = goodCount(b, 25, 0, 0, 4, 7, version, key, fec, pre);
        CHECK(got == 4, "band %d key %d fec %d version %d preamble %d: %d of 4", b, key, fec, version, pre, got);
    }
    // Carrier offset and transmitter clock: +-20 kHz covers +-20 ppm at 868 MHz on the wide channels; O-LDR is narrow but must take +-10 kHz. Polarity through conj below.
    for (Band b : kBands) {
        const double lim = b == BandLdr ? 10000 : 20000;
        for (double cfo = -lim; cfo <= lim; cfo += lim / 2) for (double ppm : {-40.0, 0.0, 40.0}) {
            const int got = goodCount(b, 25, cfo, ppm, 4, 11);
            CHECK(got == 4, "band %d offset %.0f Hz clock %.0f ppm: %d of 4", b, cfo, ppm, got);
        }
    }
    // Inverted spectrum (an inverting tuner): the polarity is found on its own
    for (Band b : kBands) {
        std::vector<cf32> iq = adslScene(kRate, 2 * kChannelHz[b] - kCentre, 0.3, {tx(0.05, b, adslTrafficNet(ac(0x111111)))}, 25);
        for (auto& v : iq) v = std::conj(v);
        Result r = run(iq);
        CHECK(r.byId.count(0x111111), "inverted spectrum, band %d", b);
    }
    // Noise sweep
    {
        const double kSnrs[] = {25, 18, 14, 12, 10, 8};
        for (Band b : kBands) {
            printf("adsl snr band %d:", b);
            for (double s : kSnrs) { int got = 0; for (unsigned seed = 1; seed <= 3; seed++) got += goodCount(b, s, 3000, 0, 8, seed); printf(" %.0f dB %d/24", s, got); if (s >= 18) CHECK(got >= 24, "band %d at %.0f dB: %d of 24", b, s, got); }
            printf("\n");
        }
    }
    // Overlap: on one channel the stronger packet survives; packets on different channels do not disturb each other
    {
        const auto a = adslTrafficNet(ac(0x777777));
        const auto q = adslTrafficNet(ac(0x888888));
        Result r = run(adslScene(kRate, kCentre, 0.3, {tx(0.05, BandM0, a, 0, 0), tx(0.05 + 0.0015, BandM0, q, 0, -14)}, 25));
        CHECK(r.byId.count(0x777777), "stronger packet of a collision lost");
        r = run(adslScene(kRate, kCentre, 0.3, {tx(0.05, BandM0, a), tx(0.05, BandM1, q), tx(0.05, BandHdr, adslTrafficNet(ac(0x999999))), tx(0.08, BandLdr, adslTrafficNet(ac(0xAAAAAA)))}, 25));
        CHECK(r.byId.size() == 4, "four channels at once: %zu", r.byId.size());
        // back to back on one channel (a Manchester Traffic packet lasts 4.56 ms)
        r = run(adslScene(kRate, kCentre, 0.3, {tx(0.05, BandM0, a), tx(0.0548, BandM0, q)}, 25));
        CHECK(r.byId.size() == 2, "back to back: %zu", r.byId.size());
        // A strong M-band neighbour 200 kHz away does not take the channel
        r = run(adslScene(kRate, kCentre, 0.3, {tx(0.05, BandM0, a, 0, -20), tx(0.05, BandM1, q, 0, 0)}, 45));
        CHECK(r.byId.count(0x777777) && r.byId.count(0x888888), "adjacent channel 20 dB stronger");
    }
    // Garbled length byte
    for (Band b : {BandM0, BandHdr}) {
        auto t = tx(0.05, b, adslTrafficNet(ac(0x123123))); t.garbleLength = true;
        CHECK(run(adslScene(kRate, kCentre, 0.3, {t}, 25)).byId.count(0x123123), "garbled length, band %d", b);
    }
    // One bad bit: repaired, but a repaired packet alone never creates an aircraft
    {
        auto bad = adslTrafficNet(ac(0x654321));
        bad[9] ^= 0x10;
        AdslTx t1 = tx(0.05, BandM0, bad);
        Result r = run(adslScene(kRate, kCentre, 0.3, {t1}, 25));
        CHECK(r.t.corrected == 1 && !r.byId.count(0x654321), "lone repaired packet");
        r = run(adslScene(kRate, kCentre, 0.3, {tx(0.02, BandM0, adslTrafficNet(ac(0x654321))), tx(0.1, BandM0, bad)}, 25));
        CHECK(r.t.corrected == 1 && r.byId.count(0x654321) && r.byId[0x654321].packets == 2, "repair of a known aircraft");
    }
    // Reserved key index: counted, nothing decoded
    {
        Result r = run(adslScene(kRate, kCentre, 0.3, {tx(0.05, BandM0, adslTrafficNet(ac(0x222222), 1, 1, false))}, 25));
        CHECK(r.t.encrypted == 1 && r.byId.empty(), "reserved key");
    }
    // Status for a known aircraft, Traffic Uplink with FEC, relayed flag
    {
        Status s; s.maxVersion = 1; s.mRx = 3;
        uint8_t p[7]; packStatus(s, p);
        auto st = makeNetwork(makeData(TypeStatus, AmtFlarm, 0x333333, false, p, 7), 1, 0, false);
        std::vector<uint8_t> up;
        for (uint32_t id : {0x500001u, 0x500002u, 0x500003u}) {
            Traffic o = ac(id, 1, 50 + (id & 15));
            uint8_t e[19] = {};
            const uint32_t w = (uint32_t)AmtIcao | id << 6;
            e[0] = (uint8_t)w; e[1] = (uint8_t)(w >> 8); e[2] = (uint8_t)(w >> 16); e[3] = (uint8_t)(w >> 24);
            packTraffic(o, e + 4);
            up.insert(up.end(), e, e + 19);
        }
        while ((up.size() + 5) % 4) up.push_back(0);
        auto unet = makeNetwork(makeData(TypeUplink, AmtOgn, 0xF00001, false, up.data(), up.size()), 1, 0, true);
        Result r = run(adslScene(kRate, kCentre, 0.4, {tx(0.02, BandM1, adslTrafficNet(ac(0x333333))), tx(0.08, BandM0, st), tx(0.12, BandHdr, unet)}, 25));
        CHECK(r.byId.count(0x333333) && r.byId[0x333333].hasStatus && r.byId[0x333333].mRx == 3 && r.t.status == 1, "status");
        CHECK(r.byId.count(0x500002) && r.byId[0x500002].viaUplink && r.byId[0x500002].amt == AmtIcao && std::fabs(r.byId[0x500002].lat - 52.0) < 1e-3 && r.t.uplink == 1, "uplink");
    }
    // Sample rates and capture centres
    for (double rate : {2e6, 2.4e6, 2.56e6, 3.2e6}) for (Band b : kBands) CHECK(goodCount(b, 25, 2000, 0, 3, 3, 1, 0, false, -1, rate) == 3, "rate %.0f band %d", rate, b);
    for (double rate : {1e6, 1.024e6, 1.2e6}) {
        for (Band b : {BandM0, BandM1}) CHECK(goodCount(b, 25, 2000, 0, 3, 3, 1, 0, false, -1, rate, 868.3e6) == 3, "narrow rate %.0f band %d", rate, b);
        AdslReceiver rx; rx.configure(rate); rx.setCenterHz(868.3e6);
        AdslTelemetry t; std::vector<cf32> z(4096); rx.feed(z.data(), z.size()); CHECK(rx.telemetry(t, 0) && t.bandOn[0] && t.bandOn[1] && !t.bandOn[2] && !t.bandOn[3], "O-band must be off below the needed rate");
    }
    // A centre tuned on the O-band alone still takes the M-band out cleanly
    for (Band b : {BandLdr, BandHdr}) CHECK(goodCount(b, 25, 0, 0, 3, 5, 1, 0, false, -1, 1e6, 869.525e6) == 3, "O-band centred, band %d", b);
    // Noise alone: no packets, no aircraft
    {
        std::vector<cf32> iq = adslScene(kRate, kCentre, 8.0, {}, 0);          // sigma from a 0 dB "signal" level: a loud noise floor
        Result r = run(iq);
        CHECK(r.t.blocksOk == 0 && r.byId.empty(), "%llu false packets on noise", (unsigned long long)r.t.blocksOk);
        printf("adsl noise: %llu sync words in 8 s, none decoded\n", (unsigned long long)r.t.syncs);
    }
    // FLARM and ADS-L out of one capture: the engine feeds both receivers with the offset of AdslReceiver::flarmOffsetHz()
    {
        const double unix0 = 1.76e9;
        std::vector<cf32> iq = adslScene(kRate, kCentre, 1.0, {tx(0.05, BandM1, adslTrafficNet(ac(0x77ABCD)))}, 25);
        FlarmTx f; f.timeSec = 0.3; f.channel = 0; f.keyTime = (uint32_t)(unix0 + 0.3); f.pkt.addr = 0xDD1234; f.pkt.addrType = flarm::AddrFlarm; f.pkt.aircraftType = 1;
        flarm::packPosition(54.71, 25.31, f.pkt.lat19, f.pkt.lon20); f.pkt.alt = 900;
        FlarmTx f2 = f; f2.timeSec = 0.6; f2.channel = 1; f2.keyTime = (uint32_t)(unix0 + 0.6);
        std::vector<cf32> fl = flarmScene(kRate, 1.0, {f, f2}, -200);
        const double w = 2 * dect2::math::kPi * (868.3e6 - kCentre) / kRate;
        for (size_t i = 0; i < iq.size() && i < fl.size(); i++) iq[i] += fl[i] * cf32((float)std::cos(w * (double)i), (float)std::sin(w * (double)i));
        Result r = run(iq);
        CHECK(r.byId.count(0x77ABCD), "ADS-L beside FLARM");
        AdslReceiver ax; ax.configure(kRate); ax.setCenterHz(kCentre);
        FlarmReceiver flr; flr.configure(kRate); flr.setWallClock(false); flr.setUnixTime(unix0); flr.setSignalOffset(ax.flarmOffsetHz()); flr.setReference(54.7, 25.3);
        for (size_t i = 0; i < iq.size(); i += 16384) flr.feed(iq.data() + i, std::min<size_t>(16384, iq.size() - i));
        std::vector<cf32> tail(400000); flr.feed(tail.data(), tail.size());
        FlarmTelemetry ft; CHECK(flr.telemetry(ft, 0), "flarm telemetry");
        CHECK(ft.aircraftCount == 1 && ft.aircraft.size() == 1 && ft.aircraft[0].id == 0xDD1234 && ft.aircraft[0].hasPos, "FLARM from the ADS-L capture: %u aircraft", ft.aircraftCount);
    }
    // The synthetic scene runs through the receiver as the app will
    {
        SynthConfig c; c.snrDb = 25;
        auto syn = makeAdslSynth(c, kRate);
        CHECK(syn != nullptr, "synth");
        std::vector<cf32> iq((size_t)kRate * 4);
        syn->generate(iq.data(), iq.size());
        Result r = run(iq);
        CHECK(r.t.aircraftCount >= 4 && r.t.perBand[BandM0] > 0 && r.t.perBand[BandM1] > 0 && r.t.perBand[BandLdr] > 0 && r.t.perBand[BandHdr] > 0 && r.t.status > 0 && r.t.uplink > 0, "synth scene: %u aircraft, per band %llu %llu %llu %llu", r.t.aircraftCount, (unsigned long long)r.t.perBand[0], (unsigned long long)r.t.perBand[1], (unsigned long long)r.t.perBand[2], (unsigned long long)r.t.perBand[3]);
    }
    printf(fails ? "adsl rx: %d FAILED\n" : "adsl rx: all passed\n", fails);
    return fails ? 1 : 0;
}
