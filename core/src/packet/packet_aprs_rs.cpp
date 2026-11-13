// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The APRS information field parser (see packet_aprs.h) on top of the Rust crate rust/onair_ax25. Written from APRS101 (chapters 5-12) and the APRS 1.2
// addenda; the Mic-E longitude and speed rules were cross-checked against the description in Dire Wolf's decode_aprs.c (WB2OSZ, GPL-2.0-or-later) - no code
// was copied.
#include "dect2/packet/packet_aprs.h"
#include "dect2/rs/ax25.h"
#include <cstdint>
#include <cstdlib>

namespace dect2 {
namespace aprs {

namespace {
template <class F> std::string text(F f) {
    std::string s(64, '\0');
    for (;;) {
        const size_t n = f((uint8_t*)&s[0], s.size());
        if (n != SIZE_MAX) { s.resize(n); return s; }
        s.assign(s.size() * 4, '\0');
    }
}
} // namespace

std::string fmtLat(double v) { return text([&](uint8_t* o, size_t c) { return onair_ax25_aprs_fmt_lat(v, o, c); }); }
std::string fmtLon(double v) { return text([&](uint8_t* o, size_t c) { return onair_ax25_aprs_fmt_lon(v, o, c); }); }

bool parse(const std::string& dest, const std::string& info, Info& o) {
    OnairAprsInfo r;
    std::string buf(info.size() * 3 + 512, '\0');
    int rc;
    for (;;) {
        rc = onair_ax25_aprs_parse(reinterpret_cast<const uint8_t*>(dest.data()), dest.size(), reinterpret_cast<const uint8_t*>(info.data()), info.size(), &r,
                              reinterpret_cast<uint8_t*>(&buf[0]), buf.size());
        if (rc >= 0) break;
        buf.assign(buf.size() * 2, '\0');
    }
    auto str = [&](const OnairSpan& s) { return buf.substr(s.off, s.len); };
    o = Info();
    o.type = text([&](uint8_t* p, size_t c) { return onair_ax25_aprs_type_name(r.kind, p, c); });
    o.hasPos = r.has_pos != 0;
    o.lat = r.lat; o.lon = r.lon;
    o.symTable = (char)r.sym_table; o.symCode = (char)r.sym_code;
    o.hasAlt = r.has_alt != 0; o.altM = r.alt_m;
    o.hasCourse = r.has_course != 0; o.courseDeg = r.course_deg;
    o.hasSpeed = r.has_speed != 0; o.speedKnots = r.speed_knots;
    o.name = str(r.name);
    o.live = r.live != 0;
    o.comment = str(r.comment);
    o.text = str(r.text);
    o.msgNo = str(r.msg_no);
    o.isAck = r.is_ack != 0; o.isRej = r.is_rej != 0;
    o.micEStatus = str(r.mic_status);
    o.wx.windDirDeg = r.wx[0]; o.wx.windMph = r.wx[1]; o.wx.gustMph = r.wx[2]; o.wx.tempF = r.wx[3];
    o.wx.rain1hIn = r.wx[4]; o.wx.rain24hIn = r.wx[5]; o.wx.rainMidnightIn = r.wx[6];
    o.wx.humidityPct = r.wx[7]; o.wx.pressureMbar = r.wx[8];
    o.hasWx = r.has_wx != 0;
    o.summary = str(r.summary);
    return rc == 1;
}

} // namespace aprs
} // namespace dect2
