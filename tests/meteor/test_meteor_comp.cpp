// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor LRPT colour composites (meteor_comp.h): AVHRR 221 (channel 2 in red and green, channel 1 in blue) built from synthetic channels with the same
// per-channel tables as the other composites, the channel choice of every view, and the images the satellites send (APID 64, 65, 67 on M2-3 / M2-4).
#include "dect2/meteor/meteor_comp.h"
#include "../common/test_util.h"
#include <memory>
#include <vector>
using namespace dect2;

static std::shared_ptr<const MeteorStrip> strip(int row, uint8_t v) {
    auto s = std::make_shared<MeteorStrip>();
    s->row = row;
    s->px.assign((size_t)8 * kMeteorWidth, v);
    return s;
}

int main() {
    // views that do not depend on the picture
    CHECK(meteorViewChannels(nullptr, kMeteorViewAvhrr221) == (std::array<int, 3>{1, 1, 0}), "221 is channels 2 2 1");
    CHECK(meteorViewChannels(nullptr, kMeteorViewVegetation) == (std::array<int, 3>{1, 0, 0}), "vegetation unchanged");
    CHECK(meteorViewChannels(nullptr, 9) == (std::array<int, 3>{0, 1, 3}), "124 unchanged");
    CHECK(meteorViewChannels(nullptr, 1) == (std::array<int, 3>{0, 1, 2}) && meteorViewChannels(nullptr, 2) == (std::array<int, 3>{0, 1, 4}), "123 and 125 unchanged");
    CHECK(meteorViewChannels(nullptr, 5) == (std::array<int, 3>{2, 2, 2}), "single channel is grey");
    CHECK(kMeteorViewCount == kMeteorViewAvhrr221 + 1, "the new view is the last one (the number is saved in the settings)");

    // a pass as Meteor-M N2-4 sends it: APID 64, 65 and 67 only. 221 needs channels 1 and 2, which are there; the automatic view picks 124.
    MeteorImage im;
    im.rows = 2;
    const uint8_t v1 = 40, v2 = 200;                       // channel 1 (visible) dark: sea; channel 2 (near-infrared) bright: land
    for (int c : {0, 1, 3}) {
        im.ch[(size_t)c].resize(2);
        im.packets[(size_t)c] = 10;
    }
    for (int r = 0; r < 2; r++) { im.ch[0][(size_t)r] = strip(r, v1); im.ch[1][(size_t)r] = strip(r, v2); im.ch[3][(size_t)r] = strip(r, 99); }
    CHECK(meteorViewChannels(&im, 0) == (std::array<int, 3>{0, 1, 3}), "automatic picks 124 for APID 64, 65, 67");
    CHECK(meteorViewChannels(&im, kMeteorViewAvhrr221) == (std::array<int, 3>{1, 1, 0}), "221 does not depend on the other channels");

    // the same per-channel tables as the other composites: an identity table and a stretch that differs per channel
    uint8_t lut[6][256];
    for (auto& l : lut) for (int i = 0; i < 256; i++) l[i] = (uint8_t)i;
    std::vector<uint32_t> px((size_t)8 * kMeteorWidth);
    meteorComposeRow(im, meteorViewChannels(&im, kMeteorViewAvhrr221), 0, &lut[0][0], false, px.data());
    const uint32_t want = 0xFF000000u | (uint32_t)v1 << 16 | (uint32_t)v2 << 8 | v2;   // 0xAABBGGRR: R = ch2, G = ch2, B = ch1
    bool all = true;
    for (uint32_t p : px) if (p != want) { all = false; break; }
    CHECK(all, "221 pixel is R=ch2 G=ch2 B=ch1 (got %08X, want %08X)", px[0], want);
    CHECK(((px[0] >> 8) & 255) > ((px[0] >> 16) & 255), "land: green above blue");

    for (int i = 0; i < 256; i++) { lut[0][i] = (uint8_t)(i * 2); lut[1][i] = (uint8_t)(i / 2); }   // channel tables are applied per channel, red and green both through ch2's
    meteorComposeRow(im, {1, 1, 0}, 1, &lut[0][0], false, px.data());
    CHECK((px[5] & 255) == v2 / 2 && ((px[5] >> 8) & 255) == v2 / 2 && ((px[5] >> 16) & 255) == v1 * 2, "tables per channel: %08X", px[5]);

    // sea: channel 2 dark and channel 1 brighter gives blue
    for (int r = 0; r < 2; r++) { im.ch[0][(size_t)r] = strip(r, 120); im.ch[1][(size_t)r] = strip(r, 20); }
    for (auto& l : lut) for (int i = 0; i < 256; i++) l[i] = (uint8_t)i;
    meteorComposeRow(im, meteorViewChannels(&im, kMeteorViewAvhrr221), 0, &lut[0][0], false, px.data());
    CHECK(((px[100] >> 16) & 255) == 120 && ((px[100] >> 8) & 255) == 20 && (px[100] & 255) == 20, "sea is blue: %08X", px[100]);

    // a row that has not arrived is black, not a crash; a channel that was not sent stays black in its colour
    meteorComposeRow(im, {1, 1, 0}, 5, &lut[0][0], false, px.data());
    CHECK(px[0] == 0xFF000000u, "missing row is black: %08X", px[0]);
    meteorComposeRow(im, {1, 1, 4}, 0, &lut[0][0], false, px.data());
    CHECK(((px[0] >> 16) & 255) == 0 && (px[0] & 255) == 20, "channel not sent is 0: %08X", px[0]);
    return testSummary("meteor_comp");
}
