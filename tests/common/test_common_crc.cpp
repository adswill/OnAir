// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Compares every shared CRC in dect2/common/crc.h against the ORIGINAL per-protocol implementations
// (embedded here as reference lambdas) on >= 10000 random inputs each, plus the catalogue check vectors.
#include "dect2/common/crc.h"
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <vector>
using namespace dect2;
using Bytes = std::vector<uint8_t>;
#include "test_util.h"

struct Variant { const char* name; std::function<uint32_t(const Bytes&)> ref, now; bool bitInput; };

static std::vector<Variant> variants() {
    std::vector<Variant> v;
    auto add = [&](const char* n, auto ref, auto now, bool bitIn = false) { v.push_back({n, ref, now, bitIn}); };
    // ant / flarm / iridium: CCITT-FALSE
    add("ant crc16", [](const Bytes& d) { uint16_t c = 0xFFFF; for (size_t i = 0; i < d.size(); i++) { c ^= (uint16_t)(d[i] << 8); for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); } return (uint32_t)c; },
        [](const Bytes& d) { return (uint32_t)crc::crc16CcittFalse(d.data(), d.size()); });
    // ccsds with init
    add("ccsds crc16 init", [](const Bytes& d) { uint16_t crc = (uint16_t)(d.size() * 2654435761u); static const struct T { uint16_t t[256]; T() { for (int i = 0; i < 256; i++) { uint16_t c = (uint16_t)(i << 8); for (int j = 0; j < 8; j++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1); t[i] = c; } } } tab; for (size_t i = 0; i < d.size(); i++) crc = (uint16_t)((crc << 8) ^ tab.t[((crc >> 8) ^ d[i]) & 0xFF]); return (uint32_t)crc; },
        [](const Bytes& d) { return (uint32_t)crc::crc16CcittFalse(d.data(), d.size(), (uint16_t)(d.size() * 2654435761u)); });
    // wifi crc32 / wisun / png-style
    add("wifi crc32", [](const Bytes& d) { uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < d.size(); i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); } return ~c; },
        [](const Bytes& d) { return crc::crc32(d.data(), d.size()); });
    add("drm/dab-pad crc32 chained", [](const Bytes& d) { uint32_t crc = (uint32_t)d.size() * 2246822519u; crc = ~crc; for (size_t i = 0; i < d.size(); i++) { crc ^= d[i]; for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1; } return ~crc; },
        [](const Bytes& d) { return crc::crc32Update((uint32_t)d.size() * 2246822519u, d.data(), d.size()); });
    add("hdr crc32 raw", [](const Bytes& d) { uint32_t c = (uint32_t)d.size() * 3266489917u; for (size_t i = 0; i < d.size(); i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; } return c; },
        [](const Bytes& d) { return crc::lsbBytes<0xEDB88320u>(d.data(), d.size(), (uint32_t)d.size() * 3266489917u); });
    // wifi plcp crc16 over bits
    add("wifi crc16 bits", [](const Bytes& d) { uint16_t r = 0xFFFF; for (size_t i = 0; i < d.size(); i++) { const int fb = ((r >> 15) & 1) ^ (d[i] & 1); r = (uint16_t)(r << 1); if (fb) r ^= 0x1021; } return (uint32_t)(uint16_t)~r; },
        [](const Bytes& d) { return crc::bits(d.data(), d.size(), 16, 0x1021, 0xFFFF, 0xFFFF); }, true);
    // zigbee = kermit
    add("zigbee crc16", [](const Bytes& d) { uint16_t c = 0; for (size_t i = 0; i < d.size(); i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); } return (uint32_t)c; },
        [](const Bytes& d) { return (uint32_t)crc::crc16Kermit(d.data(), d.size()); });
    add("dji duml", [](const Bytes& d) { uint16_t crc = 0x3692; for (size_t i = 0; i < d.size(); i++) { crc ^= d[i]; for (int b = 0; b < 8; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1); } return (uint32_t)crc; },
        [](const Bytes& d) { return (uint32_t)crc::crc16Kermit(d.data(), d.size(), 0x3692); });
    // mac: bytes LSB first into MSB register
    add("mac crc16", [](const Bytes& d) { uint32_t r = 0; for (size_t i = 0; i < d.size(); i++) for (int b = 0; b < 8; b++) { const int bit = (d[i] >> b) & 1; const int fb = ((r >> 15) & 1) ^ bit; r = (r << 1) & 0xFFFF; if (fb) r ^= 0x1021; } return (uint32_t)(uint16_t)r; },
        [](const Bytes& d) { return crc::generic(d.data(), d.size(), 16, 0x1021, 0, true, false, 0); });
    // tetra bits
    add("tetra crc16 bits", [](const Bytes& d) { uint16_t crc = 0xFFFF; for (size_t i = 0; i < d.size(); i++) { crc ^= (uint16_t)((d[i] & 1) << 15); crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1); } return (uint32_t)(uint16_t)~crc; },
        [](const Bytes& d) { return crc::bits(d.data(), d.size(), 16, 0x1021, 0xFFFF, 0xFFFF); }, true);
    // dab fec / gen_coding (genibus)
    add("dab_fec crc16", [](const Bytes& d) { uint16_t crc = 0xFFFF; for (size_t i = 0; i < d.size(); i++) { crc ^= (uint16_t)(d[i] << 8); for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1); } return (uint32_t)(uint16_t)~crc; },
        [](const Bytes& d) { return (uint32_t)crc::crc16Genibus(d.data(), d.size()); });
    add("dab_gen_coding crc16", [](const Bytes& d) { unsigned reg = 0xFFFF; for (size_t i = 0; i < d.size(); i++) for (int b = 7; b >= 0; b--) { const unsigned in = (d[i] >> b) & 1u, fb = ((reg >> 15) & 1u) ^ in; reg = (reg << 1) & 0xFFFFu; if (fb) reg ^= 0x1021u; } return (uint32_t)(uint16_t)(~reg & 0xFFFFu); },
        [](const Bytes& d) { return (uint32_t)crc::crc16Genibus(d.data(), d.size()); });
    add("drm crc16Bytes", [](const Bytes& d) { const uint32_t mask = 0xFFFF; uint32_t reg = mask; for (size_t i = 0; i < d.size(); i++) for (int j = 0; j < 8; j++) { const uint32_t fb = ((reg >> 15) & 1u) ^ ((d[i] >> (7 - j)) & 1u); reg = (reg << 1) & mask; if (fb) reg ^= 0x1021; } return reg ^ mask; },
        [](const Bytes& d) { return (uint32_t)crc::crc16Genibus(d.data(), d.size()); });
    add("drm crc8Bytes", [](const Bytes& d) { const uint32_t mask = 0xFF; uint32_t reg = mask; for (size_t i = 0; i < d.size(); i++) for (int j = 0; j < 8; j++) { const uint32_t fb = ((reg >> 7) & 1u) ^ ((d[i] >> (7 - j)) & 1u); reg = (reg << 1) & mask; if (fb) reg ^= 0x1D; } return reg ^ mask; },
        [](const Bytes& d) { return crc::generic(d.data(), d.size(), 8, 0x1D, 0xFF, false, false, 0xFF); });
    add("drm crcBits 16/8/32", [](const Bytes& d) { uint32_t acc = 0; for (int w : {8, 16, 32}) { const uint32_t poly = w == 8 ? 0x1D : w == 16 ? 0x1021 : 0x04C11DB7u; const uint32_t mask = (w == 32) ? 0xFFFFFFFFu : ((1u << w) - 1); uint32_t reg = mask; for (size_t i = 0; i < d.size(); i++) { const uint32_t fb = ((reg >> (w - 1)) & 1u) ^ (d[i] & 1u); reg = (reg << 1) & mask; if (fb) reg ^= poly; } acc = acc * 31u + (reg ^ mask); } return acc; },
        [](const Bytes& d) { uint32_t acc = 0; for (int w : {8, 16, 32}) { const uint32_t poly = w == 8 ? 0x1D : w == 16 ? 0x1021 : 0x04C11DB7u; const uint32_t mask = (w == 32) ? 0xFFFFFFFFu : ((1u << w) - 1); acc = acc * 31u + crc::bits(d.data(), d.size(), w, poly, mask, mask); } return acc; }, true);
    // m17
    add("m17 crc16", [](const Bytes& d) { uint16_t c = 0xFFFF; for (size_t i = 0; i < d.size(); i++) { c ^= (uint16_t)d[i] << 8; for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x5935) : (uint16_t)(c << 1); } return (uint32_t)c; },
        [](const Bytes& d) { return crc::msbBytes<16, 0x5935>(d.data(), d.size(), 0xFFFF); });
    // flac footer
    add("flac crc16", [](const Bytes& d) { uint16_t c = 0; for (size_t i = 0; i < d.size(); i++) { c ^= (uint16_t)(d[i] << 8); for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1); } return (uint32_t)c; },
        [](const Bytes& d) { return (uint32_t)crc::crc16Buypass(d.data(), d.size()); });
    // cdr crc32 (MSB-first, inverted)
    add("cdr crc32", [](const Bytes& d) { uint32_t r = 0xFFFFFFFFu; for (size_t i = 0; i < d.size(); i++) for (int b = 7; b >= 0; b--) { const uint32_t in = (d[i] >> b) & 1u, top = r >> 31; r <<= 1; if (in ^ top) r ^= 0x04C11DB7u; } return ~r; },
        [](const Bytes& d) { return ~crc::crc32Mpeg2(d.data(), d.size()); });
    // t2 crc32Bits
    add("t2 crc32Bits", [](const Bytes& d) { uint32_t crc = 0xffffffffu; for (size_t i = 0; i < d.size(); i++) { uint32_t b = d[i] ^ ((crc >> 31) & 1); crc <<= 1; if (b) crc ^= 0x04C11DB7u; } return crc; },
        [](const Bytes& d) { return crc::bits(d.data(), d.size(), 32, 0x04C11DB7u, 0xFFFFFFFFu); }, true);
    // dmr
    add("dmr crcCcitt", [](const Bytes& d) { uint16_t c = 0; for (size_t i = 0; i < d.size(); i++) { c ^= (uint16_t)(d[i] << 8); for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); } return (uint32_t)(uint16_t)~c; },
        [](const Bytes& d) { return (uint32_t)(uint16_t)~crc::crc16Xmodem(d.data(), d.size()); });
    add("dmr crc32Msg", [](const Bytes& d) { const uint8_t* p = d.data(); size_t n = d.size(); uint32_t c = 0; auto feed = [&](uint8_t b) { c ^= (uint32_t)b << 24; for (int i = 0; i < 8; i++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1); }; for (size_t i = 0; i + 1 < n; i += 2) { feed(p[i + 1]); feed(p[i]); } if (n & 1) feed(p[n - 1]); return c; },
        [](const Bytes& d) { const uint8_t* p = d.data(); size_t n = d.size(); uint32_t c = 0; for (size_t i = 0; i + 1 < n; i += 2) { c = crc::msbUpdate<32, 0x04C11DB7u>(c, p[i + 1]); c = crc::msbUpdate<32, 0x04C11DB7u>(c, p[i]); } if (n & 1) c = crc::msbUpdate<32, 0x04C11DB7u>(c, p[n - 1]); return c; });
    // p25 bits
    add("p25 crcCcitt bits", [](const Bytes& d) { uint16_t c = 0; for (size_t i = 0; i < d.size(); i++) { const bool fb = ((c >> 15) & 1) ^ (d[i] & 1); c = (uint16_t)(c << 1); if (fb) c ^= 0x1021; } return (uint32_t)(uint16_t)~c; },
        [](const Bytes& d) { return crc::bits(d.data(), d.size(), 16, 0x1021, 0, 0xFFFF); }, true);
    // nxdn crcGen widths 6/12/16
    add("nxdn crcGen", [](const Bytes& d) { uint32_t acc = 0; struct S { int w; uint32_t poly, init; } ss[3] = {{6, 0x27, 0x3F}, {12, 0x80F, 0xFFF}, {16, 0x1021, 0xFFFF}}; for (auto s : ss) { const uint32_t mask = (1u << s.w) - 1; uint32_t c = s.init & mask; for (size_t i = 0; i < d.size(); i++) { const uint32_t a = (d[i] & 1) ^ (c >> (s.w - 1)); c = (c << 1) & mask; if (a) c ^= s.poly; } acc = acc * 31u + c; } return acc; },
        [](const Bytes& d) { uint32_t acc = 0; struct S { int w; uint32_t poly, init; } ss[3] = {{6, 0x27, 0x3F}, {12, 0x80F, 0xFFF}, {16, 0x1021, 0xFFFF}}; for (auto s : ss) acc = acc * 31u + crc::bits(d.data(), d.size(), s.w, s.poly, s.init); return acc; }, true);
    // ais x25
    add("ais x25", [](const Bytes& d) { uint16_t crc = 0xFFFF; for (uint8_t by : d) { crc ^= by; for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1); } return (uint32_t)(uint16_t)~crc; },
        [](const Bytes& d) { return (uint32_t)crc::crc16X25(d.data(), d.size()); });
    return v;
}

int main() {
    // catalogue check vectors for "123456789"
    const uint8_t* s = (const uint8_t*)"123456789";
    CHECK(crc::crc16CcittFalse(s, 9) == 0x29B1, "CCITT-FALSE");
    CHECK(crc::crc16X25(s, 9) == 0x906E, "X25");
    CHECK(crc::crc16Xmodem(s, 9) == 0x31C3, "XMODEM");
    CHECK(crc::crc32(s, 9) == 0xCBF43926u, "CRC-32");
    CHECK(crc::crc32Mpeg2(s, 9) == 0x0376E6E7u, "CRC-32/MPEG-2");
    CHECK(crc::crc16Kermit(s, 9) == 0x2189, "KERMIT");
    CHECK(crc::crc16Genibus(s, 9) == 0xD64E, "GENIBUS");
    CHECK(crc::crc16Buypass(s, 9) == 0xFEE8, "BUYPASS");
    CHECK(crc::generic(s, 9, 16, 0x1021, 0xFFFF, false, false, 0) == 0x29B1, "generic CCITT-FALSE");
    CHECK(crc::generic(s, 9, 32, 0x04C11DB7u, 0xFFFFFFFFu, true, true, 0xFFFFFFFFu) == 0xCBF43926u, "generic CRC-32");
    std::mt19937 rng(12345);
    for (auto& v : variants()) {
        for (int it = 0; it < 12000; it++) {
            Bytes d(it % 5 == 0 ? it % 3 : rng() % 80);   // empty/tiny edge cases included
            for (auto& x : d) x = v.bitInput ? (uint8_t)(rng() & 1) : (uint8_t)rng();
            const uint32_t a = v.ref(d), b = v.now(d);
            if (a != b) { CHECK(false, "%s mismatch on input %d: ref %08x new %08x", v.name, it, a, b); break; }
        }
    }
    printf(fails ? "test_common_crc: %d FAILED\n" : "test_common_crc: OK\n", fails);
    return fails ? 1 : 0;
}
