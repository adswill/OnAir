// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM receiver: clean frames at two radio rates, privacy flags honoured, the newer protocol counted, bad packets rejected, noise alone gives nothing.
#include "dect2/flarm/flarm_gen.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/flarm/flarm_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kU0 = 1800000000.0 + 20.0;      // key time of the first sample (20 s into a 64 s step... away from the boundary)
constexpr double kRefLat = 54.70, kRefLon = 25.30;

static FlarmTx mk(double t, int ch, uint32_t id, double dn, double de, int acType = 1) {
    FlarmTx tx;
    tx.timeSec = t; tx.channel = ch;
    tx.keyTime = (uint32_t)(kU0 + t);
    flarm::V6& p = tx.pkt;
    p.addr = id; p.aircraftType = acType; p.alt = 800;
    flarm::packPosition(kRefLat + dn / 111320.0, kRefLon + de / (111320.0 * std::cos(kRefLat * 3.14159265 / 180)), p.lat19, p.lon20);
    flarm::packVelocity(25.0, 90.0, 1.0, p);
    return tx;
}

// every packet is sent twice, 2.4 s apart: an aircraft counts when a second packet agrees with the first
static std::vector<FlarmTx> twice(const std::vector<FlarmTx>& v) {
    std::vector<FlarmTx> o = v;
    for (FlarmTx t : v) { t.timeSec += 2.4; t.keyTime = (uint32_t)(kU0 + t.timeSec) + (t.keyTime - (uint32_t)(kU0 + t.timeSec - 2.4)); o.push_back(t); }
    return o;
}

static FlarmTelemetry run(const std::vector<cf32>& x, double rate, FlarmReceiver& r) {
    r.configure(rate);
    r.setUnixTime(kU0);
    r.setReference(kRefLat, kRefLon);
    FlarmTelemetry last;
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 16384) {
        r.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
        FlarmTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

static const FlarmAircraft* find(const FlarmTelemetry& t, uint32_t id) {
    for (const auto& a : t.aircraft) if (a.id == id) return &a;
    return nullptr;
}

int main() {
    CHECK(flarmTuning().stdMode == 68 && modeTuningById("flarm") && modeTuningById("flarm")->stdMode == 68, "tuning table");
    for (double rate : {1e6, 2.4e6}) {
        std::vector<FlarmTx> txs;
        txs.push_back(mk(0.10, 0, 0xDD1234, 1500, -2000));
        txs.push_back(mk(0.30, 1, 0xDDA5B6, -4000, 3000, 2));
        txs.push_back(mk(0.50, 0, 0x3F7C11, 800, 800, 8));
        txs.back().pkt.addrType = flarm::AddrIcao;
        FlarmTx nt = mk(0.70, 1, 0xDD9999, 100, 100); nt.pkt.noTrack = true; txs.push_back(nt);       // no-track
        FlarmTx st = mk(0.90, 0, 0xDD8888, 200, 100); st.pkt.stealth = true; txs.push_back(st);        // stealth
        FlarmTx an = mk(1.10, 1, 0xDD6666, 300, 100); an.pkt.addrType = flarm::AddrAnonymous; txs.push_back(an);   // anonymous address
        FlarmTx v7; v7.timeSec = 1.30; v7.channel = 0; v7.raw.assign(24, 0x5A); v7.raw[0] = 0x77; v7.raw[1] = 0x77; v7.raw[2] = 0xDD; v7.raw[3] = 0x02; txs.push_back(v7);   // the newer protocol
        FlarmTx bc = mk(1.50, 0, 0xDD5555, 10, 10); bc.badCrc = true; txs.push_back(bc);              // bad CRC
        FlarmTx wk = mk(1.70, 1, 0xDD4444, 10, 10); wk.keyTime += 64 * 40; txs.push_back(wk);          // encrypted for another time: parity / garbage
        const std::vector<cf32> x = flarmScene(rate, 4.8, twice(txs), 25.0, 0, 1);
        FlarmReceiver r;
        const FlarmTelemetry t = run(x, rate, r);
        printf("  ch %llu/%llu syncs %llu\n", (unsigned long long)t.perChannel[0], (unsigned long long)t.perChannel[1], (unsigned long long)t.syncs);
        printf("  %.1f Msps: %s, ok %llu bad %llu priv %llu new %llu\n", rate / 1e6, flarmSummary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
               (unsigned long long)t.privacyFlagged, (unsigned long long)t.newProtocol);
        CHECK(t.aircraftCount == 3 && t.blocksOk == 6, "%.1f Msps: %u aircraft, %llu ok", rate / 1e6, t.aircraftCount, (unsigned long long)t.blocksOk);
        const FlarmAircraft* a = find(t, 0xDD1234);
        CHECK(a && a->hasPos && std::fabs(a->lat - (kRefLat + 1500 / 111320.0)) < 3e-4 && a->altM == 800 && a->acType == 1, "glider fields");
        CHECK(a && std::fabs(a->speedKmh - 90.0) < 4.0 && std::fabs(a->trackDeg - 90.0) < 6.0 && std::fabs(a->climbMs - 1.0) < 0.3, "velocity %.1f %.0f %.1f", a ? a->speedKmh : 0, a ? a->trackDeg : 0, a ? a->climbMs : 0);
        CHECK(a && a->hasRange && std::fabs(a->distKm - 2.5) < 0.2, "distance %.2f", a ? a->distKm : -1);
        CHECK(find(t, 0xDDA5B6) && find(t, 0xDDA5B6)->acType == 2 && find(t, 0x3F7C11) && find(t, 0x3F7C11)->addrType == flarm::AddrIcao, "other aircraft");
        // privacy: none of the flagged ones appears anywhere, only the count does
        CHECK(!find(t, 0xDD9999) && !find(t, 0xDD8888) && !find(t, 0xDD6666), "a privacy-flagged aircraft is listed");
        CHECK(t.privacyFlagged == 3, "privacy count %llu", (unsigned long long)t.privacyFlagged);
        CHECK(t.newProtocol == 2 && t.newProtocolDevices == 1, "new protocol %llu", (unsigned long long)t.newProtocol);
        const std::string saved = r.saveResults();
        for (uint32_t id : {0xDD9999u, 0xDD8888u, 0xDD6666u}) {
            const uint8_t b[3] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16)};
            CHECK(saved.find(std::string((const char*)b, 3)) == std::string::npos, "flagged id %06X in the saved results", id);
        }
        CHECK(!find(t, 0xDD5555) && !find(t, 0xDD4444), "bad packet accepted");
        CHECK(t.blocksBad >= 1, "bad packets not counted");
        FlarmReceiver r2;
        CHECK(r2.loadResults(saved), "load");
    }
    // noise alone: nothing
    {
        const std::vector<cf32> x = flarmScene(2e6, 6.0, {}, 0.0, 0, 7);
        FlarmReceiver r;
        FlarmTelemetry t = run(x, 2e6, r);
        CHECK(t.blocksOk == 0 && t.aircraftCount == 0 && t.newProtocol == 0 && t.privacyFlagged == 0, "noise gave %llu packets", (unsigned long long)t.blocksOk);
    }
    // no reference position: packets decode, positions do not appear
    {
        std::vector<FlarmTx> txs{mk(0.1, 0, 0xDD1234, 1000, 1000)};
        const std::vector<cf32> x = flarmScene(1e6, 3.0, twice(txs), 30.0, 0, 2);
        FlarmReceiver r;
        r.configure(1e6); r.setUnixTime(kU0);
        FlarmTelemetry t; uint64_t seq = 0;
        for (size_t i = 0; i < x.size(); i += 8192) { r.feed(x.data() + i, std::min<size_t>(8192, x.size() - i)); FlarmTelemetry q; if (r.telemetry(q, seq)) { seq = q.seq; t = q; } }
        CHECK(t.aircraftCount == 1 && !t.aircraft[0].hasPos, "without a reference position there is no position");
    }
    printf(fails ? "flarm rx: %d FAILED\n" : "flarm rx: all passed\n", fails);
    return fails ? 1 : 0;
}
