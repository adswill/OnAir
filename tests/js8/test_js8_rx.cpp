// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// JS8Call receiver against generated stations: the test band (heartbeat, CQ, directed SNR report, a multi-frame free text message, a
// relay, fast and turbo) with messages put together and grids on the map; then impairments: a tuning error, drift, early and late
// starts (DT -1.2 / +2.0 s), a weak signal, five signals in one slot close in frequency, and noise only (nothing decoded).
#include "dect2/js8/js8_gen.h"
#include "dect2/js8/js8_proto.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/js8/js8_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000;
constexpr double kOff = -20000;

static Js8Telemetry run(const std::vector<Js8GenStation>& st, double secs, double cfo = 0, uint32_t seed = 5) {
    Js8GenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.stations = st; c.seed = seed; c.slotPhaseSec = 0;
    Js8Generator g(c);
    Js8Receiver r;
    r.setStartTime(0);
    r.configure(kRate);
    r.setSignalOffset(kOff);
    std::vector<cf32> x(25000);
    for (double t = 0; t < secs; t += 0.1) { g.generate(x.data(), x.size()); r.feed(x.data(), x.size()); }
    r.waitIdle();
    Js8Telemetry out;
    r.telemetry(out, 0);
    return out;
}

static bool hasDecode(const Js8Telemetry& t, const std::string& text, const char* name, float* snr = nullptr, float* dt = nullptr, float* hz = nullptr) {
    for (const auto& d : t.decodes) if (d.text == text) { if (snr) *snr = d.snrDb; if (dt) *dt = d.dt; if (hz) *hz = d.hz; printf("  %-12s %+5.1f dB %+5.2f s %7.1f Hz %s  \"%s\"\n", name, d.snrDb, d.dt, d.hz, js8::spec(d.submode).name, d.text.c_str()); return true; }
    printf("  %-12s MISSING \"%s\"\n", name, text.c_str());
    return false;
}

static const Js8Message* findMsg(const Js8Telemetry& t, const std::string& text) {
    for (const auto& m : t.messages) if (m.text == text) return &m;
    return nullptr;
}

static Js8GenStation hb(const char* call, const char* grid, double hz, double snr, int sub = 0) {
    Js8GenStation s;
    s.kind = Js8GenStation::Heartbeat; s.call = call; s.grid = grid; s.hz = hz; s.snrDb = snr; s.submode = sub; s.everySlots = 0;
    return s;
}

int main() {
    {   // the test band: 61 s, every kind of message
        const Js8Telemetry t = run(js8TestStations(), 61);
        CHECK(hasDecode(t, "LY2ABC: @HB HEARTBEAT KO24", "heartbeat"), "heartbeat");
        CHECK(hasDecode(t, "DL1XYZ: @ALLCALL CQ CQ CQ JO62", "cq"), "cq");
        CHECK(hasDecode(t, "OK1XX: LY2ABC SNR -12", "snr report"), "directed");
        CHECK(hasDecode(t, "YL2XYZ: @HB HEARTBEAT KO26", "fast"), "fast");
        CHECK(hasDecode(t, "ES1ABC: @ALLCALL CQ KO29", "turbo"), "turbo");
        const Js8Message* m = findMsg(t, "G4ABC: DL1XYZ HELLO FROM LONDON. HOW COPY?");
        CHECK(m && m->complete && m->frames >= 3, "free text message put together (%s)", m ? "incomplete" : "missing");
        const Js8Message* r = findMsg(t, "EA3XYZ: OK1XX>SP5AB PLEASE RELAY. TNX");
        CHECK(r && r->relay && r->relayTo == "SP5AB" && r->to == "OK1XX", "relay (%s)", r ? r->relayTo.c_str() : "missing");
        for (const auto& x : t.messages) printf("  message %s%s\n", x.text.c_str(), x.complete ? "" : " ...");
        bool ly = false, dl = false;
        for (const auto& s : t.stations) {
            if (s.call == "LY2ABC" && s.hasPos && std::fabs(s.lat - 54.5) < 0.6 && std::fabs(s.lon - 25) < 1.1) ly = true;
            if (s.call == "DL1XYZ" && s.hasPos && s.cq) dl = true;
        }
        CHECK(ly && dl, "stations on the map");
        CHECK(t.slots[0] >= 4 && t.slots[1] >= 5 && t.slots[2] >= 9, "slots %llu %llu %llu", (unsigned long long)t.slots[0], (unsigned long long)t.slots[1], (unsigned long long)t.slots[2]);
        float snr = 0;
        hasDecode(t, "G4ABC: DL1XYZ ", "snr check", &snr);
        CHECK(std::fabs(snr - -4) < 3, "SNR %.1f (sent -4)", snr);
    }
    {   // impairments: tuning error, drift, early and late starts, weak
        std::vector<Js8GenStation> v = {hb("LY1AA", "KO24", 800, -10), hb("LY2BB", "KO25", 1300, -10), hb("LY3CC", "KO14", 1800, -10), hb("LY4DD", "KO15", 2300, -17)};
        v[0].driftHzPerMin = 3;
        v[1].dt = 2.0;
        v[2].dt = -1.2;
        for (auto& s : v) s.firstSlot = 1;   // the second slot: an early start begins before the first one ends
        const Js8Telemetry t = run(v, 31.5, 45);
        float dt = 0, hz = 0;
        CHECK(hasDecode(t, "LY1AA: @HB HEARTBEAT KO24", "drift", nullptr, nullptr, &hz) && std::fabs(hz - 845) < 3, "drift / tuning error at %.1f Hz", hz);
        CHECK(hasDecode(t, "LY2BB: @HB HEARTBEAT KO25", "late +2.0", nullptr, &dt) && std::fabs(dt - 2.0) < 0.05, "late start, DT %.2f", dt);
        CHECK(hasDecode(t, "LY3CC: @HB HEARTBEAT KO14", "early -1.2", nullptr, &dt) && std::fabs(dt + 1.2) < 0.05, "early start, DT %.2f", dt);
        CHECK(hasDecode(t, "LY4DD: @HB HEARTBEAT KO15", "weak -17 dB"), "weak");
    }
    {   // five signals in one slot, 60 Hz apart, one strong next to a weak one
        std::vector<Js8GenStation> v;
        const char* calls[5] = {"SM5AAA", "SM5BBB", "SM5CCC", "SM5DDD", "SM5EEE"};
        for (int i = 0; i < 5; i++) v.push_back(hb(calls[i], "JO89", 1000 + 60 * i, i == 2 ? 0 : -12));
        const Js8Telemetry t = run(v, 16.5);
        int n = 0;
        for (int i = 0; i < 5; i++) n += hasDecode(t, std::string(calls[i]) + ": @HB HEARTBEAT JO89", calls[i]);
        CHECK(n == 5, "%d of 5 crowded signals", n);
    }
    {   // fast and turbo with a time offset and tuning error
        std::vector<Js8GenStation> v = {hb("OH2AA", "KP20", 900, -8, 1), hb("OH3BB", "KP21", 2000, -6, 2)};
        v[0].dt = 1.0; v[1].dt = -0.5;
        v[1].firstSlot = 1;
        const Js8Telemetry t = run(v, 12.5, -30);
        CHECK(hasDecode(t, "OH2AA: @HB HEARTBEAT KP20", "fast +1.0"), "fast offsets");
        CHECK(hasDecode(t, "OH3BB: @HB HEARTBEAT KP21", "turbo -0.5"), "turbo offsets");
    }
    {   // "Listen": the same samples into two receivers, one listening (silent, through the tap): the same decodes, 8000 audio samples a second
        Js8GenConfig c;
        c.rate = kRate; c.dialOffsetHz = kOff; c.stations = {hb("LY2ABC", "KO24", 1200, -6)}; c.seed = 7; c.slotPhaseSec = 0;
        Js8Generator g(c);
        Js8Receiver off, on;
        for (Js8Receiver* r : {&off, &on}) { r->setStartTime(0); r->configure(kRate); r->setSignalOffset(kOff); r->setSilent(true); }
        CHECK(!off.listening(), "Listen should be off by default");
        on.setListen(true);
        size_t heard = 0;
        on.setListenTap([&](const float*, size_t n) { heard += n; });
        const double secs = 16;
        std::vector<cf32> x(25000);
        for (double t = 0; t < secs - 1e-9; t += 0.1) { g.generate(x.data(), x.size()); off.feed(x.data(), x.size()); on.feed(x.data(), x.size()); }
        off.waitIdle(); on.waitIdle();
        Js8Telemetry a, b;
        off.telemetry(a, 0); on.telemetry(b, 0);
        bool same = a.decodes.size() == b.decodes.size();
        for (size_t i = 0; same && i < a.decodes.size(); i++) same = a.decodes[i].text == b.decodes[i].text && a.decodes[i].hz == b.decodes[i].hz;
        printf("  listen: %zu audio samples in %.0f s, %zu decodes %s\n", heard, secs, b.decodes.size(), same ? "the same" : "DIFFERENT");
        CHECK(std::fabs((double)heard - secs * 8000) < 400, "listen audio: %zu samples", heard);
        CHECK(!a.decodes.empty() && same, "decodes with listen differ");
    }
    {   // noise only
        const Js8Telemetry t = run({}, 31);
        printf("  noise only: %zu decodes, %llu slots\n", t.decodes.size(), (unsigned long long)(t.slots[0] + t.slots[1] + t.slots[2]));
        CHECK(t.decodes.empty(), "noise decoded %zu frames", t.decodes.size());
    }
    printf(fails ? "js8 rx: %d FAILED\n" : "js8 rx: all passed\n", fails);
    return fails ? 1 : 0;
}
