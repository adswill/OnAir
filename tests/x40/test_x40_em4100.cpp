// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EM4100 LF RFID: ids and polarities, RF/64 and RF/32, a bit clock that is 4 % off, a carrier at the edge of the channel, a drifting carrier, a noise sweep
// with the false-read count, and 15 minutes of noise alone. Simulated signals only.
#include "x40_gen2.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool has(const X40Telemetry& t, uint64_t id) {
    char b[16]; snprintf(b, sizeof b, "%02X%08X", (unsigned)(id >> 32) & 0xFF, (unsigned)(id & 0xFFFFFFFFu));
    for (auto& r : t.rows) if (r.kind == "Tag" && r.id == b) return true;
    return false;
}
static size_t tags(const X40Telemetry& t) { size_t n = 0; for (auto& r : t.rows) if (r.kind == "Tag") n++; return n; }

int main() {
    const double fs = 250000;
    for (uint64_t id : {0x0123456789ull, 0xFFFFFFFFFFull, 0x0000000001ull, 0xA55A5AA5C3ull}) for (bool inv : {false, true}) for (double rf : {64.0, 32.0}) {
        EmTag g; g.id = id; g.invert = inv; g.rfDiv = rf;
        auto x = emTag(g, fs, 1.0);
        auto t = run(x40::Em4100, x, fs);
        CHECK(has(t, id), "id %llx inv %d RF/%.0f not read (rows %zu)", (unsigned long long)id, (int)inv, rf, t.rows.size());
    }
    // clock error: 4 % and -4 %
    for (double ppm : {40000.0, -40000.0}) {
        EmTag g; g.clockPpm = ppm; auto x = emTag(g, fs, 1.0);
        CHECK(has(run(x40::Em4100, x, fs), g.id), "clock %.0f ppm", ppm);
    }
    // carrier offset inside the channel, and drifting by 800 Hz/s
    for (double off : {0.0, 2500.0, -2500.0}) {
        EmTag g; auto x = emTag(g, fs, 1.0); mix(x, off, off == 0 ? 800 : 0, fs);
        CHECK(has(run(x40::Em4100, x, fs), g.id), "offset %.0f Hz", off);
    }
    // an off-centre tuning that the receiver is told about
    { EmTag g; auto x = emTag(g, fs, 1.0); mix(x, 20000, 0, fs); CHECK(has(run(x40::Em4100, x, fs, 20000), g.id), "off-centre 20 kHz"); }
    // noise sweep: tag modulation 8 %; carrier to noise in the 5 kHz envelope band; reads in 2 s of signal (about 7 frames a second per 1953 bit/s)
    printf("noise sweep, 8 %% depth, carrier to noise in 10 kHz, 3 runs each:\n");
    for (double cnr : {60.0, 50.0, 45.0, 40.0, 35.0}) {
        int ok = 0;
        for (unsigned s = 1; s <= 3; s++) {
            EmTag g; g.id = 0x1234ABCD99ull + s; auto x = emTag(g, fs, 2.0); addNoise(x, noiseFor(cnr, 10000, fs), s);
            auto t = run(x40::Em4100, x, fs); if (has(t, g.id)) ok++;
            CHECK(tags(t) <= 1, "false extra tag at %.0f dB", cnr);
        }
        printf("  %.0f dB: %d of 3 read\n", cnr, ok);
        if (cnr >= 45) CHECK(ok == 3, "%.0f dB: %d of 3", cnr, ok);
    }
    // 15 minutes of noise (carrier + noise, no tag): no tag
    {
        const double sec = 900; size_t falseTags = 0;
        for (int k = 0; k < 15; k++) {
            std::vector<cf32> x((size_t)(fs * 60), cf32(1, 0)); addNoise(x, noiseFor(40, 10000, fs), 100 + k);
            falseTags += tags(run(x40::Em4100, x, fs));
        }
        printf("noise and carrier only, %.0f s: %zu false tags\n", sec, falseTags);
        CHECK(falseTags == 0, "false tags %zu", falseTags);
    }
    // a different tag after the first: two rows
    {
        EmTag g; auto a = emTag(g, fs, 1.0); g.id = 0x0AABBCCDDull; auto b = emTag(g, fs, 1.0); a.insert(a.end(), b.begin(), b.end());
        auto t = run(x40::Em4100, a, fs); CHECK(tags(t) == 2, "two tags: %zu rows", tags(t));
    }
    printf(fails ? "x40 em4100: %d FAILED\n" : "x40 em4100: ok\n", fails);
    return fails ? 1 : 0;
}
