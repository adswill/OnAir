// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Share-ready spectrum / waterfall snapshot: caption text, picture size, the caption strip is drawn, the PNG decodes back.
#include "../../app/clips/clips.h"
#include "dect2/common/png.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    clips::SnapCaption cap;
    cap.mode = "ADS-B"; cap.freqHz = 1090e6; cap.epoch = 1700000000; cap.version = "4.6.0";
    // caption text
    {
        const std::string t = clips::snapCaption(cap);
        CHECK(t.find("ADS-B") != std::string::npos);
        CHECK(t.find("1090 MHz") != std::string::npos);
        CHECK(t.find("2023-11-14 22:13 UTC") != std::string::npos);
        CHECK(t.find("OnAir 4.6.0") != std::string::npos);
        CHECK(t.find("Dubai") == std::string::npos);                    // no place unless given
        cap.place = "Dubai";
        CHECK(clips::snapCaption(cap).find("Dubai") != std::string::npos);
        cap.place = "";
    }
    // picture: 64 columns x 40 rows of levels, a ramp in the newest row
    {
        const int w = 64, n = 40;
        std::vector<uint8_t> lv((size_t)w * n, 200);
        for (int x = 0; x < w; x++) lv[(size_t)x] = (uint8_t)(x * 4);
        uint32_t lut[256];
        for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | (uint32_t)i << 8;   // 0xAABBGGRR: green ramp
        clips::Image im = clips::shareSnapshot(lv, w, n, lut, cap, 640);
        CHECK(im.w == 640);
        CHECK(im.h > 200 && im.h < 900);
        CHECK(im.rgba.size() == (size_t)im.w * im.h * 4);
        // the caption strip (bottom) is not blank: has light pixels
        int light = 0;
        for (int y = im.h - 60; y < im.h; y++) for (int x = 0; x < im.w; x++) if (im.rgba[((size_t)y * im.w + x) * 4] > 200) light++;
        CHECK(light > 50);
        // the waterfall part shows the level colours (some pixel has a green channel > 100)
        int green = 0;
        for (int y = im.h / 2; y < im.h / 2 + 20; y++) for (int x = 0; x < im.w; x++) if (im.rgba[((size_t)y * im.w + x) * 4 + 1] > 100) green++;
        CHECK(green > 0);
        // PNG round trip
        const auto png = clips::encodePng(im.rgba.data(), im.w, im.h);
        int w2 = 0, h2 = 0; std::vector<uint8_t> back;
        CHECK(dect2::decodePng(png.data(), png.size(), w2, h2, back));
        CHECK(w2 == im.w && h2 == im.h);
    }
    // empty input still gives a captioned picture, never a crash
    {
        uint32_t lut[256] = {};
        clips::Image im = clips::shareSnapshot({}, 0, 0, lut, cap, 640);
        CHECK(im.w == 640 && im.h > 0);
    }
    // file name
    CHECK(clips::snapshotFileName(cap) == "onair_ads-b_1090MHz_20231114-221320.png");
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
