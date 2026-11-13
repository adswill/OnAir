// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Reverse engineering of an unknown sensor: generated packets with a CRC-8 or sum, a binary or BCD temperature, a counter and an id. Simulated.
#include "dect2/sensors/sensors_re.h"
#include <cmath>
#include <cstdio>
#include <random>
#include "../common/test_util.h"
using namespace dect2::sensors;

static uint8_t crc8(const uint8_t* d, size_t n, uint8_t poly) { uint8_t c = 0; for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = c & 0x80 ? (uint8_t)((c << 1) ^ poly) : (uint8_t)(c << 1); } return c; }

int main() {
    std::mt19937 r(3); std::uniform_real_distribution<double> u(-5, 35);
    std::vector<std::vector<uint8_t>> p, bcd; std::vector<double> v;
    for (int i = 0; i < 14; i++) {
        const double t = std::round(u(r) * 10) / 10; v.push_back(t);
        const int raw = (int)std::lround((t + 40) * 10);
        std::vector<uint8_t> m = {0xA5, 0x3C, (uint8_t)(raw >> 4), (uint8_t)(((raw & 15) << 4) | (r() & 15)), (uint8_t)r()};
        m.push_back(crc8(m.data(), m.size(), 0x31));
        p.push_back(m);
        const int b10 = (int)std::lround(std::fabs(t) * 10);
        std::vector<uint8_t> q = {0x11, (uint8_t)(((b10 / 100) << 4) | ((b10 / 10) % 10)), (uint8_t)(((b10 % 10) << 4) | 0x7)};
        bcd.push_back(q);
    }
    auto c = reFindChecksum(p);
    printf("checksum: %s width %d\n", c.name.c_str(), c.width);
    CHECK(c.found && c.width == 8, "crc found");
    auto f = reFindFields(p, v, "temperature_C");
    CHECK(!f.empty(), "field");
    if (!f.empty()) { printf("%s  %s\n", f[0].text.c_str(), f[0].flexGet.c_str()); CHECK(f[0].bitOffset + f[0].bits == 28 && f[0].bits >= 10 && f[0].kind == 0, "where"); CHECK(std::fabs(f[0].scale - 0.1) < 0.001 && std::fabs(f[0].offset + 40) < 0.5, "scale %.3f offset %.2f", f[0].scale, f[0].offset); }
    std::vector<double> av; for (double x : v) av.push_back(std::fabs(x));
    auto g = reFindFields(bcd, av, "temperature_C");
    CHECK(!g.empty() && g[0].kind == 2 && g[0].bitOffset == 8 && std::fabs(g[0].scale - 0.1) < 0.001, "BCD");
    CHECK(reFindFields(p, {1, 1, 1, 1, 1}).empty(), "no variation");
    CHECK(reFindChecksum({p[0], p[1]}).found == false, "too few");
    // a ramp without any reading
    std::vector<std::vector<uint8_t>> ramp;
    for (int i = 0; i < 30; i++) { const int raw = 300 + i * 2 + (int)(r() % 2); ramp.push_back({0xA5, (uint8_t)(raw >> 8), (uint8_t)raw, (uint8_t)r()}); }
    auto q = reFindRamps(ramp);
    CHECK(!q.empty() && q[0].bitOffset >= 8 && q[0].bitOffset + q[0].bits <= 24, "ramp inside bits 8-23: %s", q.empty() ? "none" : q[0].text.c_str());
    auto h = reParseHexRows("a1 b2 c3\nA1B2C3\nnot hex\n");
    CHECK(h.size() == 2 && h[0] == h[1] && h[0][2] == 0xC3, "hex rows");
    return testSummary("sensors_re");
}
