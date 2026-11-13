// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB+ test transmitter: a slide show picture in the PAD (ETSI TS 102 563 clause 5.2, EN 300 401 clause 7.4, EN 301 234). Written from the standards,
// without the receiver's code (dab_pad.cpp).
#include "dect2/dab/dab_gen.h"
#include <algorithm>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/endian.h"

namespace dect2 {
namespace dabgen {

namespace {
constexpr int kW = 80, kH = 60;
constexpr uint16_t kTransportId = 0x1234;
constexpr int kSub = 16;              // bytes of a data subfield (content indicator length index 4)

uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) { return dect2::crc::crc32Update(crc, d, n); }
void put32(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putBe32(v, x); }
void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put32(png, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    put32(png, crc32(td.data(), td.size()));
}

// Palette entry i: bar i / 32 of eight colours, darkening with i % 32. 254 = black, 255 = white.
void palette(int i, uint8_t rgb[3]) {
    static const uint8_t base[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {40, 40, 40}};
    if (i == 254) { rgb[0] = rgb[1] = rgb[2] = 0; return; }
    if (i == 255) { rgb[0] = rgb[1] = rgb[2] = 255; return; }
    const int k = 255 - (i % 32) * 6;
    for (int c = 0; c < 3; c++) rgb[c] = (uint8_t)(base[i / 32][c] * k / 255);
}
int pixel(int x, int y) {
    if (x == 0 || y == 0 || x == kW - 1 || y == kH - 1) return 255;
    if (x * kH / kW == y || (kW - 1 - x) * kH / kW == y) return 254;             // the two diagonals
    return (x * 8 / kW) * 32 + y * 31 / (kH - 1);
}

std::vector<uint8_t> be16crc(const uint8_t* d, size_t n) { const uint16_t c = crc16(d, (int)n); return {(uint8_t)(c >> 8), (uint8_t)c}; }

// One MSC data group of the MOT (EN 301 234 header mode): CRC, segment field, user access field with the transport id, one segment
std::vector<uint8_t> dataGroup(int type, const std::vector<uint8_t>& seg) {
    std::vector<uint8_t> g = {(uint8_t)(0x40 | 0x20 | 0x10 | type), 0x00, 0x80, 0x00, 0x12, (uint8_t)(kTransportId >> 8), (uint8_t)kTransportId};
    g.push_back((uint8_t)(seg.size() >> 8)); g.push_back((uint8_t)seg.size());      // segmentation header: repetition count 0, segment size
    g.insert(g.end(), seg.begin(), seg.end());
    const std::vector<uint8_t> c = be16crc(g.data(), g.size());
    g.insert(g.end(), c.begin(), c.end());
    return g;
}
} // namespace

void slideImage(int& w, int& h, std::vector<uint8_t>& rgba) {
    w = kW; h = kH;
    rgba.assign((size_t)kW * kH * 4, 255);
    for (int y = 0; y < kH; y++) for (int x = 0; x < kW; x++) { uint8_t c[3]; palette(pixel(x, y), c); std::memcpy(&rgba[((size_t)y * kW + x) * 4], c, 3); }
}

std::vector<uint8_t> slidePng() {
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    put32(ihdr, kW); put32(ihdr, kH);
    for (uint8_t b : {8, 3, 0, 0, 0}) ihdr.push_back(b);
    chunk(png, "IHDR", ihdr);
    std::vector<uint8_t> plte;
    for (int i = 0; i < 256; i++) { uint8_t c[3]; palette(i, c); plte.insert(plte.end(), c, c + 3); }
    chunk(png, "PLTE", plte);
    std::vector<uint8_t> raw;                                   // filter type 0 and the palette indices of every row
    for (int y = 0; y < kH; y++) { raw.push_back(0); for (int x = 0; x < kW; x++) raw.push_back((uint8_t)pixel(x, y)); }
    std::vector<uint8_t> z = {0x78, 0x01, 0x01, (uint8_t)(raw.size() & 0xFF), (uint8_t)(raw.size() >> 8), (uint8_t)(~raw.size() & 0xFF), (uint8_t)((~raw.size() >> 8) & 0xFF)};
    z.insert(z.end(), raw.begin(), raw.end());                  // one stored deflate block
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    put32(z, b << 16 | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

std::vector<std::vector<uint8_t>> slideDse(size_t nAu) {
    // the MOT object: header (core + ContentName), body = the PNG
    const std::vector<uint8_t> body = slidePng();
    std::vector<uint8_t> head(7, 0);
    const char* name = "OnAir test slide";
    const size_t nl = std::strlen(name);
    const size_t hsize = 7 + 2 + 1 + nl;                        // core, parameter id + length, character set, text
    head[0] = (uint8_t)(body.size() >> 20); head[1] = (uint8_t)(body.size() >> 12); head[2] = (uint8_t)(body.size() >> 4);
    head[3] = (uint8_t)((body.size() << 4) | (hsize >> 9)); head[4] = (uint8_t)(hsize >> 1);
    head[5] = (uint8_t)(((hsize & 1) << 7) | (2 << 1) | 0);     // content type 2 = image, subtype 3 = PNG (9 bits: bit 0 here, then the next byte)
    head[6] = 3;
    head.push_back((3 << 6) | 0x0C); head.push_back((uint8_t)(nl + 1)); head.push_back(0);
    head.insert(head.end(), name, name + nl);
    // the subfields in transmission order: for each data group the length indicator (type 1, 4 bytes), then 16 byte pieces of type 12 and 13
    struct Item { int type; std::vector<uint8_t> d; };
    std::vector<Item> items;
    for (const auto& g : {dataGroup(3, head), dataGroup(4, body)}) {
        std::vector<uint8_t> l = {(uint8_t)((g.size() >> 8) & 0x3F), (uint8_t)g.size()};
        const std::vector<uint8_t> c = be16crc(l.data(), 2);
        l.insert(l.end(), c.begin(), c.end());
        items.push_back({1, l});
        for (size_t p = 0; p < g.size(); p += kSub) {
            Item it{p == 0 ? 12 : 13, std::vector<uint8_t>(kSub, 0)};
            std::memcpy(it.d.data(), &g[p], std::min((size_t)kSub, g.size() - p));
            items.push_back(it);
        }
    }
    std::vector<std::vector<uint8_t>> out(nAu);
    for (size_t j = 0; j < nAu && j < items.size(); j++) {
        const Item& it = items[j];
        std::vector<uint8_t> xp = {(uint8_t)(((it.type == 1 ? 0 : 4) << 5) | it.type), 0x00};    // content indicator (length index 0 = 4 bytes, 4 = 16 bytes), end marker
        xp.insert(xp.end(), it.d.begin(), it.d.end());
        std::vector<uint8_t> d(xp.rbegin(), xp.rend());                                          // the X-PAD sits backwards in front of the F-PAD
        d.push_back(0x20);                                                                       // F-PAD: type 0, variable size X-PAD
        d.push_back(0x02);                                                                       // content indicator flag
        out[j] = {0x80, (uint8_t)d.size()};                                                      // data stream element, instance 0, count
        out[j].insert(out[j].end(), d.begin(), d.end());
    }
    // the rest of the loop: the dynamic label, two segments (16 and 8 characters) sent over and over, each in one X-PAD with a type 2 indicator
    const char* label = "OnAir Melody - test tune";
    const size_t ll = std::strlen(label);
    for (size_t j = items.size(); j < nAu; j++) {
        const size_t seg = (j - items.size()) % 2, from = seg * 16, n = std::min<size_t>(16, ll - from);
        std::vector<uint8_t> s = {(uint8_t)((seg == 0 ? 0x40 : 0x20) | (n - 1)), (uint8_t)(seg == 0 ? 0x00 : seg << 4)};   // first / last, length - 1; charset 0 (EBU Latin) or the number
        s.insert(s.end(), label + from, label + from + n);
        const std::vector<uint8_t> c = be16crc(s.data(), s.size());
        s.insert(s.end(), c.begin(), c.end());
        const int idx = s.size() <= 24 && s.size() > 16 ? 5 : 3;                                  // subfield of 24 or 12 bytes
        s.resize(idx == 5 ? 24 : 12, 0);
        std::vector<uint8_t> xp = {(uint8_t)((idx << 5) | 2), 0x00};
        xp.insert(xp.end(), s.begin(), s.end());
        std::vector<uint8_t> d(xp.rbegin(), xp.rend());
        d.push_back(0x20); d.push_back(0x02);
        out[j] = {0x80, (uint8_t)d.size()};
        out[j].insert(out[j].end(), d.begin(), d.end());
    }
    return out;
}

} // namespace dabgen
} // namespace dect2
