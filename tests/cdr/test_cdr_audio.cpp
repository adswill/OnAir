// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The receiver's audio path against what a real link does to it (issue #138: sound that stopped and never came back):
// a real DRA+ stream (tests/cdr/cdr_dra_clip.h) is fed in units that are damaged, lost, or preceded by a long run of junk.
// The decoder must keep going (skip what is broken) and must recover on its own. Headless: nothing is played.
// Needs the REAL DRA+ add-on (OnAir ships none: docs/CODECS.md, tools/addons/dra); without it the test prints SKIP and passes. The loader and the
// units/frames logic are tested with a fake add-on in test_cdr_dra_addon. Point DECT2_DRA_PATH at your libonair_dra to run it.
#include "../../core/src/cdr/cdr_audio.h"
#include "dect2/cdr/cdr_dra.h"
#include "cdr_dra_clip.h"
#include "cdr_dra_ext_units.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Lcg { uint32_t s = 12345; uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; } };

// the PCM frames (samples) the clip yields when it is cut into units of `unit` bytes and `mangle` changes the list
template <class F> static uint64_t run(size_t unit, F mangle, uint64_t* resets = nullptr) {
    std::vector<std::vector<uint8_t>> units;
    for (size_t i = 0; i < sizeof kDraClip; i += unit) units.emplace_back(kDraClip + i, kDraClip + std::min(sizeof kDraClip, i + unit));
    mangle(units);
    CdrAudio a(true);
    CdrTelemetry t;
    for (const auto& u : units) {
        std::vector<cdr::AudioUnit> v(1);
        v[0].data = u;
        a.play(1, 1, v, 1.f, true, t);
    }
    if (resets) *resets = t.draResets;
    return t.draPcmFrames;
}

int main() {
    if (!cdr::draAvailable()) { printf("SKIP: no DRA+ add-on found (set DECT2_DRA_PATH to libonair_dra)\n"); return 0; }
    const uint64_t clean = run(400, [](auto&) {});
    printf("clean: %llu samples\n", (unsigned long long)clean);
    CHECK(clean >= 60 * 1024, "the real clip should decode (%llu samples)", (unsigned long long)clean);

    // damaged units: every 5th unit has bytes flipped (a block the LDPC passed but the sub-frame header did not cover)
    const uint64_t flipped = run(400, [](auto& u) { Lcg r; for (size_t i = 0; i < u.size(); i += 5) for (int k = 0; k < 12; k++) u[i][r.next() % u[i].size()] ^= (uint8_t)(1 + r.next() % 255); });
    printf("every 5th unit damaged: %llu\n", (unsigned long long)flipped);
    CHECK(flipped >= clean / 2, "damaged units must not stop the decoder (%llu of %llu)", (unsigned long long)flipped, (unsigned long long)clean);

    // lost units: every 4th is missing (a logical frame that did not decode)
    const uint64_t lost = run(400, [](auto& u) { std::vector<std::vector<uint8_t>> o; for (size_t i = 0; i < u.size(); i++) if (i % 4 != 3) o.push_back(u[i]); u.swap(o); });
    printf("every 4th unit lost: %llu\n", (unsigned long long)lost);
    CHECK(lost >= clean / 3, "missing units must not stop the decoder (%llu of %llu)", (unsigned long long)lost, (unsigned long long)clean);

    // a long run of junk first (the receiver locked on the wrong thing): the sound comes back as soon as real units arrive
    uint64_t resets = 0;
    const uint64_t afterJunk = run(400, [](auto& u) {
        Lcg r; std::vector<std::vector<uint8_t>> o;
        for (int i = 0; i < 300; i++) { std::vector<uint8_t> j(300 + r.next() % 400); for (auto& b : j) b = (uint8_t)r.next(); o.push_back(j); }
        o.insert(o.end(), u.begin(), u.end()); u.swap(o);
    }, &resets);
    printf("300 junk units first: %llu (decoder rebuilt %llu times)\n", (unsigned long long)afterJunk, (unsigned long long)resets);
    CHECK(afterJunk >= clean * 7 / 10, "sound must come back after junk (%llu of %llu)", (unsigned long long)afterJunk, (unsigned long long)clean);

    // units cut at other places and very short ones
    const uint64_t odd = run(37, [](auto&) {});
    CHECK(odd >= clean * 9 / 10, "unit size must not matter (%llu of %llu)", (unsigned long long)odd, (unsigned long long)clean);

    // real units with extension bytes behind each frame: every one must give its sound (the old path lost 1 in 10: a false sync in the tail)
    {
        CdrAudio a(true);
        CdrTelemetry t;
        for (const auto& raw : kDraExtUnits) {
            std::vector<cdr::AudioUnit> v(1);
            v[0].data.assign(raw, raw + sizeof raw);
            a.play(1, 1, v, 1.f, true, t);
        }
        printf("48 units with extension bytes: %llu samples\n", (unsigned long long)t.draPcmFrames);
        CHECK(t.draPcmFrames == 48 * 1024, "each unit gives one frame (%llu of %d samples)", (unsigned long long)t.draPcmFrames, 48 * 1024);
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
