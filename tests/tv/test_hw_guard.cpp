// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hardware video start guard: mid-GOP join, silent GPU, intra-refresh stream, copy-out failures.
#include "dect2/tv/hw_guard.h"
#include <cstdio>
using dect2::HwStartGuard;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    {   // joined mid-GOP: non-key packets are held back, the keyframe starts the session, pictures come out: never gives up
        HwStartGuard g; g.arm(0);
        for (int i = 0; i < 7; i++) CHECK(!g.feed(false, 0.02 * i));
        CHECK(g.feed(true, 0.2));
        CHECK(g.feed(false, 0.24));
        g.gotPicture();
        CHECK(!g.giveUp(60));
    }
    {   // the GPU takes the keyframe but never returns a picture: give up after the time and packet counts
        HwStartGuard g; g.arm(0);
        CHECK(g.feed(true, 1.0));
        for (int i = 0; i < 100; i++) g.feed(false, 1.0 + 0.02 * i);
        CHECK(!g.giveUp(2.5));   // 1.5 s fed: too early
        CHECK(g.giveUp(5.1));
    }
    {   // slow stream: few packets so far, do not give up on time alone
        HwStartGuard g; g.arm(0);
        g.feed(true, 0); for (int i = 0; i < 5; i++) g.feed(false, 1 + i);
        CHECK(!g.giveUp(20));
    }
    {   // gradual-refresh stream without any keyframe: after a long wait the CPU decoder takes over
        HwStartGuard g; g.arm(0);
        for (int i = 0; i < 200; i++) g.feed(false, 0.04 * i);
        CHECK(!g.giveUp(4));
        CHECK(g.giveUp(8.5));
    }
    {   // issue #203: the GPU takes the start but returns nothing; 3 s of input is enough to switch to software
        HwStartGuard g; g.arm(0);
        CHECK(g.feed(true, 0.0));
        for (int i = 1; i <= 31; i++) g.feed(false, 0.1 * i);   // 3.1 s, 32 packets
        CHECK(g.giveUp(3.1));
    }
    {   // a RASL-led start (no IRAP) is held back like any other non-random-access packet
        HwStartGuard g; g.arm(0);
        CHECK(!g.feed(false, 0.0));
        CHECK(!g.feed(false, 0.02));
        CHECK(g.feed(true, 0.04));
    }
    {   // copy-out failures with no picture shown
        HwStartGuard g; g.arm(0); g.feed(true, 0);
        for (int i = 0; i < 4; i++) g.fetchFailed();
        CHECK(!g.giveUp(0.1));
        g.fetchFailed();
        CHECK(g.giveUp(0.1));
    }
    printf(fails ? "hw_guard: FAILED\n" : "hw_guard: all passed\n");
    return fails ? 1 : 0;
}
