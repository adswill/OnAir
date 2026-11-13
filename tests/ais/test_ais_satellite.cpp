// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Satellite AIS (ais_sat.h): Doppler sweep over the +-4 kHz of a low earth orbit, Doppler tracking with a changing rate, long-delay bursts that run
// into the next slot, collisions (successive cancellation, frequency candidates), the receiver's satellite mode, and the pass statistics.
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_longrange.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/ais/ais_sat.h"
#include "dect2/ais/ais_testutil.h"
#include "dect2/geo/passes.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static ais::Bits lr(std::mt19937& rng) {
    ais::LongRangeReport r;
    r.mmsi = aistest::randomMmsi(rng());
    r.lat = 30 + (double)(rng() % 600) / 60; r.lon = 120 + (double)(rng() % 600) / 60;
    r.sogKn = (int)(rng() % 30); r.cogDeg = (int)(rng() % 360); r.navStatus = 0;
    return ais::encodeLongRange(r);
}

// one channel at 48 kHz (channel A at 0 Hz), as the receiver hands a burst to the decoder
static std::vector<cf32> at48(const std::vector<AisBurstSpec>& s, double dur, double snr, uint32_t seed = 1) {
    AisRenderConfig c; c.rate = kAisWorkRate; c.durationSec = dur; c.snrDb = snr; c.tuneOffsetHz = -25000; c.seed = seed;
    return renderAisBursts(s, c);
}

static int countFound(const AisCollisionResult& r, const std::vector<ais::Bits>& want) {
    int n = 0;
    for (const auto& w : want) for (const auto& f : r.frames) if (f.payload == w) { n++; break; }
    return n;
}

static void geometry() {
    const double d = aisMaxDopplerHz(162e6, 500);
    CHECK(d > 3600 && d < 4200, "max Doppler at 500 km: %.0f Hz", d);
    CHECK(std::fabs(aisSlantRangeKm(500, 90) - 500) < 1e-6, "slant range overhead");
    CHECK(aisSlantRangeKm(500, 0) > 2500 && aisSlantRangeKm(500, 0) < 2700, "slant range at the horizon %.0f", aisSlantRangeKm(500, 0));
    const double sw = aisSwathKm(500, 20);
    CHECK(sw > 1800 && sw < 2600, "swath above 20 deg at 500 km: %.0f km", sw);
    CHECK(aisDelayMs(2574) > 8.5 && aisDelayMs(2574) < 8.7, "delay");
}

// a single burst anywhere in the Doppler range decodes with the satellite filters
static void dopplerSweep() {
    std::mt19937 rng(9);
    AisPhyConfig cfg;
    int okSat = 0, okGround = 0, n = 0;
    for (double f = -4500; f <= 4500; f += 500) {
        const ais::Bits b = lr(rng);
        AisBurstSpec s; s.payload = b; s.startSec = 0.004; s.cfoHz = f;
        const std::vector<cf32> x = at48({s}, 0.03, 18, (uint32_t)(f + 10000));
        const AisCollisionResult r = aisDecodeCollided(x.data(), x.size(), 200, 1200, cfg, {});
        okSat += countFound(r, {b});
        const AisBurstResult g = aisDecodeBurst(x.data(), x.size(), 200, 1200, cfg);
        okGround += !g.frames.empty();
        if (!r.frames.empty()) CHECK(std::fabs(r.frames[0].cfoHz - f) < 250, "frequency of the burst at %.0f: %.0f", f, r.frames[0].cfoHz);
        n++;
    }
    CHECK(okSat == n, "Doppler sweep: %d / %d decoded (the plain decoder %d)", okSat, n, okGround);

    // through the receiver: the satellite mode's wide channel filter at -4.4 kHz (the coast filter cuts the burst's edge)
    std::vector<AisBurstSpec> sp;
    std::vector<ais::Bits> want;
    for (int i = 0; i < 12; i++) {
        AisBurstSpec s; s.payload = lr(rng); s.channel = i & 1 ? 'B' : 'A'; s.startSec = 0.05 + 0.035 * i; s.cfoHz = (i % 2 ? 1 : -1) * (3000 + 120 * i);
        sp.push_back(s); want.push_back(s.payload);
    }
    AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 0.6; rc.snrDb = 20;
    const std::vector<cf32> x = renderAisBursts(sp, rc);
    AisReceiver rx; rx.configure(rc.rate); rx.setSatelliteMode(true);
    const AisTelemetry t = aistest::runReceiver(rx, x);
    CHECK(t.satellite, "satellite mode reported");
    CHECK(t.blocksOk == want.size(), "receiver, Doppler 3 - 4.4 kHz: %llu / %zu", (unsigned long long)t.blocksOk, want.size());
}

// the tracker follows a Doppler curve that changes at a steady rate (the middle of a pass)
static void tracking() {
    std::mt19937 rng(3);
    std::vector<AisBurstSpec> sp;
    const double rate = -45;   // Hz per second
    for (int i = 0; i < 40; i++) {
        AisBurstSpec s; s.payload = lr(rng); s.channel = 'A'; s.startSec = 0.1 + 0.25 * i; s.cfoHz = 2500 + rate * s.startSec;
        sp.push_back(s);
    }
    AisRenderConfig rc; rc.rate = 250e3; rc.durationSec = 10.3; rc.snrDb = 22;
    const std::vector<cf32> x = renderAisBursts(sp, rc);
    AisReceiver rx; rx.configure(rc.rate); rx.setSatelliteMode(true);
    const AisTelemetry t = aistest::runReceiver(rx, x);
    const double expect = 2500 + rate * t.timeSec;
    CHECK(t.blocksOk >= 38, "tracking: %llu / 40 decoded", (unsigned long long)t.blocksOk);
    CHECK(std::fabs(t.dopplerRateHzS[0] - rate) < 8, "Doppler rate %.1f Hz/s (sent %.1f)", t.dopplerRateHzS[0], rate);
    CHECK(std::fabs(t.dopplerHz[0] - expect) < 150, "Doppler now %.0f Hz (sent %.0f)", t.dopplerHz[0], expect);

    AisDopplerTracker d;
    for (int i = 0; i < 20; i++) d.add(i, 1000 + 30.0 * i);
    CHECK(d.valid() && std::fabs(d.rateHzS() - 30) < 0.01 && std::fabs(d.estimate(25) - 1750) < 0.5, "tracker fit");
}

// long delays: a burst of the next slot starts before the late one of this slot has ended
static void longDelay() {
    std::mt19937 rng(11);
    AisPhyConfig cfg;
    int got = 0, total = 0, plain = 0;
    for (int k = 0; k < 10; k++) {
        const double slot = 0.026667;
        const ais::Bits a = aistest::randomPosition(rng), b = aistest::randomPosition(rng);
        AisBurstSpec s1; s1.payload = a; s1.startSec = 0.004 + 0.0085; s1.cfoHz = 1500 + 100 * k; s1.level = 1.0;       // 8.5 ms late (2500 km)
        AisBurstSpec s2; s2.payload = b; s2.startSec = 0.004 + slot + 0.0012; s2.cfoHz = -1800 + 50 * k; s2.level = 0.5;   // 1.2 ms late, 6 dB weaker
        const std::vector<cf32> x = at48({s1, s2}, 0.075, 22, 100 + k);
        // the power detector sees one long burst from the first start to the second's end
        const size_t c0 = (size_t)(s1.startSec * kAisWorkRate), c1 = (size_t)((s2.startSec + 0.0267) * kAisWorkRate);
        const AisCollisionResult r = aisDecodeCollided(x.data(), x.size(), c0, c1, cfg, {});
        got += countFound(r, {a, b});
        plain += (int)aisDecodeBurst(x.data(), x.size(), c0, c1, cfg).frames.size();
        total += 2;
    }
    CHECK(got >= total - 1, "overlapping slots: %d / %d decoded (plain decoder %d)", got, total, plain);
}

// collisions: several bursts in one slot at different powers and Doppler shifts
static void collisions() {
    std::mt19937 rng(21);
    AisPhyConfig cfg;
    int got2 = 0, n2 = 0, got3 = 0, n3 = 0, plain2 = 0, cancelled = 0;
    for (int k = 0; k < 12; k++) {
        const ais::Bits a = aistest::randomPosition(rng), b = aistest::randomPosition(rng);
        AisBurstSpec s1; s1.payload = a; s1.startSec = 0.005; s1.cfoHz = -2200 + 40 * k; s1.phase = 0.3 * k;
        AisBurstSpec s2; s2.payload = b; s2.startSec = 0.005 + 0.0004 * (k % 4); s2.cfoHz = 1400 - 30 * k; s2.level = std::pow(10.0, -8.0 / 20); s2.phase = 1.1 * k;
        const std::vector<cf32> x = at48({s1, s2}, 0.04, 25, 300 + k);
        const AisCollisionResult r = aisDecodeCollided(x.data(), x.size(), 240, 1520, cfg, {});
        got2 += countFound(r, {a, b}); n2 += 2;
        for (const auto& f : r.frames) cancelled += f.layer >= 2;
        plain2 += (int)aisDecodeBurst(x.data(), x.size(), 240, 1520, cfg).frames.size();
    }
    CHECK(got2 >= n2 - 2, "two in one slot (8 dB apart): %d / %d (plain decoder %d, %d after cancellation)", got2, n2, plain2, cancelled);
    CHECK(cancelled >= 6, "successive cancellation found the weaker burst %d times", cancelled);
    for (int k = 0; k < 8; k++) {
        const ais::Bits a = lr(rng), b = lr(rng), c = lr(rng);
        AisBurstSpec s1; s1.payload = a; s1.startSec = 0.005; s1.cfoHz = 3000;
        AisBurstSpec s2; s2.payload = b; s2.startSec = 0.0053; s2.cfoHz = -500 + 60 * k; s2.level = 0.4;
        AisBurstSpec s3; s3.payload = c; s3.startSec = 0.0058; s3.cfoHz = -3500; s3.level = 0.16;
        const std::vector<cf32> x = at48({s1, s2, s3}, 0.035, 30, 500 + k);
        const AisCollisionResult r = aisDecodeCollided(x.data(), x.size(), 240, 1150, cfg, {});
        got3 += countFound(r, {a, b, c}); n3 += 3;
    }
    CHECK(got3 >= n3 * 2 / 3, "three in one slot (0 / -8 / -16 dB): %d / %d", got3, n3);
    // equal powers, frequency candidates from the tracker: a hint at the right shift
    int gotH = 0;
    for (int k = 0; k < 6; k++) {
        const ais::Bits a = lr(rng), b = lr(rng);
        AisBurstSpec s1; s1.payload = a; s1.startSec = 0.005; s1.cfoHz = 2600;
        AisBurstSpec s2; s2.payload = b; s2.startSec = 0.005; s2.cfoHz = -2600; s2.level = 0.9;
        const std::vector<cf32> x = at48({s1, s2}, 0.035, 25, 700 + k);
        const AisCollisionResult r = aisDecodeCollided(x.data(), x.size(), 240, 1150, cfg, {2600, -2600});
        gotH += countFound(r, {a, b});
    }
    CHECK(gotH >= 10, "equal powers 5.2 kHz apart with hints: %d / 12", gotH);
    // cancellation leaves little of a burst behind
    const ais::Bits a = aistest::randomPosition(rng);
    AisBurstSpec s; s.payload = a; s.startSec = 0.003; s.cfoHz = 777; s.phase = 0.7;
    std::vector<cf32> x = at48({s}, 0.035, -200);
    double e0 = 0, e1 = 0;
    for (const cf32& v : x) e0 += std::norm(v);
    double st = 0, f = 0;
    CHECK(aisCancelBurst(x, a, 700, 0, x.size(), nullptr, nullptr, &st, &f), "fit");
    for (const cf32& v : x) e1 += std::norm(v);
    CHECK(e1 < 0.003 * e0, "cancellation removes the burst: %.1f dB left", 10 * std::log10(e1 / e0));
    CHECK(std::fabs(st - 0.003 * kAisWorkRate) < 0.3 && std::fabs(f - 777) < 5, "fit: start %.2f (%.2f), %.1f Hz", st, 0.003 * kAisWorkRate, f);
}

// the pass statistics on made-up receptions
static void stats() {
    AisSatStats st;
    std::vector<AisRxRecord> rr;
    uint64_t n = 0;
    auto rec = [&](double utc, uint32_t mmsi, int type, double lat, double lon, float snr) {
        AisRxRecord r; r.n = ++n; r.utc = utc; r.mmsi = mmsi; r.type = type; r.hasPos = true; r.lat = (float)lat; r.lon = (float)lon; r.snrDb = snr;
        r.channel = type == 27 ? 'C' : 'A';
        rr.push_back(r);
    };
    const double t0 = 1222000000;   // 2008-09-21, near the epoch of the TLE below
    // pass 1: three ships, one new-type that also sends type 1
    rec(t0, 412000001, 1, 31, 121, 10); rec(t0 + 20, 412000001, 27, 31, 121, 11); rec(t0 + 40, 412000002, 1, 32, 122, 8); rec(t0 + 60, 412000003, 5, 33, 123, 6);
    // pass 2, 90 minutes later: two ships, one new-type that sends only 27
    rec(t0 + 5400, 412000004, 27, 30, 120, 9); rec(t0 + 5420, 412000002, 18, 32, 122, 7);
    st.addAll(rr);
    st.addAll(rr);   // the same list again changes nothing
    CHECK(st.receptions() == 6, "receptions %llu", (unsigned long long)st.receptions());
    CHECK(st.passes().size() == 2, "passes %zu", st.passes().size());
    if (st.passes().size() == 2) {
        CHECK(st.passes()[0].detections == 4 && st.passes()[0].vessels.size() == 3 && st.passes()[0].newTypeVessels == 1, "pass 1");
        CHECK(st.passes()[1].detections == 2 && st.passes()[1].vessels.size() == 2 && st.passes()[1].longRange == 1, "pass 2");
    }
    CHECK(st.vessels().size() == 4 && st.newTypeCount() == 2, "new-type vessels %u of %zu", st.newTypeCount(), st.vessels().size());
    CHECK(std::fabs(st.newTypeRatio() - 0.5) < 1e-9, "new-type ratio %.2f", st.newTypeRatio());
    CHECK(std::fabs(st.newTypeStillCommonShare() - 0.5) < 1e-9, "new-type still sending the common reports %.2f", st.newTypeStillCommonShare());
    CHECK(st.typeMix()[27] == 2 && st.typeMix()[1] == 2 && st.typeMix()[18] == 1 && st.typeMix()[5] == 1, "type mix");
    const auto m = st.months();
    CHECK(m.size() == 1 && m[0].passes == 2 && m[0].vessels == 4 && m[0].newType == 2, "one month");
    if (!m.empty()) CHECK(std::fabs(m[0].detectionProb - 5.0 / 8.0) < 1e-9 && !m[0].fromFootprint, "detection probability without a TLE: %.3f", m[0].detectionProb);
    const std::string csv = st.receptionsCsv();
    CHECK(csv.rfind("utc,unix_time,", 0) == 0 && std::count(csv.begin(), csv.end(), '\n') == 7, "receptions CSV");
    CHECK(st.passesCsv().find("new_type_vessels") != std::string::npos && st.vesselsCsv().find("412000004") != std::string::npos && st.monthsCsv().find("2008,9,2,4,2") != std::string::npos,
          "the other CSV files");
    // with the satellite's elements: elevations and the footprint
    const auto tl = parseTleText("ISS\n1 25544U 98067A   08264.51782528 -.00002182  00000-0 -11606-4 0  2927\n2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.72125391563537\n");
    CHECK(tl.size() == 1, "TLE");
    if (tl.size() == 1) {
        AisSatStats s2;
        s2.setSatellite(tl[0]);
        CHECK(s2.hasSatellite(), "satellite set");
        double lat = 0, lon = 0, alt = 0;
        subPoint([&] { Sgp4 g; g.init(tl[0]); return g; }(), t0, lat, lon, alt);
        CHECK(s2.elevationOf(t0, lat, lon) > 85, "the ship under the satellite sees it overhead: %.1f", s2.elevationOf(t0, lat, lon));
        std::vector<AisRxRecord> r2;
        for (int i = 0; i < 30; i++) {
            AisRxRecord r; r.n = (uint64_t)i + 1; r.utc = t0 + i; r.mmsi = 412100000 + (uint32_t)i; r.type = 27; r.hasPos = true;
            r.lat = (float)(lat + (i - 15) * 0.4); r.lon = (float)lon; r.snrDb = (float)(5 + i % 10);
            r2.push_back(r);
        }
        s2.addAll(r2);
        uint32_t inBins = 0;
        for (const AisSnrBin& b : s2.snrVsElevation()) inBins += b.n;
        CHECK(inBins == 30, "every reception has an elevation: %u", inBins);
        const auto m2 = s2.months();
        CHECK(m2.size() == 1 && m2[0].fromFootprint && m2[0].detectionProb > 0.99, "footprint detection probability %.2f", m2.empty() ? -1.0 : m2[0].detectionProb);
    }
}

// the receiver in satellite mode: a busy slot plan on two channels with collisions and a spread of Doppler shifts
static void receiverBusy() {
    std::mt19937 rng(5);
    std::vector<AisBurstSpec> sp;
    std::set<uint32_t> sent;
    for (int i = 0; i < 60; i++) {
        AisBurstSpec s; s.payload = i % 3 ? aistest::randomPosition(rng) : lr(rng);
        s.channel = i & 1 ? 'B' : 'A';
        const int slot = (int)(rng() % 70);
        s.startSec = 0.05 + slot * 0.026667 + (double)(rng() % 80) * 1e-4;   // up to 8 ms of delay
        s.cfoHz = (double)(rng() % 8000) - 4000;
        s.level = std::pow(10.0, -(double)(rng() % 12) / 20);
        sp.push_back(s); sent.insert(ais::getU(s.payload, 8, 30));
    }
    AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 2.1; rc.snrDb = 24;
    const std::vector<cf32> x = renderAisBursts(sp, rc);
    AisReceiver g; g.configure(rc.rate);
    const AisTelemetry tg = aistest::runReceiver(g, x);
    AisReceiver s; s.configure(rc.rate); s.setSatelliteMode(true);
    const AisTelemetry ts = aistest::runReceiver(s, x);
    printf("busy slots: coast mode %llu, satellite mode %llu (%llu recovered, %llu after cancellation) of %zu\n", (unsigned long long)tg.blocksOk,
           (unsigned long long)ts.blocksOk, (unsigned long long)ts.recovered, (unsigned long long)ts.cancelled, sp.size());
    CHECK(ts.blocksOk >= tg.blocksOk + 3, "the satellite mode decodes more: %llu vs %llu", (unsigned long long)ts.blocksOk, (unsigned long long)tg.blocksOk);
    if (getenv("AISDBG2")) {
        std::set<uint32_t> got;
        for (const AisVessel& v : ts.vessels) got.insert(v.mmsi);
        for (size_t i = 0; i < sp.size(); i++) {
            const uint32_t m = ais::getU(sp[i].payload, 8, 30);
            if (got.count(m)) continue;
            const double len = (ais::burstLineBits(sp[i].payload).size()) / 9600.0;
            int ov = 0; double worst = 0;
            for (size_t j = 0; j < sp.size(); j++) if (j != i && sp[j].channel == sp[i].channel) {
                const double lj = ais::burstLineBits(sp[j].payload).size() / 9600.0;
                const double o = std::min(sp[i].startSec + len, sp[j].startSec + lj) - std::max(sp[i].startSec, sp[j].startSec);
                if (o > 0) { ov++; worst = std::max(worst, 20 * std::log10(sp[j].level / sp[i].level)); }
            }
            printf("missed %c t %.4f lvl %.1f dB cfo %.0f overlaps %d (strongest other %+.1f dB)\n", sp[i].channel, sp[i].startSec, 20 * std::log10(sp[i].level), sp[i].cfoHz, ov, worst);
        }
    }
    int bogus = 0;
    for (const AisVessel& v : ts.vessels) bogus += !sent.count(v.mmsi);
    CHECK(bogus == 0, "no made-up stations: %d", bogus);
}

int main() {
    geometry();
    dopplerSweep();
    tracking();
    longDelay();
    collisions();
    stats();
    receiverBusy();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
