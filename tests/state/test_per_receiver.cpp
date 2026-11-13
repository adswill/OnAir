// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Auto gain state per receiver (#450): two receivers never see each other's object.
#include "../../app/state/per_receiver.h"
#include <cstdio>
#include "../common/test_util.h"

struct Thing { int v = 0; };
static Thing& rx(int id) { return perReceiver<Thing, 4>(id); }

int main() {
    rx(1).v = 11;
    rx(2).v = 22;
    CHECK(rx(1).v == 11, "RX 1 lost its state");
    CHECK(rx(2).v == 22, "RX 2 lost its state");
    CHECK(&rx(1) != &rx(2), "RX 1 and RX 2 share one object");
    CHECK(&rx(4) != &rx(1), "RX 4 shares RX 1's object");
    CHECK(&rx(0) == &rx(5), "out of range ids share the spare slot");
    CHECK(&rx(0) != &rx(1), "the spare slot is a real receiver's");
    printf("ok\n");
    return fails ? 1 : 0;
}
