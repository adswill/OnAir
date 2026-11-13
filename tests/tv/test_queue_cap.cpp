// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Player picture queue: the cap follows the frame rate and the picture size; over the cap the late pictures go first, then the newest.
#include "dect2/tv/player.h"
#include <cstdio>
#include <deque>
#include <memory>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::deque<std::shared_ptr<dect2::VideoFrame>> make(int n, double step) {
    std::deque<std::shared_ptr<dect2::VideoFrame>> q;
    for (int i = 0; i < n; i++) { auto f = std::make_shared<dect2::VideoFrame>(); f->pts = i * step; q.push_back(f); }
    return q;
}

int main() {
    const size_t hd = 1920 * 1080 * 3 / 2, uhd = 3840 * 2160 * 3 / 2;
    CHECK(dect2::videoQueueCap(25, hd) == 88);                 // 3.5 s, above the old 80
    CHECK(dect2::videoQueueCap(50, hd) == 175);                // 1080p50 with the video 1.6 s ahead of the audio needs ~90
    CHECK(dect2::videoQueueCap(10, hd) == 80);                 // never below 80
    CHECK(dect2::videoQueueCap(0, hd) == 88);                  // unknown rate: 25 fps
    CHECK(dect2::videoQueueCap(60, uhd) == 64);                // 4K bounded by memory (768 MB / 12.4 MB)
    CHECK(dect2::videoQueueCap(60, (size_t)200 << 20) == 30);  // enormous pictures: the floor of 30
    {   // 100 pictures at 50 fps, clock at 0.5 s: 25 are late. Cap 90 -> the 10 oldest late ones go, the due ones stay
        auto q = make(100, 0.02);
        CHECK(dect2::trimVideoQueue(q, 90, 0.5, true) == 10);
        CHECK(q.size() == 90 && q.front()->pts > 0.19 && q.back()->pts > 1.97);
    }
    {   // nothing late: the newest are dropped, the next due stays first
        auto q = make(100, 0.02);
        CHECK(dect2::trimVideoQueue(q, 90, -1.0, true) == 10);
        CHECK(q.size() == 90 && q.front()->pts == 0 && q.back()->pts < 1.8);
    }
    {   // no clock yet: newest dropped
        auto q = make(100, 0.02);
        CHECK(dect2::trimVideoQueue(q, 80, 5.0, false) == 20);
        CHECK(q.front()->pts == 0);
    }
    {   auto q = make(50, 0.02); CHECK(dect2::trimVideoQueue(q, 80, 0, true) == 0 && q.size() == 50); }
    if (!fails) printf("queue cap: all passed\n");
    return fails ? 1 : 0;
}
