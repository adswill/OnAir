// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Orbcomm receiver against the test transmitter: format helpers, one and two downlinks, Doppler (fixed and a +/-4 kHz ramp), noise
// near the threshold, symbol clock offset, packets with a broken checksum, ephemeris positions.
#include "dect2/orbcomm/orbcomm_gen.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace dect2;
using namespace dect2::orbcomm;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Result { OrbcommTelemetry t; uint64_t sent[2], broken[2]; double dop[2]; };

static Result run(SignalConfig c, double secs, double centerHz = 137.275e6, int maxCh = 2) {
    Signal s(c);
    OrbcommReceiver rx;
    rx.configure(c.sampleRate);
    rx.setCenterHz(centerHz);
    rx.setMaxChannels(maxCh);
    std::vector<cf32> buf(8192);
    const size_t total = (size_t)(secs * c.sampleRate);
    for (size_t done = 0; done < total; done += buf.size()) {
        s.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        static const bool dbg = getenv("ORB_DEBUG") != nullptr;
        OrbcommTelemetry t;
        if (dbg && rx.telemetry(t, 0) && (done / buf.size()) % 30 == 0) {
            printf("    t %.2f", t.timeSec);
            for (const auto& ch : t.ch) printf("  [st %d off %.0f dop %.0f sat %d ok %llu bad %llu]", ch.state, ch.offsetHz, ch.dopplerHz, ch.sat, (unsigned long long)ch.pktOk, (unsigned long long)ch.pktBad);
            printf("  sent dop %.0f %.0f\n", s.dopplerAt(0, t.timeSec), s.dopplerAt(1, t.timeSec));
        }
    }
    Result r;
    rx.telemetry(r.t, 0);
    for (int i = 0; i < 2; i++) { r.sent[i] = s.packetsSent(i); r.broken[i] = s.packetsBroken(i); r.dop[i] = s.dopplerAt(i, total / c.sampleRate); }
    return r;
}

static const OrbcommChannel* chanOfSat(const OrbcommTelemetry& t, int sat) {
    for (const auto& c : t.ch) if (c.active && c.sat == sat) return &c;
    return nullptr;
}
static const OrbcommSat* satOf(const OrbcommTelemetry& t, int id) {
    for (const auto& s : t.sats) if (s.id == id) return &s;
    return nullptr;
}

static void formatTests() {
    uint8_t p[12] = {0x1F};
    packEphemeris(p, -4123.5, 5021.25, 2999.0, 1234);
    fletcherSet(p);
    CHECK(fletcherOk(p), "fletcher set / check");
    p[5] ^= 4;
    CHECK(!fletcherOk(p), "fletcher sees a flipped bit");
    p[5] ^= 4;
    double x, y, z; int s;
    unpackEphemeris(p, x, y, z, s);
    CHECK(std::fabs(x + 4123.5) < 0.02 && std::fabs(y - 5021.25) < 0.02 && std::fabs(z - 2999) < 0.02 && s == 1234, "ephemeris round trip %.2f %.2f %.2f %d", x, y, z, s);
    double la, lo, h;
    ecefToGeo(6378.137 + 775, 0, 0, la, lo, h);
    CHECK(std::fabs(la) < 1e-6 && std::fabs(lo) < 1e-6 && std::fabs(h - 775) < 1e-3, "geodetic on the equator");
    CHECK(std::fabs(rasterHz(137.2881e6) - 137.2875e6) < 1, "raster");
    CHECK(typeOf(0x65) == TSync && typeOf(0x1F) == TEphemeris && typeOf(0x22) == TElements && typeOf(0x99) == TOther, "types");
}

static SignalConfig base(double fs = 250e3) {
    SignalConfig c;
    c.sampleRate = fs;
    c.channels = 1;
    c.ch[0].offsetHz = -25e3; c.ch[0].satId = 23; c.ch[0].dopplerHz = 1200; c.ch[0].raanDeg = 20; c.ch[0].phaseDeg = 40;
    c.ch[1].offsetHz = 25e3; c.ch[1].satId = 41; c.ch[1].dopplerHz = -2100; c.ch[1].raanDeg = 80; c.ch[1].phaseDeg = 200;
    c.snrDb = 18;
    return c;
}

static void report(const char* name, const Result& r) {
    printf("  %s: %s\n", name, orbcommSummary(r.t).c_str());
}

int main() {
    formatTests();

    {   // clean, one downlink: every packet after the lock, satellite, Doppler, position, the packet types
        SignalConfig c = base();
        Result r = run(c, 12);
        report("clean", r);
        const OrbcommChannel* ch = chanOfSat(r.t, 23);
        CHECK(ch && ch->state == 3, "clean: frame lock on satellite 23");
        CHECK(r.t.blocksOk + 200 >= r.sent[0] && r.t.blocksBad == 0, "clean: %llu of %llu packets, %llu bad", (unsigned long long)r.t.blocksOk, (unsigned long long)r.sent[0], (unsigned long long)r.t.blocksBad);
        if (ch) {
            CHECK(std::fabs(ch->dopplerHz - 1200) < 15, "clean: Doppler %.1f", ch->dopplerHz);
            CHECK(std::fabs(ch->freqHz - (137.25e6 + 1200)) < 15, "clean: carrier %.1f", ch->freqHz);
        }
        const OrbcommSat* s = satOf(r.t, 23);
        CHECK(s && s->hasPos && s->hasElements, "clean: ephemeris and elements");
        if (s) {
            double x, y, z, al, la, lo, h;
            genOrbit(c.ch[0], c.startSec + s->posTime, x, y, z, al);
            ecefToGeo(x, y, z, la, lo, h);
            CHECK(std::fabs(la - s->lat) < 0.2 && std::fabs(std::remainder(lo - s->lon, 360.0)) < 0.2 && std::fabs(h - s->altKm) < 2, "clean: position %.2f %.2f %.0f (sent %.2f %.2f %.0f)", s->lat, s->lon, s->altKm, la, lo, h);
            CHECK(std::fabs(s->inclDeg - 45) < 0.01 && s->periodMin > 99 && s->periodMin < 102, "clean: elements %.2f %.2f", s->inclDeg, s->periodMin);
            CHECK(!s->downlinksMhz.empty() && !s->uplinksMhz.empty(), "clean: channel assignments");
        }
        for (int k : {TMessage, TUplink, TDownlink, TEphemeris, TElements, TNetwork, TFill, TSync}) CHECK(r.t.typeCount[k] > 0, "clean: type %s seen", typeLabel(k));
        CHECK(r.t.messages > 5, "clean: %llu messages", (unsigned long long)r.t.messages);
        bool full = false;
        for (const auto& p : r.t.log) full |= p.text.find("complete: TEST") != std::string::npos && p.text.find("FROM SAT 23 HELLO") != std::string::npos;
        CHECK(full, "clean: a whole message in the log");
        CHECK(ch && !ch->constel.empty() && ch->eyeLen > 0 && !ch->eye.empty(), "clean: constellation and eye");
    }
    {   // two downlinks, a Doppler ramp of +/-4 kHz (triangle, 40 s legs: 200 Hz/s, some three times a real overhead pass), 1 MHz input
        SignalConfig c = base(1e6);
        c.channels = 2;
        c.ch[0].dopplerHz = -4000; c.ch[0].dopplerEndHz = 4000; c.ch[0].rampSec = 40;
        c.ch[1].dopplerHz = 3000; c.ch[1].dopplerEndHz = -3000; c.ch[1].rampSec = 40;
        Result r = run(c, 42);
        report("ramp, two", r);
        const OrbcommChannel *a = chanOfSat(r.t, 23), *b = chanOfSat(r.t, 41);
        CHECK(a && b && a->state == 3 && b->state == 3, "ramp: both downlinks locked");
        const OrbcommSat *sa = satOf(r.t, 23), *sb = satOf(r.t, 41);
        CHECK(sa && sb && sa->packets + 200 > r.sent[0] && sb->packets + 200 > r.sent[1], "ramp: packets %llu/%llu %llu/%llu", sa ? (unsigned long long)sa->packets : 0ull, (unsigned long long)r.sent[0], sb ? (unsigned long long)sb->packets : 0ull, (unsigned long long)r.sent[1]);
        if (a) CHECK(std::fabs(a->dopplerHz - r.dop[0]) < 40, "ramp: Doppler A %.0f (sent %.0f)", a->dopplerHz, r.dop[0]);
        if (b) CHECK(std::fabs(b->dopplerHz - r.dop[1]) < 40, "ramp: Doppler B %.0f (sent %.0f)", b->dopplerHz, r.dop[1]);
        CHECK(r.t.blocksBad < 20, "ramp: %llu bad", (unsigned long long)r.t.blocksBad);
    }
    {   // the second downlink 37.5 kHz away, one decoder only: the stronger one
        SignalConfig c = base();
        c.channels = 2;
        c.ch[1].offsetHz = -25e3 + 37.5e3;
        Result r = run(c, 8, 137.275e6, 1);
        report("one of two", r);
        int active = 0; for (const auto& ch : r.t.ch) active += ch.active;
        CHECK(active == 1 && r.t.blocksOk > 250, "one decoder: %d active, %llu packets", active, (unsigned long long)r.t.blocksOk);
    }
    {   // noise near the threshold: Es/N0 10 dB
        SignalConfig c = base();
        c.snrDb = 10;
        Result r = run(c, 20);
        report("Es/N0 10 dB", r);
        const double share = (double)r.t.blocksOk / std::max<uint64_t>(1, r.sent[0]);
        CHECK(share > 0.7, "noise: %.2f of the packets", share);
    }
    {   // clock offset 150 ppm and an extra carrier offset
        SignalConfig c = base();
        c.clockPpm = 150;
        c.ch[0].dopplerHz = -3700;
        Result r = run(c, 12);
        report("clock +150 ppm", r);
        const OrbcommChannel* ch = chanOfSat(r.t, 23);
        CHECK(ch && ch->state == 3 && r.t.blocksOk + 200 >= r.sent[0] && r.t.blocksBad == 0, "clock: %llu of %llu", (unsigned long long)r.t.blocksOk, (unsigned long long)r.sent[0]);
        if (ch) printf("  clock measured %.0f ppm\n", ch->clockPpm);
        if (ch) CHECK(std::fabs(ch->clockPpm - 150) < 60, "clock: %.0f ppm", ch->clockPpm);
    }
    {   // 10 % of the packets with a broken checksum: counted bad, never decoded
        SignalConfig c = base();
        c.badFraction = 0.10;
        Result r = run(c, 15);
        report("checksum errors", r);
        const double seenShare = (double)(r.t.blocksOk + r.t.blocksBad) / (r.sent[0] - 150);
        const double badShare = (double)r.t.blocksBad / std::max<uint64_t>(1, r.t.blocksOk + r.t.blocksBad);
        CHECK(seenShare > 0.85 && std::fabs(badShare - 0.10) < 0.03, "checksum: %.2f seen, %.3f bad", seenShare, badShare);
        const OrbcommChannel* ch = chanOfSat(r.t, 23);
        CHECK(ch && ch->state == 3, "checksum: lock kept");
        bool wrong = false;
        for (const auto& p : r.t.log) wrong |= !fletcherOk(p.bytes);
        CHECK(!wrong, "checksum: only good packets in the log");
    }
    {   // nothing but noise: no downlink claimed
        SignalConfig c = base();
        c.snrDb = -40;
        Result r = run(c, 6);
        CHECK(r.t.blocksOk == 0 && r.t.sats.empty(), "noise only: %llu packets", (unsigned long long)r.t.blocksOk);
    }
    printf(fails ? "orbcomm rx: %d FAILED\n" : "orbcomm rx: all passed\n", fails);
    return fails ? 1 : 0;
}
