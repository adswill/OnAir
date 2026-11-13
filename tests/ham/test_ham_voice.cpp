// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The ham voice band plan: segments per IARU region, sideband convention, and the region of a position / locator.
#include "dect2/ham/ham_voice.h"
#include "dect2/ham/ham.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    // regions of known places (the app picks the region from the locator or the position)
    struct P { const char* name; double la, lo; int r; } places[] = {
        {"Vilnius", 54.69, 25.28, 1}, {"London", 51.5, -0.1, 1}, {"Cape Town", -33.9, 18.4, 1}, {"Moscow", 55.75, 37.6, 1}, {"Novosibirsk", 55.0, 82.9, 1},
        {"Dubai", 25.2, 55.3, 1}, {"Riyadh", 24.7, 46.7, 1}, {"Ulaanbaatar", 47.9, 106.9, 1}, {"Tashkent", 41.3, 69.3, 1}, {"Cairo", 30.0, 31.2, 1},
        {"Istanbul", 41.0, 29.0, 1}, {"Reykjavik", 64.1, -21.9, 1}, {"Nairobi", -1.3, 36.8, 1},
        {"New York", 40.7, -74.0, 2}, {"Buenos Aires", -34.6, -58.4, 2}, {"Honolulu", 21.3, -157.9, 2}, {"Anchorage", 61.2, -149.9, 2}, {"Mexico City", 19.4, -99.1, 2},
        {"Sydney", -33.9, 151.2, 3}, {"Tokyo", 35.7, 139.7, 3}, {"Beijing", 39.9, 116.4, 3}, {"Delhi", 28.6, 77.2, 3}, {"Tehran", 35.7, 51.4, 3},
        {"Wellington", -41.3, 174.8, 3}, {"Jakarta", -6.2, 106.8, 3}, {"Auckland", -36.9, 174.8, 3}};
    for (const auto& p : places) {
        if (dect2::ham::hamIaruRegionForPosition(p.la, p.lo) != p.r) { printf("FAIL region of %s: %d, want %d\n", p.name, hamIaruRegionForPosition(p.la, p.lo), p.r); fails++; }
    }
    CHECK(hamIaruRegionForPosition(10, 175) == 3 && hamIaruRegionForPosition(40, -74 + 360) == 2);   // longitudes past 180 wrap
    CHECK(hamIaruRegionForPosition(NAN, 0) == 0 && hamIaruRegionForPosition(100, 0) == 0);
    CHECK(hamIaruRegionForGrid("KO24") == 1 && hamIaruRegionForGrid("ko24ab") == 1 && hamIaruRegionForGrid("FN30") == 2 && hamIaruRegionForGrid("QF56") == 3 && hamIaruRegionForGrid("ZZ99") == 0 && hamIaruRegionForGrid("") == 0);

    for (int reg = 1; reg <= 3; reg++) {
        const auto segs = hamVoiceSegments(reg);
        CHECK(segs.size() > 15);
        bool have[8] = {}; const char* hf[] = {"160m", "80m", "40m", "20m", "17m", "15m", "12m", "10m"};
        for (const auto& s : segs) {
            CHECK(s.loHz < s.hiHz && s.tuneHz >= s.loHz && s.tuneHz <= s.hiHz && s.bwHz > 0);   // the click lands inside its own segment
            CHECK(s.regions & (1u << (reg - 1)));
            // every segment lies inside an amateur band (11 m / CB and broadcast are never in the plan)
            if (strcmp(s.band, "60m")) CHECK(dect2::ham::hamBandAt(s.loHz) && dect2::ham::hamBandAt(s.hiHz));   // the US 60 m channels lie outside the ITU 60 m table
            if (s.mode != VoiceMode::Nfm) {
                CHECK(s.mode == hamVoiceSideband(s.tuneHz));   // SSB: LSB below 10 MHz, USB above (60 m USB)
                CHECK(s.bwHz == 2700);
            } else CHECK(s.loHz > 29e6 && s.bwHz == 12500);    // FM only on 10 m and VHF / UHF
            for (int i = 0; i < 8; i++) if (!strcmp(s.band, hf[i])) have[i] = true;
        }
        for (int i = 0; i < 8; i++) { if (!have[i]) printf("missing %s in region %d\n", hf[i], reg); CHECK(have[i]); }
        bool v6 = false, v2 = false, v2fm = false, v70 = false, v70fm = false;
        for (const auto& s : segs) {
            v6 |= !strcmp(s.band, "6m"); v2 |= !strcmp(s.band, "2m") && s.mode == VoiceMode::Usb; v2fm |= !strcmp(s.band, "2m") && s.mode == VoiceMode::Nfm;
            v70 |= !strcmp(s.band, "70cm") && s.mode == VoiceMode::Usb; v70fm |= !strcmp(s.band, "70cm") && s.mode == VoiceMode::Nfm;
        }
        CHECK(v6 && v2 && v2fm && v70 && v70fm);
    }
    // the regions differ where the IARU plans do
    CHECK(hamVoiceSegmentAt(hamVoiceSegments(1), 3.9e6) == nullptr && hamVoiceSegmentAt(hamVoiceSegments(2), 3.9e6) != nullptr);   // 80 m: 3.6-3.8 in R1, to 4.0 in R2
    CHECK(hamVoiceSegmentAt(hamVoiceSegments(1), 7.250e6) == nullptr && hamVoiceSegmentAt(hamVoiceSegments(2), 7.250e6) != nullptr);
    CHECK(hamVoiceSegmentAt(hamVoiceSegments(2), 7.100e6) == nullptr && hamVoiceSegmentAt(hamVoiceSegments(1), 7.100e6) != nullptr);
    const auto* s = hamVoiceSegmentAt(hamVoiceSegments(1), 145.5e6);
    CHECK(s && s->mode == VoiceMode::Nfm && !strcmp(s->band, "2m"));
    s = hamVoiceSegmentAt(hamVoiceSegments(2), 146.52e6);
    CHECK(s && s->mode == VoiceMode::Nfm);
    s = hamVoiceSegmentAt(hamVoiceSegments(1), 144.3e6);
    CHECK(s && s->mode == VoiceMode::Usb && s->tuneHz == 144.3e6);
    CHECK(hamVoiceSegmentAt(hamVoiceSegments(1), 14.074e6) == nullptr && hamVoiceSegmentAt(hamVoiceSegments(1), 14.2e6) != nullptr);   // FT8 is below the phone segment
    CHECK(hamVoiceSegmentAt(hamVoiceSegments(1), 27.185e6) == nullptr);   // CB is not a ham band: not in the plan
    CHECK(hamVoiceSegmentAt(hamVoiceSegments(1), 10.12e6) == nullptr);    // 30 m has no voice
    CHECK(hamVoiceSideband(3.7e6) == VoiceMode::Lsb && hamVoiceSideband(7.1e6) == VoiceMode::Lsb && hamVoiceSideband(14.2e6) == VoiceMode::Usb &&
          hamVoiceSideband(5.357e6) == VoiceMode::Usb && hamVoiceSideband(144.3e6) == VoiceMode::Usb);
    CHECK(hamVoiceSegments(99).size() == hamVoiceSegments(1).size());
    printf(fails ? "ham voice: %d FAILED\n" : "ham voice: all passed\n", fails);
    return fails ? 1 : 0;
}
