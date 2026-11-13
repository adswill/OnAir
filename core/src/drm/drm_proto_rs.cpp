// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// drm_proto.h on top of the Rust crate rust/onair_drm: the FAC / SDC / super frame records of drm_msg.h are copied to and from the plain C
// records of the crate (the SDC parser works in place on them).
#include "dect2/drm/drm_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/drm.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2::drmp {
using namespace dect2::drm;

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[4];
        onair_drm_struct_sizes(s);
        dect2::rsAbiCheck("onair_drm", s, {sizeof(OnairDrmFacInfo), sizeof(OnairDrmSdcInfo), sizeof(OnairDrmAacInfo), sizeof(OnairDrmTextSeg)});
    }
} abiCheck;

template <class F> std::string name(size_t cap, F f) {
    uint8_t b[64];
    const size_t n = f(b, std::min(cap, sizeof b));
    return std::string((const char*)b, std::min(n, sizeof b));
}

void toC(const SdcMux& m, OnairDrmSdcMux& c) {
    memset(&c, 0, sizeof c);
    c.present = m.present; c.prot_a = m.protA; c.prot_b = m.protB; c.n_streams = m.nStreams;
    for (int i = 0; i < 4; i++) { c.stream[i].len_a = m.stream[i].lenA; c.stream[i].len_b = m.stream[i].lenB; }
}
void fromC(const OnairDrmSdcMux& c, SdcMux& m) {
    m.present = c.present != 0; m.protA = c.prot_a; m.protB = c.prot_b; m.nStreams = c.n_streams;
    for (int i = 0; i < 4; i++) { m.stream[i].lenA = c.stream[i].len_a; m.stream[i].lenB = c.stream[i].len_b; }
}
} // namespace

uint32_t crc8(const uint8_t* bits, size_t n) { return onair_drm_crc8_bits(bits, n); }
uint32_t crc16(const uint8_t* bits, size_t n) { return onair_drm_crc16_bits(bits, n); }

bool facParse(const uint8_t* bits, bool modeE, FacInfo& f) {
    OnairDrmFacInfo c;
    if (!onair_drm_fac_parse(bits, modeE ? 116 : 72, modeE, &c)) return false;
    f = FacInfo();
    f.enhancement = c.enhancement != 0; f.identity = c.identity; f.modeE = c.mode_e != 0; f.occupancy = c.occupancy; f.interleaver = c.interleaver;
    f.mscMode = c.msc_mode; f.sdcMode = c.sdc_mode; f.numServicesCode = c.num_services_code; f.reconfig = c.reconfig; f.toggle = c.toggle; f.nSvc = c.n_svc;
    for (int i = 0; i < 2; i++) {
        FacService& s = f.svc[i];
        s.id = c.svc[i].id; s.shortId = c.svc[i].short_id; s.audioCa = c.svc[i].audio_ca != 0; s.dataCa = c.svc[i].data_ca != 0; s.language = c.svc[i].language;
        s.data = c.svc[i].data != 0; s.descriptor = c.svc[i].descriptor;
    }
    return true;
}

int sdcParse(const uint8_t* d, int bytes, SdcInfo& out) {
    OnairDrmSdcInfo c;
    memset(&c, 0, sizeof c);
    toC(out.mux, c.mux); toC(out.muxNext, c.mux_next);
    c.time.present = out.time.present; c.time.has_offset = out.time.hasOffset; c.time.mjd = out.time.mjd; c.time.hour = out.time.hour; c.time.minute = out.time.minute;
    c.time.offset_half_hours = out.time.offsetHalfHours;
    for (int i = 0; i < 4; i++) {
        const SdcAudio& a = out.audio[i];
        OnairDrmSdcAudio& o = c.audio[i];
        o.present = a.present; o.text = a.text; o.enhancement = a.enhancement; o.short_id = a.shortId; o.stream_id = a.streamId; o.coding = a.coding; o.sbr = a.sbr;
        o.mode = a.mode; o.rate_code = a.rateCode; o.mps = a.mps; o.n_config = (uint32_t)std::min<size_t>(a.config.size(), 128);
        if (o.n_config) memcpy(o.config, a.config.data(), o.n_config);   // (an empty vector: data() may be null)
        const SdcLabel& l = out.label[i];
        OnairDrmSdcLabel& p = c.label[i];
        p.present = l.present; p.short_id = l.shortId; p.text_control = l.textControl; p.n_text = (uint32_t)std::min<size_t>(l.text.size(), 128);
        memcpy(p.text, l.text.data(), p.n_text);
        const SdcLang& g = out.lang[i];
        OnairDrmSdcLang& q = c.lang[i];
        q.present = g.present; q.short_id = g.shortId; q.n_language = (uint8_t)std::min<size_t>(g.language.size(), 3); q.n_country = (uint8_t)std::min<size_t>(g.country.size(), 2);
        memcpy(q.language, g.language.data(), q.n_language); memcpy(q.country, g.country.data(), q.n_country);
        const SdcApp& s = out.app[i];
        OnairDrmSdcApp& r = c.app[i];
        r.present = s.present; r.packet = s.packet; r.data_unit = s.dataUnit; r.short_id = s.shortId; r.stream_id = s.streamId; r.domain = s.domain; r.packet_id = s.packetId;
        r.packet_len = s.packetLen; r.n_app = (uint32_t)std::min<size_t>(s.appData.size(), 128);
        if (r.n_app) memcpy(r.app_data, s.appData.data(), r.n_app);
    }
    std::vector<uint8_t> other((size_t)(bytes < 0 ? 0 : bytes) / 2 + 1);
    const int count = onair_drm_sdc_parse(d, bytes < 0 ? 0 : (size_t)bytes, &c, other.data(), other.size());
    fromC(c.mux, out.mux); fromC(c.mux_next, out.muxNext);
    out.time.present = c.time.present != 0; out.time.hasOffset = c.time.has_offset != 0; out.time.mjd = c.time.mjd; out.time.hour = c.time.hour; out.time.minute = c.time.minute;
    out.time.offsetHalfHours = c.time.offset_half_hours;
    for (int i = 0; i < 4; i++) {
        const OnairDrmSdcAudio& o = c.audio[i];
        SdcAudio& a = out.audio[i];
        a.present = o.present != 0; a.text = o.text != 0; a.enhancement = o.enhancement != 0; a.shortId = o.short_id; a.streamId = o.stream_id; a.coding = o.coding; a.sbr = o.sbr;
        a.mode = o.mode; a.rateCode = o.rate_code; a.mps = o.mps; a.config.assign(o.config, o.config + o.n_config);
        const OnairDrmSdcLabel& p = c.label[i];
        SdcLabel& l = out.label[i];
        l.present = p.present != 0; l.shortId = p.short_id; l.textControl = p.text_control; l.text.assign((const char*)p.text, p.n_text);
        const OnairDrmSdcLang& q = c.lang[i];
        SdcLang& g = out.lang[i];
        g.present = q.present != 0; g.shortId = q.short_id; g.language.assign((const char*)q.language, q.n_language); g.country.assign((const char*)q.country, q.n_country);
        const OnairDrmSdcApp& r = c.app[i];
        SdcApp& s = out.app[i];
        s.present = r.present != 0; s.packet = r.packet != 0; s.dataUnit = r.data_unit != 0; s.shortId = r.short_id; s.streamId = r.stream_id; s.domain = r.domain; s.packetId = r.packet_id;
        s.packetLen = r.packet_len; s.appData.assign(r.app_data, r.app_data + r.n_app);
    }
    for (uint32_t i = 0; i < c.n_other && i < other.size(); i++) out.otherTypes.push_back(other[i]);
    out.entities = c.entities;
    return count;
}

bool sdcCheckBits(const uint8_t* bits, int fieldBytes, int& afsIndex) {
    int32_t a = 0;
    const bool ok = onair_drm_sdc_check_bits(bits, 4 + (size_t)(fieldBytes < 0 ? 0 : fieldBytes) * 8 + 16, (size_t)(fieldBytes < 0 ? 0 : fieldBytes), &a);
    afsIndex = a;
    return ok;
}

int aacNumFrames(bool modeE, int rateHz) { return onair_drm_aac_num_frames(modeE, rateHz); }
int aacHeaderBytes(int n) { return onair_drm_aac_header_bytes(n); }

bool aacSuperFrameParse(const uint8_t* lf, int len, int lenA, int n, AacSuperFrame& out) {
    out = AacSuperFrame();
    OnairDrmAacInfo c;
    std::vector<uint8_t> frames((size_t)(len < 0 ? 0 : len) + 1);
    const bool ok = onair_drm_aac_super_frame_parse(lf, len < 0 ? 0 : (size_t)len, lenA, n, &c, frames.data(), frames.size());
    out.numFrames = c.num_frames; out.hpBytes = c.hp_bytes; out.headerOk = c.header_ok != 0;
    if (!ok) return false;
    size_t at = 0;
    for (int i = 0; i < n && i < 16; i++) {
        out.frames.emplace_back(frames.begin() + (ptrdiff_t)at, frames.begin() + (ptrdiff_t)(at + c.frame_len[i]));
        out.crc.push_back(c.crc[i]);
        at += c.frame_len[i];
    }
    return true;
}

std::string languageName(int c) { return name(32, [&](uint8_t* o, size_t k) { return onair_drm_language_name(c, o, k); }); }
std::string programmeTypeName(int c) { return name(32, [&](uint8_t* o, size_t k) { return onair_drm_programme_type_name(c, o, k); }); }
std::string mscModeName(bool modeE, int m) { return name(32, [&](uint8_t* o, size_t k) { return onair_drm_msc_mode_name(modeE, m, o, k); }); }
std::string occupancyName(bool modeE, int o) { return name(32, [&](uint8_t* b, size_t k) { return onair_drm_occupancy_name(modeE, o, b, k); }); }
std::string appName(int domain, int app) { return name(48, [&](uint8_t* o, size_t k) { return onair_drm_app_name(domain, app, o, k); }); }
int facAudioServices(int c) { return onair_drm_fac_audio_services(c); }
int facDataServices(int c) { return onair_drm_fac_data_services(c); }
void mjdToDate(int mjd, int& y, int& m, int& d) { int32_t a, b, c; onair_drm_mjd_to_date(mjd, &a, &b, &c); y = a; m = b; d = c; }
int dateToMjd(int y, int m, int d) { return onair_drm_date_to_mjd(y, m, d); }

bool groupPayload(const uint8_t* d, size_t n, size_t& off, size_t& len) { return onair_drm_group_payload(d, n, &off, &len); }

std::string displayText(const uint8_t* b, size_t n) {
    std::vector<uint8_t> o(std::min<size_t>(n, 16384) * 2 + 8);
    return std::string((const char*)o.data(), std::min(onair_drm_display_text(b, n, o.data(), o.size()), o.size()));
}

bool looksText(const uint8_t* b, size_t n) { return onair_drm_looks_text(b, n); }
int textSegmentNeed(uint8_t b0) { return onair_drm_text_segment_need(b0); }

TextSeg textSegment(const uint8_t* seg, size_t n, int need) {
    OnairDrmTextSeg c;
    onair_drm_text_segment_parse(seg, n, need, &c);
    TextSeg r;
    r.crcOk = c.crc_ok != 0; r.toggle = c.toggle != 0; r.first = c.first != 0; r.last = c.last != 0; r.command = c.command != 0; r.cmd = c.cmd; r.segnum = c.segnum; r.n = c.n;
    return r;
}

} // namespace dect2::drmp
