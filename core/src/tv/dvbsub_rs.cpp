// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The DVB subtitle layer of dvbsub.h on top of the Rust crate rust/onair_dvbsi: PES header, segment walk, the five segment parsers, the colour
// tables, the run-length pixel decoder, region blending and the PTS arithmetic. The decoder class with its page, region, CLUT and object state stays in
// dvbsub.cpp.
#include "dect2/tv/dvbsub.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/dvbsi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[10];
        onair_dvbsi_struct_sizes(s);
        dect2::rsAbiCheck("onair_dvbsi", s, {sizeof(OnairDvbsiTtxUnit), sizeof(OnairDvbsiTtxCell), sizeof(OnairDvbsiSubPesInfo), sizeof(OnairDvbsiSubSegment), sizeof(OnairDvbsiSubPageInfo), sizeof(OnairDvbsiSubPageRegion), sizeof(OnairDvbsiSubRegionInfo), sizeof(OnairDvbsiSubRegionObject), sizeof(OnairDvbsiSubClutEntry), sizeof(OnairDvbsiSubObjectInfo)});
    }
} abiCheck;
inline size_t L(int n) { return n < 0 ? 0 : (size_t)n; }   // negative lengths are 0 bytes
}

double ptsDelta(double a, double b) { return onair_dvbsi_sub_pts_delta(a, b); }

void dvbSubComposite(const DvbSubDisplay& d, std::vector<uint8_t>& out) {
    out.assign((size_t)d.dispW * d.dispH * 4, 0);
    for (const auto& r : d.regions)
        if (r.rgba.size() >= (size_t)r.w * r.h * 4) onair_dvbsi_sub_blend_region(out.data(), d.dispW, d.dispH, r.x, r.y, r.w, r.h, r.rgba.data());
}

bool dvbSubPesInfo(const uint8_t* pes, size_t n, DvbSubPesInfo& out) {
    OnairDvbsiSubPesInfo c;
    if (!onair_dvbsi_sub_pes_info(pes, n, &c)) return false;
    out.payloadOff = c.payload_off; out.hasPts = c.has_pts != 0; out.pts = c.pts;
    return true;
}

std::vector<DvbSubSegment> dvbSubSegments(const uint8_t* d, size_t n) {
    std::vector<OnairDvbsiSubSegment> v(n / 6 + 1);
    const size_t c = onair_dvbsi_sub_segments(d, n, v.data(), v.size());
    std::vector<DvbSubSegment> out;
    for (size_t i = 0; i < c && i < v.size(); i++) out.push_back(DvbSubSegment{v[i].type, v[i].page, v[i].off, v[i].len});
    return out;
}

bool dvbSubParsePage(const uint8_t* d, int n, DvbSubPageInfo& out) {
    out = DvbSubPageInfo();
    std::vector<OnairDvbsiSubPageRegion> v(L(n) / 6 + 1);
    OnairDvbsiSubPageInfo info;
    if (!onair_dvbsi_sub_page_composition(d, L(n), &info, v.data(), v.size())) return false;
    out.timeout = info.timeout; out.version = info.version; out.state = info.state;
    for (uint32_t i = 0; i < info.n_regions && i < v.size(); i++) out.regions.push_back(DvbSubPageReg{v[i].id, v[i].x, v[i].y});
    return true;
}

bool dvbSubParseRegion(const uint8_t* d, int n, DvbSubRegionInfo& out) {
    out = DvbSubRegionInfo();
    std::vector<OnairDvbsiSubRegionObject> v(L(n) / 6 + 1);
    OnairDvbsiSubRegionInfo info;
    if (!onair_dvbsi_sub_region_composition(d, L(n), &info, v.data(), v.size())) return false;
    out.id = info.id; out.version = info.version; out.fill = info.fill != 0; out.w = info.w; out.h = info.h; out.depth = info.depth; out.clut = info.clut;
    out.pix8 = info.pix8; out.pix4 = info.pix4; out.pix2 = info.pix2;
    for (uint32_t i = 0; i < info.n_objs && i < v.size(); i++) out.objs.push_back(DvbSubObjRef{v[i].id, v[i].type, v[i].x, v[i].y, v[i].fg, v[i].bg});
    return true;
}

bool dvbSubParseClut(const uint8_t* d, int n, int& id, std::vector<DvbSubClutEntry>& out) {
    out.clear();
    std::vector<OnairDvbsiSubClutEntry> v(L(n) / 4 + 1);
    int32_t cid = 0;
    const size_t c = onair_dvbsi_sub_clut_definition(d, L(n), &cid, v.data(), v.size());
    if (c == SIZE_MAX) return false;
    id = cid;
    for (size_t i = 0; i < c && i < v.size(); i++) out.push_back(DvbSubClutEntry{v[i].id, v[i].flags, v[i].rgba});
    return true;
}

bool dvbSubParseDisplay(const uint8_t* d, int n, int& w, int& h) {
    int32_t a, b;
    if (!onair_dvbsi_sub_display_definition(d, L(n), &a, &b)) return false;
    w = a; h = b;
    return true;
}

bool dvbSubParseObject(const uint8_t* d, int n, DvbSubObjectInfo& out) {
    OnairDvbsiSubObjectInfo c;
    if (!onair_dvbsi_sub_object_data(d, L(n), &c)) return false;
    out.id = c.id; out.topOff = c.top_off; out.topLen = c.top_len; out.botOff = c.bot_off; out.botLen = c.bot_len;
    return true;
}

void dvbSubDefaultClut(uint32_t e2[4], uint32_t e4[16], uint32_t e8[256]) { onair_dvbsi_sub_default_clut(e2, e4, e8); }
uint32_t dvbSubYcrcb(int y, int cr, int cb, int t) { return onair_dvbsi_sub_ycrcb(y, cr, cb, t); }

void dvbSubDecodeField(const uint8_t* f, size_t n, int x0, int y0, int lineStep, bool doubleLines, int w, int h, int depth, uint8_t* pix) {
    onair_dvbsi_sub_decode_field(f, n, x0, y0, lineStep, doubleLines, w, h, depth, pix);
}

void dvbSubRegionRgba(const uint8_t* pix, size_t n, int depth, const uint32_t* e2, const uint32_t* e4, const uint32_t* e8, uint8_t* out) {
    onair_dvbsi_sub_region_rgba(pix, n, depth, e2, e4, e8, out);
}

} // namespace dect2
