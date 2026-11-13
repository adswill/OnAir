// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Compares dect2/common/bits.h and linecode.h against the ORIGINAL per-protocol implementations (embedded as reference
// code) on random inputs: popcount/parity variants, Gray, bit reversal, bit field readers/writers, byte<->bit packing, NRZI, Manchester.
#include "dect2/common/bits.h"
#include "dect2/common/linecode.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    std::mt19937_64 r(777);
    for (int it = 0; it < 20000; it++) {
        const uint64_t w = r() >> (r() % 64);
        const uint32_t v = (uint32_t)w;
        // popcount loops (ant/ble/zigbee/dmr/iridium...)
        { int n = 0; uint32_t t = v; while (t) { t &= t - 1; n++; } CHECK(n == bits::popcount(v), "popc32 %08x", v); }
        { int n = 0; uint64_t t = w; while (t) { t &= t - 1; n++; } CHECK(n == bits::popcount(w), "popc64"); }
        // parity cascade (halow/psk/jt9/pager)
        { uint32_t x = v; x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; CHECK((int)(x & 1) == bits::parity(v), "parity32"); }
        // cubesat loop parity
        { int p = 0; uint32_t t = v; while (t) { p ^= 1; t &= t - 1; } CHECK(p == bits::parity(v), "parity loop"); }
        // parity8 over unsigned (meteor / hrpt parity7: folds the low 8 bits only)
        { unsigned x = v; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; CHECK((int)(x & 1) == bits::parity(v & 0xFFu), "parity8 unsigned"); }
        { uint8_t b = (uint8_t)v; b ^= b >> 4; b ^= b >> 2; b ^= b >> 1; CHECK((b & 1) == bits::parity((uint32_t)(uint8_t)v), "parity8 uint8"); }
        // m17 parity5
        { int x = (int)v; x &= 0x1F; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; CHECK((x & 1) == bits::parity((uint32_t)((int)v & 0x1F)), "parity5"); }
        // navtex popcount7
        { int n = 0; for (int i = 0; i < 7; i++) n += (v >> i) & 1; CHECK(n == bits::popcount(v & 0x7Fu), "popcount7"); }
        // gray / grayDecode (mesh_lora_code) / grayInverse int (atsc3_bicm, non-negative)
        { uint32_t x = v, g = v ^ (v >> 1); uint32_t d = g; for (uint32_t s = g >> 1; s; s >>= 1) d ^= s; CHECK(bits::gray(x) == g && bits::grayInverse(g) == d && d == x, "gray"); }
        { int g = (int)(v & 0x7FFFFFFF); int q = 0; for (int t = g; t; t >>= 1) q ^= t; CHECK((int)bits::grayInverse((uint32_t)g) == q, "grayInverse int"); }
        // reverse8 (ble) and wspr rev8(int)
        { uint8_t b = (uint8_t)v, o = 0; for (int i = 0; i < 8; i++) if (b >> i & 1) o |= (uint8_t)(0x80 >> i); CHECK(o == bits::reverse8(b), "rev8"); }
        { int j = (int)(v & 0xFF), o = 0; for (int b = 0; b < 8; b++) if (j & (1 << b)) o |= 1 << (7 - b); CHECK(o == bits::reverse8((uint8_t)j), "wspr rev8"); }

        // bit field readers / writers over a random 0/1 buffer
        uint8_t buf[200];
        for (auto& x : buf) x = (uint8_t)(r() & 1);
        const int pos = (int)(r() % 100), n = (int)(r() % 33);
        // MSB &1 (js8/cdr/ftx/sonde)
        { uint64_t a = 0; for (int i = 0; i < n; i++) a = (a << 1) | (buf[pos + i] & 1); CHECK(a == bits::getMsb(buf, (size_t)pos, n), "getMsb"); }
        // MSB raw + advance (isdbt/p25/tetrapol/t2fec)
        { int p = pos; uint64_t a = 0; for (int i = 0; i < n; i++) a = (a << 1) | buf[p++]; int p2 = pos; CHECK(a == bits::getMsbAdvRaw(buf, p2, n) && p == p2, "getMsbAdvRaw");
          CHECK(a == bits::getMsbRaw(buf, (size_t)pos, n), "getMsbRaw"); int p3 = pos; CHECK(a == bits::getMsbAdv(buf, p3, n) && p3 == p, "getMsbAdv"); }
        // LSB (halow)
        { uint32_t a = 0; for (int i = 0; i < n; i++) a |= (uint32_t)(buf[pos + i] & 1) << i; CHECK(a == (uint32_t)bits::getLsb(buf, (size_t)pos, n), "getLsb"); }
        // packed (adsb: first is 1-based; dab: 0-based)
        { uint8_t pk[40]; for (auto& x : pk) x = (uint8_t)r(); int first = 1 + (int)(r() % 100); uint32_t a = 0; for (int i = 0; i < n; i++) { const int b = first - 1 + i; a = (a << 1) | ((pk[b >> 3] >> (7 - (b & 7))) & 1u); }
          CHECK(a == (uint32_t)bits::getPackedMsb(pk, (size_t)(first - 1), n), "adsb getBits");
          uint32_t d = 0; for (int i = 0; i < n; i++) d = (d << 1) | ((pk[(pos + i) >> 3] >> (7 - ((pos + i) & 7))) & 1); CHECK(d == (uint32_t)bits::getPackedMsb(pk, (size_t)pos, n), "dab getBits"); }
        // writers: MSB indexed (js8/ftx/sonde), cdr with int v (neg), MSB advance (isdbt/p25/tetrapol), LSB (halow)
        { uint8_t a[64] = {}, b[64] = {}; const uint64_t val = r(); for (int i = 0; i < n; i++) a[3 + i] = (uint8_t)((val >> (n - 1 - i)) & 1); bits::putMsb(b, 3, val, n); CHECK(!memcmp(a, b, 64), "putMsb"); }
        { uint8_t a[64] = {}, b[64] = {}; const int val = (int)r(); for (int i = 0; i < n; i++) a[3 + i] = (uint8_t)((val >> (n - 1 - i)) & 1); bits::putMsb(b, 3, (uint64_t)(int64_t)val, n); CHECK(!memcmp(a, b, 64), "cdr putBits neg"); }
        { uint8_t a[64] = {}, b[64] = {}; const int val = (int)r(); int p = 5, q = 5; for (int i = n - 1; i >= 0; i--) a[p++] = (uint8_t)((val >> i) & 1); bits::putMsbAdv(b, q, (uint64_t)(uint32_t)val, n); CHECK(!memcmp(a, b, 64) && p == q, "tetrapol putBits neg"); }
        { uint8_t a[64] = {}, b[64] = {}; const uint32_t val = (uint32_t)r(); for (int i = 0; i < n; i++) a[2 + i] = (uint8_t)(val >> i & 1); bits::putLsb(b, 2, val, n); CHECK(!memcmp(a, b, 64), "putLsb"); }
        { std::vector<uint8_t> a, b; const uint64_t val = r(); for (int i = n - 1; i >= 0; i--) a.push_back((uint8_t)((val >> i) & 1)); bits::pushMsb(b, val, n); CHECK(a == b, "pushMsb"); }

        // bytes <-> bits
        { uint8_t by[8]; for (auto& x : by) x = (uint8_t)r(); uint8_t a[64], b[64];
          for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) a[i * 8 + k] = (by[i] >> (7 - k)) & 1; bits::bytesToBitsMsb(by, 8, b); CHECK(!memcmp(a, b, 64), "bytesToBitsMsb");
          for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) a[i * 8 + k] = (by[i] >> k) & 1; bits::bytesToBitsLsb(by, 8, b); CHECK(!memcmp(a, b, 64), "bytesToBitsLsb");
          uint8_t o1[8], o2[8]; for (int i = 0; i < 8; i++) { o1[i] = 0; for (int k = 0; k < 8; k++) o1[i] |= (uint8_t)((buf[i * 8 + k] & 1) << k); } bits::bitsToBytesLsb(buf, 8, o2); CHECK(!memcmp(o1, o2, 8), "bitsToBytesLsb");
          for (int i = 0; i < 8; i++) { int v2 = 0; for (int k = 0; k < 8; k++) v2 = (v2 << 1) | buf[i * 8 + k]; o1[i] = (uint8_t)v2; } bits::bitsToBytesMsb(buf, 8, o2); CHECK(!memcmp(o1, o2, 8), "bitsToBytesMsb"); }

        // NRZI / Manchester
        { std::vector<uint8_t> d(r() % 60); for (auto& x : d) x = (uint8_t)(r() & 1); const int lv0 = (int)(r() & 7);
          std::vector<uint8_t> e(d.size()); int lv = lv0 & 1; for (size_t i = 0; i < d.size(); i++) { if (!d[i]) lv ^= 1; e[i] = (uint8_t)lv; }
          CHECK(e == linecode::nrziEncode(d, lv0), "nrziEncode");
          std::vector<uint8_t> dd(d.size()); int p = lv0 & 1; for (size_t i = 0; i < d.size(); i++) { dd[i] = (d[i] == p) ? 1 : 0; p = d[i]; }
          CHECK(dd == linecode::nrziDecode(d, lv0), "nrziDecode");
          std::vector<uint8_t> by(r() % 20); for (auto& x : by) x = (uint8_t)r(); std::vector<uint8_t> c;
          for (size_t i = 0; i < by.size(); i++) for (int k = 7; k >= 0; k--) { const int b = (by[i] >> k) & 1; c.push_back(b ? 0 : 1); c.push_back(b ? 1 : 0); }
          CHECK(c == linecode::manchesterEncode(by.data(), by.size()), "manchester"); }
    }
    printf(fails ? "test_common_bits: %d FAILED\n" : "test_common_bits: OK\n", fails);
    return fails ? 1 : 0;
}
