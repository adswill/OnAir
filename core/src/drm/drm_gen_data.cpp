// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM test signal: the content of a packet mode data service, MOT objects (ETSI EN 301 234, TS 101 499) in MSC data groups (EN 300 401 clause 5.3.3.1).
// Written from the standards, not from the receiver's MotDecoder.
#include "dect2/drm/drm_gen.h"
#include "dect2/dab/dab.h"
#include <algorithm>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/endian.h"

namespace dect2 {

namespace {
uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) { return dect2::crc::crc32Update(crc, d, n); }
void put32(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putBe32(v, x); }
void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put32(png, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    put32(png, crc32(td.data(), td.size()));
}
void pixelRgb(int w, int h, int seed, int x, int y, uint8_t rgb[3]) {
    if (x == 0 || y == 0 || x == w - 1 || y == h - 1) { rgb[0] = rgb[1] = rgb[2] = 255; return; }
    rgb[0] = (uint8_t)((x * 255 / std::max(1, w - 1) + seed * 40) & 255);
    rgb[1] = (uint8_t)(y * 255 / std::max(1, h - 1));
    rgb[2] = (uint8_t)((seed * 83 + (x + y) * 3) & 255);
}

// a parameter of the header extension: 2 bit length class 3 (a length indicator follows), parameter id
void putParam(std::vector<uint8_t>& h, int id, const std::vector<uint8_t>& d) {
    h.push_back((uint8_t)(0xC0 | id));
    if (d.size() < 128) h.push_back((uint8_t)d.size());
    else { h.push_back((uint8_t)(0x80 | (d.size() >> 8))); h.push_back((uint8_t)d.size()); }
    h.insert(h.end(), d.begin(), d.end());
}
std::vector<uint8_t> withCharset(const std::string& s) {   // character set 15 (UTF-8) in the high nibble
    std::vector<uint8_t> v = {0xF0};
    v.insert(v.end(), s.begin(), s.end());
    return v;
}

class MotSource : public DrmDataSource {
public:
    MotSource(const std::vector<DrmMotObject>& objs, int segSize, bool dir, uint16_t firstTid) : objs_(objs), seg_(std::max(8, segSize)), dir_(dir), tid0_(firstTid) { build(); }
    std::vector<uint8_t> nextUnit() override {
        if (units_.empty()) return {};
        const std::vector<uint8_t>& u = units_[pos_];
        pos_ = (pos_ + 1) % units_.size();
        std::vector<uint8_t> g = u;
        // the continuity index of the data groups of a type counts up in the 4 bit field of the second byte (high nibble)
        g[1] = (uint8_t)((cont_ & 15) << 4);
        cont_++;
        const uint16_t c = dab::crc16(g.data(), (int)g.size() - 2);
        g[g.size() - 2] = (uint8_t)(c >> 8); g[g.size() - 1] = (uint8_t)c;
        return g;
    }

private:
    void segments(int type, uint16_t tid, const std::vector<uint8_t>& data) {
        const size_t n = std::max<size_t>(1, (data.size() + (size_t)seg_ - 1) / (size_t)seg_);
        for (size_t i = 0; i < n; i++) {
            const size_t at = i * (size_t)seg_, len = std::min((size_t)seg_, data.size() - std::min(at, data.size()));
            units_.push_back(drmGenMotGroup(type, tid, (int)i, i + 1 == n, 0, data.data() + at, len, 0));
        }
    }
    void build() {
        std::vector<std::vector<uint8_t>> heads;
        for (const DrmMotObject& o : objs_) heads.push_back(drmGenMotHeader(o));
        if (dir_) {
            std::vector<uint8_t> d;
            size_t total = 13;
            for (const auto& h : heads) total += 2 + h.size();
            put32(d, (uint32_t)total & 0x3FFFFFFF);
            d.push_back((uint8_t)(objs_.size() >> 8)); d.push_back((uint8_t)objs_.size());
            d.push_back(0); d.push_back(0); d.push_back(0);                      // carousel period: not given
            d.push_back((uint8_t)(seg_ >> 8)); d.push_back((uint8_t)seg_);       // rfa and the segment size
            d.push_back(0); d.push_back(0);                                      // no directory extension
            for (size_t i = 0; i < heads.size(); i++) {
                const uint16_t tid = (uint16_t)(tid0_ + i);
                d.push_back((uint8_t)(tid >> 8)); d.push_back((uint8_t)tid);
                d.insert(d.end(), heads[i].begin(), heads[i].end());
            }
            segments(6, (uint16_t)(tid0_ + 0x100), d);
            for (size_t i = 0; i < objs_.size(); i++) segments(4, (uint16_t)(tid0_ + i), objs_[i].body);
        } else {
            for (size_t i = 0; i < objs_.size(); i++) {
                segments(3, (uint16_t)(tid0_ + i), heads[i]);
                segments(4, (uint16_t)(tid0_ + i), objs_[i].body);
            }
        }
    }
    std::vector<DrmMotObject> objs_;
    int seg_;
    bool dir_;
    uint16_t tid0_;
    std::vector<std::vector<uint8_t>> units_;
    size_t pos_ = 0;
    uint32_t cont_ = 0;
};
} // namespace

std::vector<uint8_t> drmGenPng(int w, int h, int seed) {
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w); put32(ihdr, (uint32_t)h);
    for (uint8_t b : {8, 2, 0, 0, 0}) ihdr.push_back(b);                       // 8 bit RGB
    chunk(png, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) { raw.push_back(0); for (int x = 0; x < w; x++) { uint8_t c[3]; pixelRgb(w, h, seed, x, y, c); raw.insert(raw.end(), c, c + 3); } }
    std::vector<uint8_t> z = {0x78, 0x01, 0x01, (uint8_t)(raw.size() & 0xFF), (uint8_t)(raw.size() >> 8), (uint8_t)(~raw.size() & 0xFF), (uint8_t)((~raw.size() >> 8) & 0xFF)};
    z.insert(z.end(), raw.begin(), raw.end());                                  // one stored deflate block (the picture is small)
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    put32(z, b << 16 | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

void drmGenPngPixels(int w, int h, int seed, std::vector<uint8_t>& rgba) {
    rgba.assign((size_t)w * (size_t)h * 4, 255);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) pixelRgb(w, h, seed, x, y, &rgba[((size_t)y * (size_t)w + (size_t)x) * 4]);
}

std::vector<uint8_t> drmGenMotHeader(const DrmMotObject& o) {
    std::vector<uint8_t> ext;
    {   // ContentName: character set 15 (UTF-8) in the high nibble, then the text
        putParam(ext, 0x0C, withCharset(o.name));
    }
    if (!o.caption.empty()) putParam(ext, 0x17, withCharset(o.caption));
    if (o.hasTrigger) {
        // TriggerTime: validity flag, MJD (17 bits), LSI, confidence, UTC flag 0, hours (5), minutes (6); validity 0 = now
        uint32_t v = 0;
        if (o.trigMjd) v = (1u << 31) | ((uint32_t)o.trigMjd << 14) | ((uint32_t)o.trigHour << 6) | (uint32_t)o.trigMinute;
        ext.push_back((uint8_t)(0x80 | 0x05));                                  // length class 2: 4 bytes
        put32(ext, v);
    }
    if (o.categoryId >= 0) putParam(ext, 0x25, {(uint8_t)o.categoryId, (uint8_t)std::max(0, o.slideId)});
    if (!o.category.empty()) putParam(ext, 0x26, std::vector<uint8_t>(o.category.begin(), o.category.end()));
    if (!o.clickUrl.empty()) putParam(ext, 0x27, std::vector<uint8_t>(o.clickUrl.begin(), o.clickUrl.end()));
    const size_t hsize = 7 + ext.size();
    std::vector<uint8_t> h(7, 0);
    const size_t bs = o.body.size();
    h[0] = (uint8_t)(bs >> 20); h[1] = (uint8_t)(bs >> 12); h[2] = (uint8_t)(bs >> 4);
    h[3] = (uint8_t)(((bs & 15) << 4) | (hsize >> 9)); h[4] = (uint8_t)(hsize >> 1);
    h[5] = (uint8_t)(((hsize & 1) << 7) | ((o.contentType & 0x3F) << 1) | ((o.subtype >> 8) & 1));
    h[6] = (uint8_t)o.subtype;
    h.insert(h.end(), ext.begin(), ext.end());
    return h;
}

std::vector<uint8_t> drmGenMotGroup(int type, uint16_t tid, int segNo, bool last, int repetition, const uint8_t* seg, size_t n, int continuity) {
    // header: extension flag 0, CRC flag 1, segment flag 1, user access flag 1, data group type; continuity index and repetition index
    std::vector<uint8_t> g = {(uint8_t)(0x40 | 0x20 | 0x10 | type), (uint8_t)(((continuity & 15) << 4) | (repetition & 15))};
    g.push_back((uint8_t)((last ? 0x80 : 0) | ((segNo >> 8) & 0x7F))); g.push_back((uint8_t)segNo);   // segment field
    g.push_back(0x12); g.push_back((uint8_t)(tid >> 8)); g.push_back((uint8_t)tid);                  // user access field: transport id, length 2
    g.push_back((uint8_t)(n >> 8) & 0x1F); g.push_back((uint8_t)n);                                  // MOT segmentation header: repetition count 0, segment size
    g.insert(g.end(), seg, seg + n);
    const uint16_t c = dab::crc16(g.data(), (int)g.size());
    g.push_back((uint8_t)(c >> 8)); g.push_back((uint8_t)c);
    return g;
}

std::shared_ptr<DrmDataSource> makeDrmMotSource(const std::vector<DrmMotObject>& objects, int segSize, bool directoryMode, uint16_t firstTid) {
    return std::make_shared<MotSource>(objects, segSize, directoryMode, firstTid);
}

} // namespace dect2
