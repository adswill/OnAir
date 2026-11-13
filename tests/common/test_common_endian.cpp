// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Checks dect2/common/endian.h against independent byte-by-byte reference code (the shapes of the original per-file
// put16/put32/be32/le32 copies) on random values, including all-zero, all-one and sign-extended inputs.
#include "dect2/common/endian.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    std::mt19937_64 r(99);
    for (int it = 0; it < 20000; it++) {
        uint64_t raw = r(); if (it % 9 == 0) raw = 0; if (it % 13 == 0) raw = ~0ull;
        const uint32_t v32 = (uint32_t)raw; const uint16_t v16 = (uint16_t)raw;
        { std::vector<uint8_t> a, e; endian::putBe16(a, v16); e = {(uint8_t)(v16 >> 8), (uint8_t)v16}; CHECK(a == e, "putBe16");
          a.clear(); endian::putLe16(a, v16); e = {(uint8_t)v16, (uint8_t)(v16 >> 8)}; CHECK(a == e, "putLe16");
          // negative int arguments (the old put16(int) forms) behave as their two's complement bytes
          const int neg = -(int)(raw & 0x7FFF) - 1; a.clear(); endian::putBe16(a, (uint32_t)neg); e = {(uint8_t)(neg >> 8), (uint8_t)neg}; CHECK(a == e, "putBe16 neg"); }
        { std::vector<uint8_t> a, e; endian::putBe32(a, v32); for (int s = 24; s >= 0; s -= 8) e.push_back((uint8_t)(v32 >> s)); CHECK(a == e, "putBe32");
          a.clear(); e.clear(); endian::putLe32(a, v32); for (int i = 0; i < 4; i++) e.push_back((uint8_t)(v32 >> (8 * i))); CHECK(a == e, "putLe32"); }
        { std::vector<uint8_t> a, e; endian::putBe64(a, raw); for (int s = 56; s >= 0; s -= 8) e.push_back((uint8_t)(raw >> s)); CHECK(a == e, "putBe64");
          a.clear(); e.clear(); endian::putLe64(a, raw); for (int i = 0; i < 8; i++) e.push_back((uint8_t)(raw >> (8 * i))); CHECK(a == e, "putLe64"); }
        uint8_t b[16], c[16];
        for (auto& x : b) x = (uint8_t)r();
        CHECK(endian::loadBe16(b) == (uint16_t)((b[0] << 8) | b[1]), "loadBe16");
        CHECK(endian::loadLe16(b) == (uint16_t)(b[0] | b[1] << 8), "loadLe16");
        CHECK(endian::loadLe24(b) == ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16), "loadLe24");
        { uint32_t be = 0, le = 0; for (int i = 0; i < 4; i++) { be = be << 8 | b[i]; le |= (uint32_t)b[i] << (8 * i); } CHECK(endian::loadBe32(b) == be && endian::loadLe32(b) == le, "load32"); }
        { uint64_t be = 0, le = 0; for (int i = 0; i < 8; i++) { be = be << 8 | b[i]; le |= (uint64_t)b[i] << (8 * i); } CHECK(endian::loadBe64(b) == be && endian::loadLe64(b) == le, "load64"); }
        memset(c, 0xAA, 16); endian::storeBe32(c, v32); CHECK(c[0] == (uint8_t)(v32 >> 24) && c[3] == (uint8_t)v32 && c[4] == 0xAA, "storeBe32");
        memset(c, 0xAA, 16); endian::storeLe32(c, v32); CHECK(c[0] == (uint8_t)v32 && c[3] == (uint8_t)(v32 >> 24) && c[4] == 0xAA, "storeLe32");
        memset(c, 0xAA, 16); endian::storeBe16(c, v16); CHECK(c[0] == (uint8_t)(v16 >> 8) && c[1] == (uint8_t)v16 && c[2] == 0xAA, "storeBe16");
        memset(c, 0xAA, 16); endian::storeLe16(c, v16); CHECK(c[0] == (uint8_t)v16 && c[1] == (uint8_t)(v16 >> 8) && c[2] == 0xAA, "storeLe16");
        memset(c, 0xAA, 16); endian::storeBe64(c, raw); CHECK(endian::loadBe64(c) == raw && c[8] == 0xAA, "storeBe64");
    }
    printf(fails ? "test_common_endian: %d FAILED\n" : "test_common_endian: OK\n", fails);
    return fails ? 1 : 0;
}
