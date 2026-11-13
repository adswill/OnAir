// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drones (dji_rx.h) with the test signal (dji_gen.h) at 20 Msps: DroneID bursts decode to the serial number and position sent, with a carrier offset,
// at lower SNR, with the first symbol left out and at another sample rate; the OcuSync-like (10 and 20 MHz) and Lightbridge-like trains are classified by
// their shape; Wi-Fi-like wideband bursts and noise are not taken for DJI; DroneID bursts that collide with Wi-Fi-like bursts are reported as they are.
// The OcuSync-like and Lightbridge-like trains are our own stand-ins for the described shapes, not recordings: this checks the detector's logic, not that
// real DJI links look like this.
#include "dect2/dji/dji_gen.h"
#include "dect2/dji/dji_rx.h"
#include "dect2/dji/dji_tel.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Opt { int scen = 1; int eight = 0, wide = 0; double rate = 20e6, snr = 25, cfo = 0, secs = 3.5, period = 0.64; bool noiseOnly = false; };

static DjiTelemetry run(const Opt& o) {
    SynthConfig sc;
    sc.modeOpt[0] = o.scen; sc.modeOpt[1] = o.eight; sc.modeOpt[2] = o.wide; sc.modeVal[0] = o.period; sc.snrDb = o.snr; sc.cfoHz = o.cfo;
    std::unique_ptr<ModeSynth> g = makeDjiSynth(sc, o.rate);
    DjiReceiver r;
    r.configure(o.rate);
    r.setCenterHz(2440e6);
    genutil::NoiseSource ns(3);
    std::vector<cf32> buf(65536);
    DjiTelemetry last;
    uint64_t seq = 0;
    const size_t total = (size_t)(o.secs * o.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        if (o.noiseOnly) { for (auto& v : buf) v = 0; ns.add(buf.data(), buf.size(), 0.05f); } else g->generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        DjiTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

static const DjiTrack* find(const DjiTelemetry& t, int cls) {
    const DjiTrack* b = nullptr;
    for (auto& k : t.tracks) if (k.cls == cls && (!b || k.bursts > b->bursts)) b = &k;
    return b;
}

static void droneId(const char* what, const Opt& o, int minPackets) {
    const DjiTelemetry t = run(o);
    printf("  %s: %llu packets ok, %llu failed, %zu drones, %zu tracks, %s\n", what, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.drones.size(), t.tracks.size(), djiSummary(t).c_str());
    CHECK((int)t.blocksOk >= minPackets, "%s: %llu packets decoded, wanted %d", what, (unsigned long long)t.blocksOk, minPackets);
    CHECK(t.drones.size() == 1, "%s: %zu drones", what, t.drones.size());
    if (t.drones.empty()) return;
    const DjiDrone& d = t.drones[0];
    CHECK(d.serial == kDjiGenSerial, "%s: serial '%s'", what, d.serial.c_str());
    CHECK(std::abs(d.last.lat() - 51.4468) < 0.001 && std::abs(d.last.lon() - 7.2679) < 0.001, "%s: position %.5f %.5f", what, d.last.lat(), d.last.lon());
    CHECK(std::abs(d.last.homeLat() - 51.4468438) < 1e-4 && std::abs(d.last.appLat() - 43.2682071) < 1e-4, "%s: home / pilot position", what);
    CHECK(d.track.size() >= 2, "%s: trail %zu", what, d.track.size());
    const DjiTrack* k = find(t, kDjiDroneId);
    CHECK(k && k->decoded > 0 && k->conf > 0.9f && k->bwMhz > 7.5 && k->bwMhz < 11, "%s: the track is DroneID, 9 MHz wide (%.1f MHz)", what, k ? k->bwMhz : 0.0);
    CHECK(k && std::abs(k->freqMhz - 2435.0) < 0.5, "%s: frequency %.2f MHz", what, k ? k->freqMhz : 0.0);
}

int main() {
    const ModeTuning mt = djiTuning();
    CHECK(mt.stdMode == 70 && modeTuningById("dji") && modeTuningById("dji")->stdMode == 70, "tuning table");
    { Opt o; droneId("clean", o, 4); }
    { Opt o; o.cfo = 31000; o.snr = 18; droneId("offset 31 kHz, 18 dB", o, 3); }
    { Opt o; o.cfo = -52000; o.snr = 12; o.secs = 5.0; droneId("offset -52 kHz, 12 dB", o, 3); }
    { Opt o; o.eight = 1; droneId("8 symbols", o, 3); }
    { Opt o; o.rate = 25e6; o.cfo = 9000; droneId("25 Msps", o, 3); }
    { Opt o; o.rate = 61.44e6; o.secs = 2.5; droneId("61.44 Msps", o, 2); }
    { Opt o; o.scen = 5; o.secs = 6.0; o.snr = 20; Opt q = o; const DjiTelemetry t = run(q);
      printf("  with colliding Wi-Fi-like bursts: %llu ok, %llu failed, %zu drones\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.drones.size());
      CHECK(t.drones.size() <= 1, "no invented drones"); for (auto& d : t.drones) CHECK(d.serial == kDjiGenSerial, "serial of a collided burst '%s'", d.serial.c_str()); }
    // shape classes
    {
        Opt o; o.scen = 2; o.secs = 2.0;
        const DjiTelemetry t = run(o);
        const DjiTrack* k = find(t, kDjiOcusync);
        printf("  OcuSync-like 10 MHz: %s\n", k ? k->note.c_str() : "no track");
        CHECK(k && k->conf > 0.6f && std::abs(k->bwMhz - 9.0) < 1.5 && std::abs(k->periodMs - 2.0) < 0.1 && std::abs(k->durUs - 1000) < 60, "OcuSync-like 10 MHz: track %s", k ? djiSummary(t).c_str() : "none");
        CHECK(t.drones.empty() && !find(t, kDjiDroneId) && !find(t, kDjiLightbridge), "no DroneID / Lightbridge from an OcuSync-like train");
    }
    {
        Opt o; o.scen = 2; o.wide = 1; o.secs = 2.0;
        const DjiTelemetry t = run(o);
        const DjiTrack* k = find(t, kDjiOcusync);
        printf("  OcuSync-like 20 MHz: OFDM %.2f, %.1f MHz wide, period %.2f ms\n", k ? k->ofdm : 0.f, k ? k->bwMhz : 0.0, k ? k->periodMs : 0.0);
        CHECK(k && k->conf > 0.5f && k->bwMhz > 15.5, "OcuSync-like 20 MHz: %s (%.1f MHz)", k ? "found" : "none", k ? k->bwMhz : 0.0);
    }
    {
        Opt o; o.scen = 3; o.secs = 2.0;
        const DjiTelemetry t = run(o);
        const DjiTrack* k = find(t, kDjiLightbridge);
        printf("  Lightbridge-like: %s\n", k ? k->note.c_str() : "no track");
        CHECK(k && k->conf > 0.6f && std::abs(k->periodMs - 1.0) < 0.05, "Lightbridge-like: %s", k ? djiSummary(t).c_str() : "none");
        CHECK(!find(t, kDjiOcusync), "Lightbridge-like train called OcuSync");
    }
    {
        Opt o; o.scen = 0; o.secs = 5.0; o.snr = 22;
        const DjiTelemetry t = run(o);
        CHECK(t.drones.size() == 1 && find(t, kDjiDroneId), "all together: DroneID %zu drones", t.drones.size());
        CHECK(find(t, kDjiOcusync) != nullptr, "all together: the OcuSync-like train");
        printf("  all together: %zu tracks, %llu bursts\n", t.tracks.size(), (unsigned long long)t.burstsTotal);
        for (auto& k : t.tracks) printf("    %-26s %5.1f MHz at %+6.2f, OFDM %.2f, period %.2f ms, conf %.2f, %d bursts\n", djiClassName(k.cls).c_str(), k.bwMhz, k.offsetHz / 1e6, k.ofdm, k.periodMs, k.conf, k.bursts);
    }
    // not DJI
    {
        Opt o; o.scen = 4; o.secs = 3.0;
        const DjiTelemetry t = run(o);
        printf("  Wi-Fi-like bursts: %llu bursts, %zu tracks\n", (unsigned long long)t.burstsTotal, t.tracks.size());
        CHECK(t.burstsTotal > 50, "the wideband bursts were seen: %llu", (unsigned long long)t.burstsTotal);
        int dji = 0;
        for (auto& k : t.tracks) dji += k.cls >= kDjiUnknownOfdm;
        CHECK(dji == 0 && t.drones.empty() && t.state <= 1, "Wi-Fi-like bursts taken for DJI: %d tracks, state %d", dji, t.state);
    }
    {
        Opt o; o.noiseOnly = true; o.secs = 2.0;
        const DjiTelemetry t = run(o);
        CHECK(t.tracks.empty() && t.drones.empty() && t.state == 0, "noise alone: %zu tracks", t.tracks.size());
    }
    // a rate the receiver cannot work with
    {
        DjiReceiver r; r.configure(10e6);
        CHECK(!r.ready(), "10 Msps is not enough");
    }
    printf(fails ? "dji rx: %d FAILED\n" : "dji rx: all passed\n", fails);
    return fails ? 1 : 0;
}
