// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "web_decoders.h"
#include "web_json.h"
#include "dect2/adsb/adsb_rx.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/fm/fm_tel.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/marine/marine_fax.h"
#include "dect2/meteor/meteor_rx.h"
#include "dect2/meteor/meteor_tel.h"
#include "dect2/packet/packet_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace onair_web {

using dect2::cf32;

// ================================================================ text and numbers
std::string cleanText(const std::string& s, size_t maxBytes) {
    std::string o;
    o.reserve(std::min(s.size(), maxBytes) + 4);
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        bool ok = true;
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        } else {
            ok = false;
        }
        if (ok && len > 1) {
            if (i + len > n) ok = false;
            for (size_t k = 1; ok && k < len; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) ok = false;
            if (ok && len == 3) {
                const unsigned char c1 = (unsigned char)s[i + 1];
                if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;   // overlong, surrogate
            }
            if (ok && len == 4) {
                const unsigned char c1 = (unsigned char)s[i + 1];
                if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false;
            }
        }
        if (!ok) { if (o.size() + 1 > maxBytes) break; o += '?'; i++; continue; }
        if (o.size() + len > maxBytes) break;
        if (len == 1) {
            if (c == '\n') o += '\n';
            else if (c < 0x20 || c == 0x7F) o += ' ';
            else o += (char)c;
        } else {
            o.append(s, i, len);
        }
        i += len;
    }
    return o;
}

static std::string fnum(double v, int prec = -1) {
    if (!std::isfinite(v)) return "null";
    if (prec < 0) return jsonNumber(v);
    char b[48];
    snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

namespace {
// A JSON object written key by key
struct Obj {
    std::string s = "{";
    bool first = true;
    void key(const char* k) { if (!first) s += ','; first = false; s += '"'; s += k; s += "\":"; }
    Obj& num(const char* k, double v, int prec = -1) { key(k); s += fnum(v, prec); return *this; }
    Obj& i(const char* k, long long v) { key(k); s += std::to_string(v); return *this; }
    Obj& u(const char* k, unsigned long long v) { key(k); s += std::to_string(v); return *this; }
    Obj& str(const char* k, const std::string& v, size_t max = 512) { key(k); s += '"'; s += jsonEsc(cleanText(v, max)); s += '"'; return *this; }
    Obj& b(const char* k, bool v) { key(k); s += v ? "true" : "false"; return *this; }
    Obj& raw(const char* k, const std::string& v) { key(k); s += v; return *this; }
    std::string done() { return s + "}"; }
};

constexpr size_t kTableCap = 56000;   // the array part of a table event stays under this (an event is at most 64 KB)

std::string hex6(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }
}   // namespace

std::string statusEventJson(const std::string& kind, const std::string& state, const std::string& why) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", kind).str("type", "status").str("state", state).str("why", why);
    return o.done();
}

// ================================================================ base64, CRC, PNG
std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == n) {
        const uint32_t v = (uint32_t)p[i] << 16;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == n) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
    }
    return o;
}

uint32_t crc32Of(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
static void pngChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32(out, crc32Of(td.data(), td.size()));
}

std::vector<uint8_t> pngEncode(const uint8_t* px, int w, int h, int channels, size_t stride) {
    if (!px || w <= 0 || h <= 0 || w > 65535 || h > 65535 || (channels != 1 && channels != 3)) return {};
    if (stride == 0) stride = (size_t)w * channels;
    const size_t rowBytes = (size_t)w * channels;
    if (stride < rowBytes) return {};
    std::vector<uint8_t> raw;
    raw.reserve((rowBytes + 1) * (size_t)h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), px + (size_t)y * stride, px + (size_t)y * stride + rowBytes);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + n));
        pos += n;
    } while (pos < raw.size());
    put32(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w); put32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(channels == 1 ? 0 : 2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    pngChunk(out, "IHDR", ihdr);
    pngChunk(out, "IDAT", z);
    pngChunk(out, "IEND", {});
    return out;
}

std::vector<uint8_t> boxDownscale(const uint8_t* px, int w, int h, int channels, int f, int& ow, int& oh) {
    if (f < 1) f = 1;
    ow = (w + f - 1) / f; oh = (h + f - 1) / f;
    std::vector<uint8_t> o((size_t)ow * oh * channels);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
            for (int c = 0; c < channels; c++) {
                unsigned sum = 0, cnt = 0;
                for (int yy = y * f; yy < std::min(h, (y + 1) * f); yy++)
                    for (int xx = x * f; xx < std::min(w, (x + 1) * f); xx++) { sum += px[((size_t)yy * w + xx) * channels + c]; cnt++; }
                o[((size_t)y * ow + x) * channels + c] = (uint8_t)(cnt ? sum / cnt : 0);
            }
    return o;
}

// ================================================================ pictures as strips
namespace { constexpr size_t kStripRaw = 36000; }   // raw bytes of a strip: base64 makes it 48 KB, under the 64 KB event limit

void ImageStreamer::begin(uint64_t id, int w, int h, int channels, const std::string& label) {
    id_ = id ? id : 1;
    w_ = std::max(1, std::min(w, 8192)); h_ = std::max(1, std::min(h, 20000)); ch_ = channels == 3 ? 3 : 1;
    label_ = label;
    sent_flags_.assign((size_t)h_, 0);
    sent_ = 0; first_ = 0; finalSent_ = false;
}

std::string ImageStreamer::stripEvent(const ImageView& v, int y0, int rows, bool preview, int scale, bool fin) const {
    const size_t rb = (size_t)w_ * ch_;
    std::vector<uint8_t> buf((size_t)rows * rb, 0);
    for (int k = 0; k < rows; k++) {
        const uint8_t* r = v.row ? v.row(y0 + k) : nullptr;
        if (r) std::memcpy(&buf[(size_t)k * rb], r, rb);
    }
    std::vector<uint8_t> png;
    int pw = w_, ph = rows;
    if (preview && scale > 1) {
        int ow = 0, oh = 0;
        std::vector<uint8_t> d = boxDownscale(buf.data(), w_, rows, ch_, scale, ow, oh);
        png = pngEncode(d.data(), ow, oh, ch_);
        pw = ow; ph = oh;
    } else {
        png = pngEncode(buf.data(), w_, rows, ch_);
    }
    Obj o;
    o.i("v", kDecoderSchema).str("kind", kind_).str("type", "image").u("id", id_).str("label", label_, 64).i("w", w_).i("h", h_).i("ch", ch_).i("y", y0).i("rows", rows)
        .i("scale", preview ? scale : 1).b("preview", preview).b("final", fin).i("pw", pw).i("ph", ph).str("png", base64(png.data(), png.size()), 1 << 20);
    return o.done();
}

void ImageStreamer::poll(const ImageView& v, bool complete, int maxEvents, std::vector<std::string>& ev) {
    if (!active() || v.w != w_ || !v.row) return;
    const size_t rb = (size_t)w_ * ch_ + 1;
    const int maxRows = (int)std::max<size_t>(1, kStripRaw / rb);
    const int limit = std::min(h_, v.h > 0 ? v.h : h_);
    int made = 0;
    while (made < maxEvents) {
        while (first_ < h_ && sent_flags_[(size_t)first_]) first_++;
        int y = first_;
        while (y < limit && (sent_flags_[(size_t)y] || !v.row(y))) y++;
        if (y >= limit) break;
        int n = 0;
        while (y + n < limit && n < maxRows && !sent_flags_[(size_t)(y + n)] && v.row(y + n)) n++;
        for (int k = 0; k < n; k++) { sent_flags_[(size_t)(y + k)] = 1; sent_++; }
        // final: the picture is complete and nothing else is waiting
        bool more = false;
        if (complete) for (int yy = y + n; yy < limit && !more; yy++) if (!sent_flags_[(size_t)yy] && v.row(yy)) more = true;
        const bool fin = complete && !more;
        if (fin) finalSent_ = true;
        ev.push_back(stripEvent(v, y, n, false, 1, fin));
        made++;
    }
    if (made < maxEvents && complete && !finalSent_) {   // complete, every available row was sent earlier
        finalSent_ = true;
        int top = 0;
        for (int yy = limit - 1; yy >= 0; yy--) if (sent_flags_[(size_t)yy]) { top = yy + 1; break; }
        Obj o;
        o.i("v", kDecoderSchema).str("kind", kind_).str("type", "image_end").u("id", id_).i("rows", top);
        ev.push_back(o.done());
    }
}

void ImageStreamer::snapshot(const ImageView& v, bool complete, std::vector<std::string>& ev) const {
    if (!active() || v.w != w_ || !v.row) return;
    int top = 0;
    const int limit = std::min(h_, v.h > 0 ? v.h : h_);
    for (int y = limit - 1; y >= 0; y--) if (v.row(y)) { top = y + 1; break; }
    if (top <= 0) return;
    int f = 1;
    for (;; f++) {
        const size_t ow = (size_t)(w_ + f - 1) / f, oh = (size_t)(top + f - 1) / f;
        if (ow * oh * ch_ + oh <= kStripRaw || f >= 256) break;
    }
    ev.push_back(stripEvent(v, 0, top, true, f, complete));
}

// ================================================================ table events
std::string adsbEventJson(const dect2::AdsbTelemetry& t) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", "adsb").str("type", "aircraft").i("state", t.state).num("msgs_per_s", t.msgsPerSec, 1).num("snr_db", t.snrDb, 1)
        .num("noise_dbfs", t.noiseDbfs, 1).u("messages_ok", t.blocksOk).u("messages_bad", t.blocksBad).u("aircraft_count", t.aircraftCount).u("with_position", t.withPosition);
    std::string a = "[";
    bool first = true;
    for (const auto& x : t.aircraft) {
        if (a.size() > kTableCap) break;
        Obj p;
        p.str("icao", hex6(x.icao), 8);
        if (!x.callsign.empty()) p.str("callsign", x.callsign, 16);
        if (!x.category.empty()) p.str("category", x.category, 4);
        if (x.hasAlt) p.i("alt_ft", x.altFt);
        if (x.hasSpeed) p.num("speed_kt", x.speedKt, 0);
        if (x.hasHeading) p.num("track_deg", x.headingDeg, 0);
        if (x.hasVrate) p.i("vrate_fpm", x.vrateFpm);
        if (x.hasPos) { p.num("lat", x.lat, 5).num("lon", x.lon, 5); }
        if (x.hasSquawk) { char sq[8]; snprintf(sq, sizeof sq, "%04d", x.squawk); p.str("squawk", sq, 8); }
        if (x.ground) p.b("ground", true);
        if (x.emergency) p.i("emergency", x.emergency);
        p.u("msgs", x.messages).num("age_s", x.ageSec, 0).num("level_dbfs", x.levelDbfs, 0);
        if (x.hasPos && x.track.size() > 1) {   // a short trail for the map: the last points, oldest first
            std::string tr = "[";
            const size_t from = x.track.size() > 8 ? x.track.size() - 8 : 0;
            for (size_t k = from; k < x.track.size(); k++) { if (k > from) tr += ','; tr += "[" + fnum(x.track[k].lat, 4) + "," + fnum(x.track[k].lon, 4) + "]"; }
            p.raw("trail", tr + "]");
        }
        a += (first ? "" : ",") + p.done();
        first = false;
    }
    o.raw("aircraft", a + "]");
    return o.done();
}

std::string aisEventJson(const dect2::AisTelemetry& t) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", "ais").str("type", "vessels").i("state", t.state).num("snr_db", t.snrDb, 1).u("messages_ok", t.blocksOk).u("messages_bad", t.blocksBad)
        .u("vessel_count", t.vesselCount);
    std::string a = "[";
    bool first = true;
    for (const auto& x : t.vessels) {
        if (a.size() > kTableCap) break;
        Obj p;
        p.u("mmsi", x.mmsi).str("class", dect2::aisClassText(x.cls), 24);
        if (!x.name.empty()) p.str("name", x.name, 40);
        if (!x.callsign.empty()) p.str("callsign", x.callsign, 16);
        if (x.shipType >= 0) p.i("ship_type", x.shipType).str("ship_type_text", dect2::aisShipTypeText(x.shipType), 40);
        if (x.navStatus >= 0) p.i("nav_status", x.navStatus).str("nav_status_text", dect2::aisNavStatusText(x.navStatus), 40);
        if (x.hasPos) p.num("lat", x.lat, 5).num("lon", x.lon, 5);
        if (x.sog >= 0) p.num("sog_kt", x.sog, 1);
        if (x.cog >= 0) p.num("cog_deg", x.cog, 0);
        if (x.heading >= 0) p.i("heading_deg", x.heading);
        if (!x.destination.empty()) p.str("destination", x.destination, 40);
        if (x.imo) p.u("imo", x.imo);
        p.u("msgs", x.messages).num("age_s", x.ageSec, 0);
        a += (first ? "" : ",") + p.done();
        first = false;
    }
    o.raw("vessels", a + "]");
    return o.done();
}

std::string aprsEventJson(const dect2::PacketTelemetry& t) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", "aprs").str("type", "packets").i("state", t.state).num("snr_db", t.snrDb, 1).u("frames_ok", t.blocksOk).u("frames_bad", t.blocksBad)
        .u("frames_1200", t.ok1200).u("frames_9600", t.ok9600);
    std::string fr = "[";
    bool first = true;
    const size_t fromF = t.frames.size() > 30 ? t.frames.size() - 30 : 0;   // the newest frames, newest last
    for (size_t k = fromF; k < t.frames.size(); k++) {
        const auto& f = t.frames[k];
        Obj p;
        p.num("t", f.timeSec, 1).str("from", f.from, 16).str("to", f.to, 16).str("path", f.path, 80).str("type", f.type, 32).str("summary", f.summary, 200).i("baud", f.baud);
        if (f.hasPos) p.num("lat", f.lat, 5).num("lon", f.lon, 5);
        fr += (first ? "" : ",") + p.done();
        first = false;
    }
    o.raw("frames", fr + "]");
    std::string st = "[";
    first = true;
    for (const auto& x : t.stations) {
        if (st.size() > kTableCap - 16000) break;
        Obj p;
        p.str("call", x.call, 16).num("last_heard_s", x.lastHeardSec, 1).u("count", x.count);
        if (x.symTable && x.symCode) p.str("symbol", std::string(1, x.symTable) + std::string(1, x.symCode), 4);
        if (x.hasPos) p.num("lat", x.lat, 5).num("lon", x.lon, 5);
        if (x.hasAlt) p.num("alt_m", x.altM, 0);
        if (x.hasCourse) p.i("course_deg", x.courseDeg);
        if (x.hasSpeed) p.num("speed_kt", x.speedKnots, 0);
        if (!x.comment.empty()) p.str("comment", x.comment, 100);
        if (x.isObject) p.b("object", true);
        st += (first ? "" : ",") + p.done();
        first = false;
    }
    o.raw("stations", st + "]");
    return o.done();
}

std::string ftxEventJson(const std::string& kind, double dialHz, const std::vector<dect2::FtxDecode>& d, float clockErrS, int clockN) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", kind).str("type", "decodes").num("dial_hz", dialHz, 0).num("clock_err_s", clockErrS, 2).i("clock_n", clockN);
    std::string a = "[";
    bool first = true;
    for (const auto& x : d) {
        if (a.size() > kTableCap) break;
        Obj p;
        p.num("slot_utc", x.slotUtc, 1).num("snr_db", x.snrDb, 0).num("dt_s", x.dt, 1).num("audio_hz", x.hz, 0).num("rf_hz", dialHz + x.hz, 0).str("msg", x.msg, 120);
        if (x.cq) p.b("cq", true);
        if (!x.call.empty()) p.str("call", x.call, 16);
        if (!x.grid.empty()) p.str("grid", x.grid, 8);
        if (x.dbm >= 0) p.i("dbm", x.dbm);
        a += (first ? "" : ",") + p.done();
        first = false;
    }
    o.raw("decodes", a + "]");
    return o.done();
}

std::string rdsEventJson(const dect2::FmTelemetry& t) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", "rds").str("type", "rds").i("state", t.state).b("carrier", t.carrier).b("stereo", t.stereo).num("snr_db", t.snrDb, 0)
        .num("level_dbfs", t.levelDbfs, 0).b("sync", t.rdsSync).num("block_ok_pct", t.rdsBlockOkPct, 0).u("groups", t.rdsGroups);
    char pi[8];
    snprintf(pi, sizeof pi, "%04X", t.piCode & 0xFFFF);
    if (t.piCode) o.str("pi", pi, 8);
    if (!t.psName.empty()) o.str("ps", t.psName, 16);
    if (!t.radioText.empty()) o.str("radiotext", t.radioText, 80).b("radiotext_complete", t.radioTextComplete);
    if (!t.ptyText.empty()) o.str("pty", t.ptyText, 32);
    if (!t.callSign.empty()) o.str("callsign", t.callSign, 8);
    o.b("tp", t.trafficProgram).b("ta", t.trafficAlert).b("music", t.music).b("rbds", t.rbds);
    return o.done();
}

std::string cwEventJson(const dect2::CwTelemetry& t) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", "cw").str("type", "signals").i("state", t.state).num("snr_db", t.snrDb, 0).num("window_hz", t.windowHz, 0).u("chars", t.blocksOk);
    std::string a = "[";
    bool first = true;
    for (const auto& x : t.signals) {
        if (a.size() > kTableCap) break;
        Obj p;
        p.i("id", x.id).num("offset_hz", x.offsetHz, 0).num("wpm", x.wpm, 0).num("snr_db", x.snrDb, 0).b("locked", x.locked).u("chars", x.chars).str("text", x.text, 700);
        if (!x.calls.empty()) {
            std::string c = "[";
            for (size_t k = 0; k < x.calls.size() && k < 8; k++) c += (k ? ",\"" : "\"") + jsonEsc(cleanText(x.calls[k], 16)) + "\"";
            p.raw("calls", c + "]");
        }
        a += (first ? "" : ",") + p.done();
        first = false;
    }
    o.raw("signals", a + "]");
    return o.done();
}

std::string rttyEventJson(double markHz, double spaceHz, double baud, double shiftHz, int state, float quality, uint64_t chars, const std::string& text) {
    Obj o;
    o.i("v", kDecoderSchema).str("kind", "rtty").str("type", "text").i("state", state).num("mark_hz", markHz, 0).num("space_hz", spaceHz, 0).num("baud", baud, 2)
        .num("shift_hz", shiftHz, 0).num("quality", quality, 2).u("chars", chars);
    const std::string tail = text.size() > 2500 ? text.substr(text.size() - 2500) : text;
    o.str("text", tail, 2600);
    return o.done();
}

// ================================================================ USB audio from the slice
UsbAudio::UsbAudio(double rate) : rate_(rate) {
    const int N = 127, M = N / 2;
    const double fc = 1600.0 / rate, f0 = 1800.0 / rate, pi = 3.14159265358979323846;
    taps_.resize((size_t)N);
    for (int n = 0; n < N; n++) {
        const double k = n - M;
        const double sinc = k == 0 ? 2 * fc : std::sin(2 * pi * fc * k) / (pi * k);
        const double win = 0.42 - 0.5 * std::cos(2 * pi * n / (N - 1)) + 0.08 * std::cos(4 * pi * n / (N - 1));
        const double h = sinc * win;
        taps_[(size_t)n] = cf32((float)(h * std::cos(2 * pi * f0 * k)), (float)(h * std::sin(2 * pi * f0 * k)));
    }
    hist_.assign((size_t)N, cf32(0, 0));
}

void UsbAudio::process(const cf32* x, size_t n, std::vector<float>& out) {
    out.clear();
    out.reserve(n);
    const size_t N = taps_.size();
    for (size_t i = 0; i < n; i++) {
        hist_[pos_] = x[i];
        float re = 0;
        size_t idx = pos_;
        for (size_t k = 0; k < N; k++) {   // newest sample against tap 0
            const cf32 h = taps_[k], s = hist_[idx];
            re += h.real() * s.real() - h.imag() * s.imag();
            idx = idx == 0 ? N - 1 : idx - 1;
        }
        pos_ = pos_ + 1 == N ? 0 : pos_ + 1;
        const float a = std::fabs(re);
        env_ = std::max(a, env_ * (float)(1.0 - 1.0 / (rate_ * 2.0)));
        out.push_back(re * (0.5f / std::max(env_, 1e-4f)));
    }
}

// ================================================================ the decoders
namespace {

struct Spec { const char* kind; const char* name; double rate, minHz, maxHz; };
const Spec kSpecs[] = {
    {"adsb", "ADS-B / Mode S aircraft", 2000000, 1087e6, 1093e6},
    {"ais", "AIS ships", 250000, 161.9e6, 162.1e6},
    {"aprs", "APRS / AX.25 packet", 192000, 29e6, 470e6},
    {"ft8", "FT8", 250000, 0.1e6, 450e6},
    {"ft4", "FT4", 250000, 0.1e6, 450e6},
    {"wspr", "WSPR", 250000, 0.1e6, 450e6},
    {"sstv", "SSTV images", 250000, 0.1e6, 450e6},
    {"wefax", "Weather fax", 12000, 0.1e6, 30e6},
    {"meteor", "Meteor-M LRPT images", 288000, 136e6, 138.5e6},
    {"rds", "RDS", 500000, 64e6, 110e6},
    {"cw", "Morse", 250000, 0.1e6, 450e6},
    {"rtty", "RTTY", 250000, 0.1e6, 450e6},
};
const Spec* findSpec(const std::string& k) { for (const auto& s : kSpecs) if (k == s.kind) return &s; return nullptr; }

// Common to all: rate bookkeeping, the "no data" notice, and the last events for a listener who joins later
class RxDec : public WebDecoder {
public:
    RxDec(std::string kind, double freqHz) : kind_(std::move(kind)), freq_(freqHz), t0_(wallSeconds()) {}
    void feed(const cf32* x, size_t n) override { fed_ += n; feedRx(x, n); }
    void poll(std::vector<std::string>& ev) override {
        const double now = wallSeconds();
        if (fed_ == 0) {
            if (!warned_ && now - t0_ > 4.0) {
                warned_ = true;
                ev.push_back(statusEventJson(kind_, "no_data", "no samples reach this decoder: the capture is too narrow or does not cover this frequency"));
            }
            return;
        }
        pollRx(now, ev);
    }
    void snapshot(std::vector<std::string>& ev) override { for (const auto& e : cache_) ev.push_back(e); }

protected:
    virtual void feedRx(const cf32* x, size_t n) = 0;
    virtual void pollRx(double now, std::vector<std::string>& ev) = 0;
    void emitCached(std::vector<std::string>& ev, std::string e) { cache_.assign(1, e); ev.push_back(std::move(e)); }
    std::string kind_;
    double freq_;
    std::vector<std::string> cache_;
    uint64_t fed_ = 0;
    double t0_;
    bool warned_ = false;
    double next_ = 0;
};

class AdsbDec : public RxDec {
public:
    AdsbDec(double f) : RxDec("adsb", f) { rx_.configure(2000000); }
    void feedRx(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void pollRx(double now, std::vector<std::string>& ev) override {
        if (now < next_) return;
        dect2::AdsbTelemetry t;
        if (rx_.telemetry(t, seq_)) { seq_ = t.seq; next_ = now + 1.0; emitCached(ev, adsbEventJson(t)); }
    }
private:
    dect2::AdsbReceiver rx_;
    uint64_t seq_ = 0;
};

class AisDec : public RxDec {
public:
    AisDec(double f) : RxDec("ais", f) { rx_.setForwardFeeds(false); rx_.configure(250000); rx_.setSignalOffset(162.0e6 - f); }
    void feedRx(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void pollRx(double now, std::vector<std::string>& ev) override {
        if (now < next_) return;
        dect2::AisTelemetry t;
        if (rx_.telemetry(t, seq_)) { seq_ = t.seq; next_ = now + 1.0; emitCached(ev, aisEventJson(t)); }
    }
private:
    dect2::AisReceiver rx_;
    uint64_t seq_ = 0;
};

class AprsDec : public RxDec {
public:
    AprsDec(double f) : RxDec("aprs", f) { rx_.setSilent(true); rx_.setListen(false); rx_.setWideInput(true); rx_.configure(192000); }
    void feedRx(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void pollRx(double now, std::vector<std::string>& ev) override {
        if (now < next_) return;
        dect2::PacketTelemetry t;
        if (rx_.telemetry(t, seq_)) { seq_ = t.seq; next_ = now + 1.0; emitCached(ev, aprsEventJson(t)); }
    }
private:
    dect2::PacketReceiver rx_;
    uint64_t seq_ = 0;
};

class RdsDec : public RxDec {
public:
    RdsDec(double f) : RxDec("rds", f) { rx_.setSilent(true); rx_.setMuted(true); rx_.configure(500000); }
    void feedRx(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void pollRx(double now, std::vector<std::string>& ev) override {
        if (now < next_) return;
        dect2::FmTelemetry t;
        if (rx_.telemetry(t, seq_)) { seq_ = t.seq; next_ = now + 1.0; emitCached(ev, rdsEventJson(t)); }
    }
private:
    dect2::FmReceiver rx_;
    uint64_t seq_ = 0;
};

class CwDec : public RxDec {
public:
    CwDec(double f) : RxDec("cw", f) { rx_.setSilent(true); rx_.setMuted(true); rx_.setSidetone(false); rx_.configure(250000); }
    void feedRx(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void pollRx(double now, std::vector<std::string>& ev) override {
        if (now < next_) return;
        dect2::CwTelemetry t;
        if (rx_.telemetry(t, seq_)) { seq_ = t.seq; next_ = now + 1.0; emitCached(ev, cwEventJson(t)); }
    }
private:
    dect2::CwReceiver rx_;
    uint64_t seq_ = 0;
};

// FT8 / FT4 / WSPR, SSTV and RTTY all ride on the HF digital receiver with one decoder switched on
class HfDec : public RxDec {
public:
    HfDec(const std::string& kind, double f) : RxDec(kind, f) {
        rx_.setSilent(true);
        rx_.setListen(false);
        rx_.setMuted(true);
        const bool isFtx = kind == "ft8" || kind == "ft4" || kind == "wspr";
        mode_ = kind == "ft8" ? 0 : kind == "ft4" ? 1 : 3;
        rx_.setDecoders(isFtx ? 8 : kind == "sstv" ? 2 : 1);
        rx_.setDemod(kind == "sstv" || kind == "rtty" ? dect2::HfDemodAuto : dect2::HfDemodUsb);   // Auto: NFM at 30 MHz and above (VHF SSTV)
        if (isFtx) for (int m = 0; m < dect2::kFtxModes; m++) rx_.ftx().setEnabled(m, m == mode_);
        rx_.configure(250000);
        if (kind == "sstv") sstv_ = std::make_unique<ImageStreamer>("sstv");
    }
    void feedRx(const cf32* x, size_t n) override { rx_.feed(x, n); }

    void pollRx(double now, std::vector<std::string>& ev) override {
        if (kind_ == "sstv") { pollSstv(now, ev); return; }
        if (now < next_) return;
        if (kind_ == "rtty") {
            dect2::HfdigTelemetry t;
            if (rx_.telemetry(t, seq_)) {
                seq_ = t.seq; next_ = now + 1.0;
                const auto& r = t.rtty;
                emitCached(ev, rttyEventJson(r.markHz, r.spaceHz, r.baud, r.shiftHz, r.state, r.quality, r.chars, r.text));
            }
            return;
        }
        // FT8 / FT4 / WSPR: the decodes that are new
        next_ = now + 1.0;
        dect2::HfdigFtxTelemetry t;
        rx_.ftx().telemetry(t);
        const uint64_t total = t.perMode[mode_];
        if (total < seenTotal_) seenTotal_ = 0;   // the host's decoder was cleared
        uint64_t add = total - seenTotal_;
        if (add == 0) return;
        seenTotal_ = total;
        std::vector<dect2::FtxDecode> mine;
        for (const auto& d : t.decodes) if (d.mode == mode_) mine.push_back(d);
        if (add > mine.size()) add = mine.size();
        std::vector<dect2::FtxDecode> fresh(mine.end() - (long)add, mine.end());
        for (const auto& d : fresh) recent_.push_back(d);
        while (recent_.size() > 40) recent_.erase(recent_.begin());
        clockErr_ = t.clockErr; clockN_ = t.clockN;
        ev.push_back(ftxEventJson(kind_, freq_, fresh, t.clockErr, t.clockN));
    }

    void snapshot(std::vector<std::string>& ev) override {
        if (kind_ == "sstv") {
            if (img_) { auto view = viewOf(*img_); sstv_->snapshot(view, img_->complete, ev); }
            for (const auto& e : cache_) ev.push_back(e);
        } else if (kind_ == "rtty") {
            for (const auto& e : cache_) ev.push_back(e);
        } else if (!recent_.empty()) {
            ev.push_back(ftxEventJson(kind_, freq_, recent_, clockErr_, clockN_));   // the last decodes, so a new listener does not start with an empty list
        }
    }

private:
    static ImageView viewOf(const dect2::HfdigSstvImage& im) {
        ImageView v;
        v.w = im.width; v.h = im.height; v.channels = 3;
        const dect2::HfdigSstvImage* p = &im;
        v.row = [p](int y) -> const uint8_t* {
            return y >= 0 && y < p->lines && y < p->height && p->rgb.size() >= (size_t)(y + 1) * p->width * 3 ? p->rgb.data() + (size_t)y * p->width * 3 : nullptr;
        };
        return v;
    }
    void pollSstv(double now, std::vector<std::string>& ev) {
        if (now >= next_) {
            dect2::HfdigTelemetry t;
            if (rx_.telemetry(t, seq_)) {
                seq_ = t.seq; next_ = now + 0.5;
                if (t.sstv.image && t.sstv.image->width > 0 && t.sstv.image->height > 0) {
                    img_ = t.sstv.image;
                    if (img_->id != curId_) {
                        curId_ = img_->id;
                        sstv_->begin(img_->id + 1, img_->width, img_->height, 3, img_->mode);
                    }
                }
                Obj o;
                o.i("v", kDecoderSchema).str("kind", "sstv").str("type", "status").i("state", t.sstv.state).str("mode", t.sstv.mode, 32).i("lines", t.sstv.lines).i("height", t.sstv.height)
                    .u("pictures", t.sstv.picturesDone).num("offset_hz", t.sstv.offsetHz, 0);
                const std::string s = o.done();
                if (s != lastStatus_) { lastStatus_ = s; emitCached(ev, s); }
            }
        }
        if (img_ && sstv_->active()) {
            auto view = viewOf(*img_);
            sstv_->poll(view, img_->complete, 2, ev);
        }
    }

    dect2::HfdigReceiver rx_;
    uint64_t seq_ = 0, seenTotal_ = 0, curId_ = ~0ull;
    int mode_ = 0;
    std::vector<dect2::FtxDecode> recent_;
    float clockErr_ = 0; int clockN_ = 0;
    std::unique_ptr<ImageStreamer> sstv_;
    std::shared_ptr<const dect2::HfdigSstvImage> img_;
    std::string lastStatus_;
};

class WefaxDec : public RxDec {
public:
    WefaxDec(double f) : RxDec("wefax", f), usb_(12000), str_("wefax") { fax_.configure(12000); }
    void feedRx(const cf32* x, size_t n) override {
        usb_.process(x, n, aud_);
        fax_.push(aud_.data(), aud_.size());
    }
    void feedAudio(const float* a, size_t n) { fed_ += n; fax_.push(a, n); }   // tests
    void pollRx(double now, std::vector<std::string>& ev) override {
        if (now >= next_) {
            next_ = now + 1.0;
            const dect2::FaxStatus st = fax_.status();
            const bool restarted = (st.lines < lastLines_) || (prevState_ >= 3 && st.state > 0 && st.state < 3);
            lastLines_ = st.lines; prevState_ = st.state;
            dect2::FaxImage im;
            uint64_t sq = seq_;
            if (fax_.latestImage(im, sq)) {
                seq_ = sq;
                if (im.width > 0 && im.height > 0) {
                    if (restarted || !str_.active() || im.width != w_) { w_ = im.width; str_.begin(++id_, w_, 1500, 1, "IOC " + std::to_string(st.ioc) + ", " + std::to_string(st.lpm) + " LPM"); }
                    img_ = std::make_shared<dect2::FaxImage>(std::move(im));
                }
            } else if (restarted && str_.active()) {
                str_.begin(++id_, w_, 1500, 1, "IOC " + std::to_string(st.ioc) + ", " + std::to_string(st.lpm) + " LPM");
                img_.reset();
            }
            complete_ = st.state == 3;
            Obj o;
            o.i("v", kDecoderSchema).str("kind", "wefax").str("type", "status").i("state", st.state).i("lines", st.lines).i("ioc", st.ioc).i("lpm", st.lpm).num("snr_db", st.snrDb, 0)
                .num("slant_ppm", st.slantPpm, 0);
            const std::string s = o.done();
            if (s != lastStatus_) { lastStatus_ = s; emitCached(ev, s); }
        }
        if (img_ && str_.active()) { auto v = viewOf(*img_); str_.poll(v, complete_, 2, ev); }
    }
    void snapshot(std::vector<std::string>& ev) override {
        if (img_ && str_.active()) { auto v = viewOf(*img_); str_.snapshot(v, complete_, ev); }
        for (const auto& e : cache_) ev.push_back(e);
    }

private:
    static ImageView viewOf(const dect2::FaxImage& im) {
        ImageView v;
        v.w = im.width; v.h = im.height; v.channels = 1;
        const dect2::FaxImage* p = &im;
        v.row = [p](int y) -> const uint8_t* { return y >= 0 && y < p->height && p->pix.size() >= (size_t)(y + 1) * p->width ? p->pix.data() + (size_t)y * p->width : nullptr; };
        return v;
    }
    dect2::FaxDecoder fax_;
    UsbAudio usb_;
    ImageStreamer str_;
    std::vector<float> aud_;
    std::shared_ptr<dect2::FaxImage> img_;
    uint64_t seq_ = 0, id_ = 0;
    int w_ = 0, lastLines_ = 0, prevState_ = 0;
    bool complete_ = false;
    std::string lastStatus_;
};

class MeteorDec : public RxDec {
public:
    MeteorDec(double f) : RxDec("meteor", f), str_("meteor") { rx_.configure(288000); }
    void feedRx(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void pollRx(double now, std::vector<std::string>& ev) override {
        if (now >= next_) {
            dect2::MeteorTelemetry t;
            if (rx_.telemetry(t, seq_)) {
                seq_ = t.seq; next_ = now + 0.5;
                if (t.image && t.image->rows > 0) {
                    if (t.image->id != curId_) { curId_ = t.image->id; chan_ = -1; str_.begin(t.image->id + 1, dect2::kMeteorWidth, dect2::kMeteorMaxRows * 8, 1, "MSU-MR"); }
                    img_ = t.image;
                    if (chan_ < 0 || img_->packets[(size_t)chan_] == 0) {
                        uint64_t best = 0;
                        for (int c = 0; c < 6; c++) if (img_->packets[(size_t)c] > best) { best = img_->packets[(size_t)c]; chan_ = c; }
                    }
                }
                Obj o;
                o.i("v", kDecoderSchema).str("kind", "meteor").str("type", "status").i("state", t.state).str("modulation", dect2::meteorModName(t.modulation), 24).num("snr_db", t.snrDb, 1)
                    .u("frames_ok", t.blocksOk).u("frames_bad", t.blocksBad).i("lines", t.image ? t.image->rows * 8 : 0).i("channel", chan_ >= 0 ? chan_ + 1 : 0);
                const std::string s = o.done();
                if (s != lastStatus_) { lastStatus_ = s; emitCached(ev, s); }
            }
        }
        if (img_ && chan_ >= 0 && str_.active()) { auto v = viewOf(*img_, chan_); str_.poll(v, false, 2, ev); }
    }
    void snapshot(std::vector<std::string>& ev) override {
        if (img_ && chan_ >= 0 && str_.active()) { auto v = viewOf(*img_, chan_); str_.snapshot(v, false, ev); }
        for (const auto& e : cache_) ev.push_back(e);
    }

private:
    // A strip is shown when all of its packets arrived, or when later strips exist (its missing parts are given up)
    static ImageView viewOf(const dect2::MeteorImage& im, int c) {
        ImageView v;
        v.w = dect2::kMeteorWidth; v.h = im.rows * 8; v.channels = 1;
        const dect2::MeteorImage* p = &im;
        v.row = [p, c](int y) -> const uint8_t* {
            const size_t k = (size_t)(y >= 0 ? y / 8 : 0);
            const auto& strips = p->ch[(size_t)c];
            if (y < 0 || k >= strips.size() || !strips[k] || strips[k]->px.size() < (size_t)8 * dect2::kMeteorWidth) return nullptr;
            bool ok = strips[k]->got == 0x3FFF;
            for (size_t j = k + 1; !ok && j < std::min(strips.size(), k + 4); j++) if (strips[j]) ok = true;
            return ok ? strips[k]->px.data() + (size_t)(y % 8) * dect2::kMeteorWidth : nullptr;
        };
        return v;
    }
    dect2::MeteorReceiver rx_;
    ImageStreamer str_;
    std::shared_ptr<const dect2::MeteorImage> img_;
    uint64_t seq_ = 0, curId_ = ~0ull;
    int chan_ = -1;
    std::string lastStatus_;
};

}   // namespace

// ================================================================ registry
bool webDecoderSpec(const std::string& kind, double& rateHz, double& minHz, double& maxHz) {
    const Spec* s = findSpec(kind);
    if (!s) return false;
    rateHz = s->rate; minHz = s->minHz; maxHz = s->maxHz;
    return true;
}
bool webDecoderFreqOk(const std::string& kind, double hz) {
    const Spec* s = findSpec(kind);
    return s && std::isfinite(hz) && hz >= s->minHz && hz <= s->maxHz;
}
std::vector<std::string> webDecoderKinds() {
    std::vector<std::string> v;
    for (const auto& s : kSpecs) v.push_back(s.kind);
    return v;
}

static std::unique_ptr<WebDecoder> makeDecoder(const std::string& kind, double f) {
    if (!webDecoderFreqOk(kind, f)) return nullptr;
    if (kind == "adsb") return std::make_unique<AdsbDec>(f);
    if (kind == "ais") return std::make_unique<AisDec>(f);
    if (kind == "aprs") return std::make_unique<AprsDec>(f);
    if (kind == "rds") return std::make_unique<RdsDec>(f);
    if (kind == "cw") return std::make_unique<CwDec>(f);
    if (kind == "wefax") return std::make_unique<WefaxDec>(f);
    if (kind == "meteor") return std::make_unique<MeteorDec>(f);
    if (kind == "ft8" || kind == "ft4" || kind == "wspr" || kind == "sstv" || kind == "rtty") return std::make_unique<HfDec>(kind, f);
    return nullptr;
}

std::vector<std::string> registerWebDecoders(WebHub& hub) {
    std::vector<std::string> done;
    for (const auto& s : kSpecs) {
        if (decoderClass(s.kind) != DecoderClass::Data) continue;   // voice and pager decoders are never registered here
        const std::string kind = s.kind;
        hub.registerDecoder(DecoderFactory{kind, s.name, s.rate, true, [kind](double f) { return makeDecoder(kind, f); }});
        done.push_back(kind);
    }
    return done;
}

}   // namespace onair_web
