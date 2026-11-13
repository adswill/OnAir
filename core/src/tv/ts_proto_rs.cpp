// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ts_proto.h on top of the Rust crate rust/onair_ts: record arrays sized from the section length (every record needs at least 4 bytes of
// section; the two lists that corrupt overlapping descriptors can lengthen are parsed again with the counted size), turned into the std::string / vector forms of the header.
#include "dect2/tv/ts_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/ts.h"
#include <cstdio>
#include <cstdlib>

namespace dect2::tsp {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[12];
        onair_ts_struct_sizes(s);
        dect2::rsAbiCheck("onair_ts", s, {sizeof(OnairTsPacketHeader), sizeof(OnairTsPatEntry), sizeof(OnairTsPmtStream), sizeof(OnairTsPmtSub), sizeof(OnairTsPmtInfo), sizeof(OnairTsSdtService), sizeof(OnairTsNitLcn), sizeof(OnairTsNitInfo), sizeof(OnairTsTvct), sizeof(OnairTsTdt), sizeof(OnairTsEitEvent), sizeof(OnairTsEitInfo)});
    }
} abiCheck;
inline size_t L(int len) { return len < 0 ? 0 : (size_t)len; }   // negative lengths are 0 bytes
inline Span sp(const OnairTsSpan& s) { return Span{s.off, s.len}; }
const char* kindName(int k) {
    switch (k) { case 1: return "video"; case 2: return "audio"; case 3: return "subtitles"; case 4: return "teletext"; default: return "data"; }
}
} // namespace

uint32_t crc32(const uint8_t* p, int n) { return onair_ts_crc32(p, L(n)); }

bool packetHeader(const uint8_t* p, int n, PacketHeader& h) {
    OnairTsPacketHeader c;
    if (!onair_ts_packet_header(p, L(n), &c)) return false;
    h.pid = c.pid; h.tei = c.tei != 0; h.pusi = c.pusi != 0; h.scr = c.scr; h.afc = c.afc; h.cc = c.cc; h.payloadOff = c.payload_off;
    return true;
}

void patPacket(int tsid, int sid, int pmtPid, int cc, uint8_t* out) { onair_ts_pat_packet(tsid, sid, pmtPid, cc, out); }

bool parsePat(const uint8_t* s, int len, int& tsid, std::vector<PatEntry>& out) {
    out.clear();
    std::vector<OnairTsPatEntry> v(L(len) / 4 + 1);
    int32_t t = 0;
    const size_t n = onair_ts_parse_pat(s, L(len), &t, v.data(), v.size());
    if (n == SIZE_MAX) return false;
    tsid = t;
    for (size_t i = 0; i < n; i++) out.push_back(PatEntry{v[i].prog, v[i].pid});
    return true;
}

bool parsePmt(const uint8_t* s, int len, Pmt& out) {
    out = Pmt();
    std::vector<OnairTsPmtStream> st(L(len) / 5 + 1);
    std::vector<OnairTsPmtSub> sb(L(len) / 5 + 2);
    OnairTsPmtInfo info;
    if (!onair_ts_parse_pmt(s, L(len), &info, st.data(), st.size(), sb.data(), sb.size())) return false;
    if (info.n_subs > sb.size()) {   // overlapping descriptors of a corrupt section: more entries than the section length suggests
        sb.resize(info.n_subs);
        if (!onair_ts_parse_pmt(s, L(len), &info, st.data(), st.size(), sb.data(), sb.size())) return false;
    }
    out.prog = info.prog; out.pcrPid = info.pcr_pid; out.ca = info.ca != 0;
    for (uint32_t i = 0; i < info.n_streams; i++) {
        PmtStream o;
        o.pid = st[i].pid; o.streamType = st[i].stream_type; o.kind = kindName(st[i].kind);
        o.codec.assign((const char*)st[i].codec, st[i].codec_len);
        if (st[i].has_lang) o.lang.assign((const char*)st[i].lang, 3);
        for (uint32_t k = 0; k < st[i].sub_count; k++) {
            const OnairTsPmtSub& c = sb[st[i].sub_first + k];
            PmtSub e; e.teletext = c.teletext != 0; e.lang.assign((const char*)c.lang, 3); e.type = c.type; e.page = c.page; e.ancPage = c.anc_page;
            o.subs.push_back(e);
        }
        out.streams.push_back(o);
    }
    return true;
}

bool parseSdt(const uint8_t* s, int len, int& tsid, int& onid, std::vector<SdtService>& out) {
    out.clear();
    std::vector<OnairTsSdtService> v(L(len) / 5 + 1);
    int32_t t = 0, o = 0;
    const size_t n = onair_ts_parse_sdt(s, L(len), &t, &o, v.data(), v.size());
    if (n == SIZE_MAX) return false;
    tsid = t; onid = o;
    for (size_t i = 0; i < n; i++) {
        SdtService e;
        e.sid = v[i].sid; e.freeCa = v[i].free_ca != 0; e.hasType = v[i].has_type != 0; e.hasProvider = v[i].has_provider != 0; e.hasName = v[i].has_name != 0;
        e.type = v[i].type; e.provider = sp(v[i].provider); e.name = sp(v[i].name);
        out.push_back(e);
    }
    return true;
}

bool parseNit(const uint8_t* s, int len, Nit& out) {
    out = Nit();
    std::vector<OnairTsNitLcn> v(L(len) / 4 + 1);
    OnairTsNitInfo info;
    if (!onair_ts_parse_nit(s, L(len), &info, v.data(), v.size())) return false;
    if (info.n_lcn > v.size()) {   // overlapping descriptors of a corrupt section: more entries than the section length suggests
        v.resize(info.n_lcn);
        if (!onair_ts_parse_nit(s, L(len), &info, v.data(), v.size())) return false;
    }
    out.hasName = info.has_name != 0; out.name = sp(info.name);
    for (uint32_t i = 0; i < info.n_lcn; i++) out.lcn.push_back(NitLcn{v[i].sid, v[i].lcn});
    return true;
}

bool parseTvct(const uint8_t* s, int len, int& tsid, std::vector<Tvct>& out) {
    out.clear();
    OnairTsTvct v[256];
    int32_t t = 0;
    const size_t n = onair_ts_parse_tvct(s, L(len), &t, v, 256);
    if (n == SIZE_MAX) return false;
    tsid = t;
    for (size_t i = 0; i < n; i++) {
        Tvct e;
        e.name.assign((const char*)v[i].name, v[i].name_len);
        e.major = v[i].major; e.minor = v[i].minor; e.prog = v[i].prog; e.stype = v[i].stype;
        out.push_back(e);
    }
    return true;
}

bool parseTdt(const uint8_t* s, int len, Tdt& out) {
    OnairTsTdt t;
    if (!onair_ts_parse_tdt(s, L(len), &t)) return false;
    out.unixSec = t.unix_s;
    out.text.assign((const char*)t.text, t.text_len);
    return true;
}

bool parseEit(const uint8_t* s, int len, Eit& out) {
    out = Eit();
    std::vector<OnairTsEitEvent> v(L(len) / 12 + 1);
    OnairTsEitInfo info;
    if (!onair_ts_parse_eit(s, L(len), &info, v.data(), v.size())) return false;
    out.sid = info.sid; out.secNum = info.sec_num; out.legacy = info.legacy != 0; out.hasName = info.has_name != 0; out.nowNextName = sp(info.now_next_name);
    for (uint32_t i = 0; i < info.n_events; i++) {
        const OnairTsEitEvent& c = v[i];
        EitEvent e;
        e.eventId = c.event_id; e.start = c.start; e.duration = c.duration; e.running = c.running; e.genre = c.genre;
        if (c.has_lang) e.lang.assign((const char*)c.lang, 3);
        e.hasTitle = c.has_title != 0; e.hasText = c.has_text != 0; e.title = sp(c.title); e.text = sp(c.text); e.extMask = c.ext_mask;
        for (int k = 0; k < 16; k++) e.ext[k] = sp(c.ext[k]);
        out.events.push_back(e);
    }
    return true;
}

} // namespace dect2::tsp
