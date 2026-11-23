// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #455: the Mesh and Packet table sizes can be raised far past the old 20 times (150 nodes / 100 messages at 1), and 1 stays the default.
#include "dect2/mesh/mesh_rx.h"
#include "dect2/packet/packet_rx.h"
#include <cstdio>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    MeshReceiver m; PacketReceiver p;
    CHECK(m.tableScale() == 1 && p.tableScale() == 1, "default scale is 1");
    for (int s : {5, 20, 100, 200}) {
        m.setTableScale(s); p.setTableScale(s);
        CHECK(m.tableScale() == s, "mesh keeps scale %d, got %d", s, m.tableScale());
        CHECK(p.tableScale() == s, "packet keeps scale %d, got %d", s, p.tableScale());
    }
    m.setTableScale(100000); p.setTableScale(100000);
    CHECK(m.tableScale() == 200 && p.tableScale() == 200, "huge scale clamps to 200");
    m.setTableScale(0); p.setTableScale(-3);
    CHECK(m.tableScale() == 1 && p.tableScale() == 1, "zero or negative clamps to 1");
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
