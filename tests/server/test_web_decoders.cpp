// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web decoders (server/web/web_decoders.cpp): the pure helpers (text clean-up, base64, CRC, PNG, picture strips, USB audio, the event JSON of each
// table and its size limit), the policy side (only data decoders are registered, the voice / pager ones never; frequency bands), every decoder started
// through the hub on noise (no crash, valid events, nothing but a subscription), and ADS-B end to end: a synthetic airspace in, an aircraft list out.
#include "../../server/web/web_decoders.h"
#include "../../server/web/web_hub.h"
#include "dect2/adsb/adsb_gen.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/fm/fm_tel.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/packet/packet_tel.h"
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <thread>
using namespace onair_web;
#include "../common/test_util.h"

// ---------------------------------------------------------------- helpers: a strict JSON validator (the hub's parser has input limits), base64 and PNG decoders
struct JV {
    const std::string& s; size_t i = 0; int depth = 0;
    explicit JV(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++; }
    bool str() {
        if (s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            const unsigned char c = (unsigned char)s[i];
            if (c < 0x20) return false;
            if (c == '\\') { i++; if (i >= s.size() || !strchr("\"\\/bfnrtu", s[i])) return false; if (s[i] == 'u') i += 4; i++; continue; }
            if (c >= 0x80) {   // valid UTF-8 only
                int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
                if (!len || i + len > s.size()) return false;
                for (int k = 1; k < len; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
                i += len; continue;
            }
            i++;
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    bool val() {
        ws();
        if (i >= s.size() || ++depth > 12) return false;
        bool ok = false;
        const char c = s[i];
        if (c == '{') {
            i++; ws();
            if (s[i] == '}') { i++; ok = true; }
            else for (;;) { ws(); if (!str()) break; ws(); if (s[i] != ':') break; i++; if (!val()) break; ws(); if (s[i] == ',') { i++; continue; } if (s[i] == '}') { i++; ok = true; } break; }
        } else if (c == '[') {
            i++; ws();
            if (s[i] == ']') { i++; ok = true; }
            else for (;;) { if (!val()) break; ws(); if (s[i] == ',') { i++; continue; } if (s[i] == ']') { i++; ok = true; } break; }
        } else if (c == '"') ok = str();
        else if (!s.compare(i, 4, "true")) { i += 4; ok = true; }
        else if (!s.compare(i, 5, "false")) { i += 5; ok = true; }
        else if (!s.compare(i, 4, "null")) { i += 4; ok = true; }
        else { size_t j = i; if (s[j] == '-') j++; size_t d = j; while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' || s[j] == '+' || s[j] == '-')) j++; ok = j > d; i = j; }
        depth--;
        return ok;
    }
};
static bool validJson(const std::string& t) { JV v(t); if (!v.val()) return false; v.ws(); return v.i == t.size(); }

static std::vector<uint8_t> b64dec(const std::string& s) {
    std::vector<uint8_t> o;
    uint32_t acc = 0; int bits = 0;
    for (char c : s) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((uint8_t)(acc >> bits)); }
    }
    return o;
}

// Decodes a PNG made of stored blocks: checks the signature, every chunk's CRC and the Adler-32; returns the pixels (filter 0 rows)
static bool pngDecode(const std::vector<uint8_t>& p, int& w, int& h, int& ch, std::vector<uint8_t>& px) {
    if (p.size() < 8 || memcmp(p.data(), "\x89PNG\r\n\x1a\n", 8)) return false;
    size_t i = 8;
    std::vector<uint8_t> z;
    bool ihdr = false, iend = false;
    int ct = 0;
    while (i + 12 <= p.size()) {
        const uint32_t len = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 | (uint32_t)p[i + 2] << 8 | p[i + 3];
        if (i + 12 + len > p.size()) return false;
        const uint32_t crc = crc32Of(&p[i + 4], len + 4);
        const uint32_t want = (uint32_t)p[i + 8 + len] << 24 | (uint32_t)p[i + 9 + len] << 16 | (uint32_t)p[i + 10 + len] << 8 | p[i + 11 + len];
        if (crc != want) return false;
        const std::string type((const char*)&p[i + 4], 4);
        if (type == "IHDR") {
            w = (int)((uint32_t)p[i + 8] << 24 | (uint32_t)p[i + 9] << 16 | (uint32_t)p[i + 10] << 8 | p[i + 11]);
            h = (int)((uint32_t)p[i + 12] << 24 | (uint32_t)p[i + 13] << 16 | (uint32_t)p[i + 14] << 8 | p[i + 15]);
            ct = p[i + 17]; ihdr = p[i + 16] == 8;
            ch = ct == 0 ? 1 : 3;
        } else if (type == "IDAT") z.insert(z.end(), p.begin() + (long)i + 8, p.begin() + (long)i + 8 + len);
        else if (type == "IEND") iend = true;
        i += 12 + len;
    }
    if (!ihdr || !iend || z.size() < 6 || z[0] != 0x78) return false;
    std::vector<uint8_t> raw;
    size_t k = 2;
    for (;;) {
        if (k + 5 > z.size()) return false;
        const bool last = z[k] & 1;
        if ((z[k] >> 1) & 3) return false;   // stored blocks only
        const size_t n = z[k + 1] | (size_t)z[k + 2] << 8;
        if ((z[k + 3] | (size_t)z[k + 4] << 8) != (~n & 0xFFFF)) return false;
        k += 5;
        if (k + n > z.size()) return false;
        raw.insert(raw.end(), z.begin() + (long)k, z.begin() + (long)(k + n));
        k += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    if (k + 4 != z.size() || z[k] != (b >> 8) || z[k + 1] != (uint8_t)b || z[k + 2] != (a >> 8) || z[k + 3] != (uint8_t)a) return false;
    const size_t rb = (size_t)w * ch;
    if (raw.size() != (rb + 1) * (size_t)h) return false;
    px.clear();
    for (int y = 0; y < h; y++) { if (raw[(size_t)y * (rb + 1)] != 0) return false; px.insert(px.end(), raw.begin() + (long)((size_t)y * (rb + 1) + 1), raw.begin() + (long)((size_t)(y + 1) * (rb + 1))); }
    return true;
}

static double numField(const std::string& e, const char* k) {
    const std::string pat = std::string("\"") + k + "\":";
    const size_t p = e.find(pat);
    return p == std::string::npos ? -1e30 : atof(e.c_str() + p + pat.size());
}
static std::string strField(const std::string& e, const char* k) {
    const std::string pat = std::string("\"") + k + "\":\"";
    size_t p = e.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    return e.substr(p, e.find('"', p) - p);
}
static bool boolField(const std::string& e, const char* k) { return e.find(std::string("\"") + k + "\":true") != std::string::npos; }

// ---------------------------------------------------------------- tests
static void testText() {
    CHECK(cleanText("hello") == "hello", "plain");
    CHECK(cleanText("a\x01" "b\x7f" "c") == "a b c", "control characters become spaces");
    CHECK(cleanText("a\nb") == "a\nb", "a newline stays (JSON escapes it)");
    CHECK(cleanText("caf\xC3\xA9") == "caf\xC3\xA9", "valid UTF-8 stays");
    CHECK(cleanText("ab\xFF" "cd") == "ab?cd", "a bad byte becomes ?");
    CHECK(cleanText("ab\xC3") == "ab?", "a cut sequence becomes ?");
    CHECK(cleanText("\xC0\x80") == "??", "an overlong sequence is refused");
    CHECK(cleanText("\xED\xA0\x80") == "???", "a surrogate is refused");
    CHECK(cleanText("\xF4\x90\x80\x80") == "????", "above U+10FFFF is refused");
    CHECK(cleanText("abcdef", 3) == "abc", "cut at the limit");
    CHECK(cleanText("ab\xC3\xA9", 3) == "ab", "never cut inside a character");
    std::string all;
    for (int i = 0; i < 256; i++) all += (char)i;
    CHECK(validJson("\"" + jsonEsc(cleanText(all)) + "\""), "any bytes in, valid JSON out");
}

static void testEncoders() {
    CHECK(base64((const uint8_t*)"Man", 3) == "TWFu" && base64((const uint8_t*)"Ma", 2) == "TWE=" && base64((const uint8_t*)"M", 1) == "TQ==" && base64(nullptr, 0).empty(), "base64 vectors");
    CHECK(crc32Of((const uint8_t*)"123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    // small PNGs
    std::vector<uint8_t> px = {255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30, 40, 50, 60, 70, 80, 90};   // 3 x 2 RGB
    auto png = pngEncode(px.data(), 3, 2, 3);
    int w, h, ch; std::vector<uint8_t> back;
    CHECK(pngDecode(png, w, h, ch, back) && w == 3 && h == 2 && ch == 3 && back == px, "RGB PNG round trip");
    std::vector<uint8_t> g(100 * 700);
    for (size_t k = 0; k < g.size(); k++) g[k] = (uint8_t)((k * 7) ^ (k >> 5));
    png = pngEncode(g.data(), 100, 700, 1);   // 70000 bytes: more than one stored block
    CHECK(pngDecode(png, w, h, ch, back) && w == 100 && h == 700 && ch == 1 && back == g, "grey PNG, two stored blocks");
    // a stride wider than the row
    std::vector<uint8_t> s(2 * 8, 9);
    for (int x = 0; x < 4; x++) { s[(size_t)x] = (uint8_t)x; s[8 + (size_t)x] = (uint8_t)(10 + x); }
    png = pngEncode(s.data(), 4, 2, 1, 8);
    CHECK(pngDecode(png, w, h, ch, back) && back == std::vector<uint8_t>({0, 1, 2, 3, 10, 11, 12, 13}), "stride respected");
    CHECK(pngEncode(nullptr, 3, 2, 3).empty() && pngEncode(px.data(), 0, 2, 3).empty() && pngEncode(px.data(), 3, 2, 2).empty(), "bad arguments give nothing");
    int ow, oh;
    std::vector<uint8_t> d = boxDownscale(std::vector<uint8_t>{0, 100, 200, 100}.data(), 2, 2, 1, 2, ow, oh);
    CHECK(ow == 1 && oh == 1 && d[0] == 100, "box downscale mean");
}

static void testUsbAudio() {
    const double fs = 12000;
    UsbAudio u(fs);
    // +1000 Hz (USB) small, -2000 Hz (the lower sideband) three times stronger: only the first may come out
    std::vector<cf32> x((size_t)fs * 2);
    for (size_t n = 0; n < x.size(); n++) {
        const double a = 2 * M_PI * 1000 * (double)n / fs, b = -2 * M_PI * 2000 * (double)n / fs;
        x[n] = cf32((float)(0.05 * std::cos(a) + 0.15 * std::cos(b)), (float)(0.05 * std::sin(a) + 0.15 * std::sin(b)));
    }
    std::vector<float> out;
    u.process(x.data(), x.size(), out);
    CHECK(out.size() == x.size(), "one audio sample per input sample");
    auto goertzel = [&](double f) {
        double re = 0, im = 0;
        for (size_t n = (size_t)fs; n < out.size(); n++) { re += out[n] * std::cos(2 * M_PI * f * (double)n / fs); im += out[n] * std::sin(2 * M_PI * f * (double)n / fs); }
        return std::sqrt(re * re + im * im) / (fs / 2);
    };
    const double p1 = goertzel(1000), p2 = goertzel(2000);
    CHECK(p1 > 0.2 && p2 < p1 * 0.06, "the upper sideband passes, the lower is rejected by 24 dB or more (%.3f vs %.3f)", p1, p2);
    float peak = 0;
    for (size_t n = (size_t)fs; n < out.size(); n++) peak = std::max(peak, std::fabs(out[n]));
    CHECK(peak > 0.3f && peak < 0.8f, "the AGC brings it to about 0.5 (%.2f)", peak);
}

// A picture of w x h grey (or RGB) whose rows 0 .. avail-1 exist, except those in the gap
struct Pic {
    int w, h, ch, avail = 0, gapFrom = 0, gapTo = 0;
    uint8_t px(int x, int y, int c) const { return (uint8_t)(x * 3 + y * 5 + c * 40); }
    std::vector<std::vector<uint8_t>> rows;
    Pic(int w_, int h_, int ch_) : w(w_), h(h_), ch(ch_) {
        rows.resize((size_t)h);
        for (int y = 0; y < h; y++) { rows[(size_t)y].resize((size_t)w * ch); for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) rows[(size_t)y][(size_t)x * ch + c] = px(x, y, c); }
    }
    ImageView view() const {
        ImageView v; v.w = w; v.h = h; v.channels = ch;
        v.row = [this](int y) -> const uint8_t* { return y >= 0 && y < avail && !(y >= gapFrom && y < gapTo) ? rows[(size_t)y].data() : nullptr; };
        return v;
    }
};

// Puts the strips of the events back together; returns the rows that arrived
static std::vector<char> assemble(const std::vector<std::string>& ev, const Pic& pic, std::vector<uint8_t>& canvas, int& finals, int& ends, bool& allUnder64k) {
    canvas.assign((size_t)pic.w * pic.h * pic.ch, 0);
    std::vector<char> got((size_t)pic.h, 0);
    finals = ends = 0;
    allUnder64k = true;
    for (const auto& e : ev) {
        if (e.size() > 65536) allUnder64k = false;
        if (!validJson(e)) { allUnder64k = false; continue; }
        if (strField(e, "type") == "image_end") { ends++; continue; }
        if (boolField(e, "final")) finals++;
        if (boolField(e, "preview")) continue;
        int w, h, ch; std::vector<uint8_t> px;
        if (!pngDecode(b64dec(strField(e, "png")), w, h, ch, px) || w != pic.w || ch != pic.ch) { allUnder64k = false; continue; }
        const int y0 = (int)numField(e, "y");
        for (int k = 0; k < h && y0 + k < pic.h; k++) {
            std::copy(px.begin() + (long)((size_t)k * w * ch), px.begin() + (long)((size_t)(k + 1) * w * ch), canvas.begin() + (long)((size_t)(y0 + k) * pic.w * pic.ch));
            got[(size_t)(y0 + k)] = 1;
        }
    }
    return got;
}

static void testImageStreamer() {
    {   // rows arrive in two steps, the last strip says final
        Pic pic(64, 40, 1);
        ImageStreamer s("wefax");
        s.begin(5, pic.w, pic.h, 1, "IOC 576");
        std::vector<std::string> ev;
        pic.avail = 10;
        s.poll(pic.view(), false, 5, ev);
        CHECK(s.sentRows() == 10 && !ev.empty(), "the first rows go out (%d)", s.sentRows());
        s.poll(pic.view(), false, 5, ev);
        CHECK(s.sentRows() == 10, "a row is sent once");
        pic.avail = 40;
        s.poll(pic.view(), true, 5, ev);
        std::vector<uint8_t> canvas; int finals, ends; bool ok;
        auto got = assemble(ev, pic, canvas, finals, ends, ok);
        bool all = true;
        for (char c : got) all = all && c;
        std::vector<uint8_t> want;
        for (const auto& r : pic.rows) want.insert(want.end(), r.begin(), r.end());
        CHECK(all && canvas == want && ok, "all rows arrive intact");
        CHECK(finals == 1 && ends == 0, "exactly one final strip (%d, %d)", finals, ends);
        CHECK(strField(ev[0], "kind") == "wefax" && numField(ev[0], "v") == kDecoderSchema && numField(ev[0], "id") == 5, "envelope: %s", ev[0].substr(0, 80).c_str());
        ev.clear();
        s.poll(pic.view(), true, 5, ev);
        CHECK(ev.empty(), "nothing more after the final strip");
    }
    {   // a complete picture whose rows were all sent before: image_end
        Pic pic(32, 8, 3);
        pic.avail = 8;
        ImageStreamer s("sstv");
        s.begin(1, 32, 8, 3, "Robot 36");
        std::vector<std::string> ev;
        s.poll(pic.view(), false, 5, ev);
        const size_t n = ev.size();
        s.poll(pic.view(), true, 5, ev);
        CHECK(ev.size() == n + 1 && strField(ev.back(), "type") == "image_end" && numField(ev.back(), "rows") == 8, "image_end when nothing was left to send");
    }
    {   // a gap that never arrives: skipped, the rest is sent, final still comes
        Pic pic(40, 60, 1);
        pic.avail = 60; pic.gapFrom = 20; pic.gapTo = 30;
        ImageStreamer s("meteor");
        s.begin(2, 40, 60, 1, "MSU-MR");
        std::vector<std::string> ev;
        s.poll(pic.view(), true, 20, ev);
        std::vector<uint8_t> canvas; int finals, ends; bool ok;
        auto got = assemble(ev, pic, canvas, finals, ends, ok);
        bool gapEmpty = true, restFull = true;
        for (int y = 0; y < 60; y++) { if (y >= 20 && y < 30) gapEmpty = gapEmpty && !got[(size_t)y]; else restFull = restFull && got[(size_t)y]; }
        CHECK(gapEmpty && restFull && finals == 1, "gap rows are not sent, the others are (final %d)", finals);
    }
    {   // a big fax: many strips, each well under the event limit; a preview for a late listener
        Pic pic(1809, 1500, 1);
        pic.avail = 1500;
        ImageStreamer s("wefax");
        s.begin(9, pic.w, pic.h, 1, "IOC 576");
        std::vector<std::string> ev;
        int rounds = 0;
        while (s.sentRows() < 1500 && rounds++ < 200) s.poll(pic.view(), true, 2, ev);
        std::vector<uint8_t> canvas; int finals, ends; bool ok;
        auto got = assemble(ev, pic, canvas, finals, ends, ok);
        size_t have = 0;
        for (char c : got) have += c ? 1 : 0;
        std::vector<uint8_t> want;
        for (const auto& r : pic.rows) want.insert(want.end(), r.begin(), r.end());
        CHECK(have == 1500 && canvas == want && ok && finals == 1, "1809 x 1500 fax arrives in %zu events (%zu rows, final %d, under 64 KB: %d)", ev.size(), have, finals, (int)ok);
        std::vector<std::string> sn;
        pic.avail = 1000;
        ImageStreamer s2("wefax");
        s2.begin(9, pic.w, pic.h, 1, "IOC 576");
        s2.snapshot(pic.view(), false, sn);
        CHECK(sn.size() == 1 && sn[0].size() < 65536 && validJson(sn[0]) && boolField(sn[0], "preview") && numField(sn[0], "scale") >= 2 && numField(sn[0], "rows") == 1000,
              "preview of 1000 rows in one small event (%zu bytes, scale %.0f)", sn.empty() ? (size_t)0 : sn[0].size(), sn.empty() ? 0.0 : numField(sn[0], "scale"));
        if (!sn.empty()) {
            int w, h, ch; std::vector<uint8_t> px;
            const bool dec = pngDecode(b64dec(strField(sn[0], "png")), w, h, ch, px);
            CHECK(dec && w == numField(sn[0], "pw") && h == numField(sn[0], "ph") && w * numField(sn[0], "scale") >= 1809 - numField(sn[0], "scale"), "the preview PNG is valid and as announced");
        }
    }
    {   // a small SSTV picture in colour needs no downscale
        Pic pic(320, 20, 3);
        pic.avail = 20;
        ImageStreamer s("sstv");
        s.begin(3, 320, 256, 3, "Robot 36");
        std::vector<std::string> sn;
        s.snapshot(pic.view(), false, sn);
        CHECK(sn.size() == 1 && numField(sn[0], "scale") == 1 && numField(sn[0], "rows") == 20, "20 RGB rows: preview at full size");
    }
}

static std::string junk(std::mt19937& g, int n) { std::string s; for (int i = 0; i < n; i++) s += (char)(g() & 0xFF); return s; }

static void testEventJson() {
    std::mt19937 g(7);
    {   // ADS-B: 400 aircraft with hostile callsigns
        dect2::AdsbTelemetry t;
        t.state = 2; t.msgsPerSec = 123.4f; t.aircraftCount = 400; t.withPosition = 380;
        for (int i = 0; i < 400; i++) {
            dect2::AdsbAircraft a;
            a.icao = (uint32_t)g() & 0xFFFFFF; a.callsign = junk(g, 8); a.category = "A3";
            a.hasAlt = true; a.altFt = 38000; a.hasSpeed = true; a.speedKt = 450; a.hasHeading = true; a.headingDeg = 271; a.hasVrate = true; a.vrateFpm = -640;
            a.hasPos = true; a.lat = 52.123456 + i * 0.001; a.lon = 4.5; a.hasSquawk = true; a.squawk = 7700; a.emergency = 1; a.messages = 100;
            for (int k = 0; k < 40; k++) { dect2::AdsbTrackPoint p; p.lat = 52 + k * 0.01f; p.lon = 4 + k * 0.01f; a.track.push_back(p); }
            t.aircraft.push_back(a);
        }
        const std::string e = adsbEventJson(t);
        CHECK(e.size() <= 65536 && validJson(e), "ADS-B event: %zu bytes, valid", e.size());
        CHECK(strField(e, "kind") == "adsb" && strField(e, "type") == "aircraft" && numField(e, "v") == 1 && numField(e, "aircraft_count") == 400, "ADS-B envelope");
        CHECK(e.find("\"squawk\":\"7700\"") != std::string::npos && e.find("\"lat\":52.12346") != std::string::npos, "positions and squawk written");
        t.aircraft.resize(2);
        const std::string e2 = adsbEventJson(t);
        CHECK(validJson(e2) && e2.find("\"trail\":[[") != std::string::npos, "a trail is included");
    }
    {
        dect2::AisTelemetry t;
        t.vesselCount = 400; t.state = 2;
        for (int i = 0; i < 400; i++) {
            dect2::AisVessel v;
            v.mmsi = 200000000u + (uint32_t)i; v.name = junk(g, 20); v.callsign = "PA1234"; v.destination = junk(g, 20); v.shipType = 70; v.navStatus = 0;
            v.hasPos = true; v.lat = 51.9; v.lon = 4.1; v.sog = 12.3f; v.cog = 90; v.heading = 91; v.messages = 5;
            t.vessels.push_back(v);
        }
        const std::string e = aisEventJson(t);
        CHECK(e.size() <= 65536 && validJson(e) && e.find("\"ship_type_text\":\"Cargo\"") != std::string::npos, "AIS event: %zu bytes, valid", e.size());
    }
    {
        dect2::PacketTelemetry t;
        for (int i = 0; i < 100; i++) {
            dect2::PacketFrameInfo f; f.from = junk(g, 9); f.to = "APRS"; f.path = "WIDE1-1*"; f.type = "Position"; f.summary = junk(g, 100); f.hasPos = true; f.lat = 1; f.lon = 2;
            t.frames.push_back(f);
            dect2::PacketStation s; s.call = junk(g, 9); s.symTable = '/'; s.symCode = '>'; s.comment = junk(g, 100); s.hasPos = true; s.lat = 52; s.lon = 5; s.count = 3;
            for (int k = 0; k < 4; k++) t.stations.push_back(s);
        }
        const std::string e = aprsEventJson(t);
        CHECK(e.size() <= 65536 && validJson(e), "APRS event: %zu bytes, valid", e.size());
    }
    {
        std::vector<dect2::FtxDecode> d;
        for (int i = 0; i < 30; i++) { dect2::FtxDecode x; x.msg = "CQ " + junk(g, 6); x.cq = true; x.call = "PA1ABC"; x.grid = "JO21"; x.hz = 1500; x.snrDb = -12; d.push_back(x); }
        const std::string e = ftxEventJson("ft8", 14074000, d, 0.1f, 5);
        CHECK(validJson(e) && numField(e, "rf_hz") == 14075500 && strField(e, "kind") == "ft8", "FT8 event");
    }
    {
        dect2::FmTelemetry t;
        t.psName = "RADIO 1"; t.radioText = junk(g, 64); t.piCode = 0x4A21; t.rdsSync = true; t.ptyText = "Pop Music";
        const std::string e = rdsEventJson(t);
        CHECK(validJson(e) && strField(e, "ps") == "RADIO 1" && strField(e, "pi") == "4A21", "RDS event");
    }
    {
        dect2::CwTelemetry t;
        for (int i = 0; i < 24; i++) { dect2::CwSignal s; s.id = i; s.text = junk(g, 600); s.calls = {"PA1ABC", "DL2XYZ"}; t.signals.push_back(s); }
        const std::string e = cwEventJson(t);
        CHECK(e.size() <= 65536 && validJson(e), "CW event: %zu bytes, valid", e.size());
        const std::string r = rttyEventJson(2125, 2295, 45.45, 170, 2, 0.8f, 99999, junk(g, 9000));
        CHECK(r.size() <= 65536 && validJson(r), "RTTY event: %zu bytes, valid", r.size());
        CHECK(validJson(statusEventJson("sstv", "no_data", "why \"quoted\"")), "status event");
    }
}

static void testPolicySide() {
    const auto kinds = webDecoderKinds();
    CHECK(kinds.size() == 12, "twelve decoders (%zu)", kinds.size());
    for (const auto& k : kinds) {
        // CW and RTTY are class Other in the web policy: the web hub never registers them (the allow-list fails closed)
        const bool other = (k == "cw" || k == "rtty");
        CHECK(decoderClass(k) == (other ? DecoderClass::Other : DecoderClass::Data), "%s has the expected policy class", k.c_str());
        bool inCat = false;
        for (const auto& d : decoderCatalogue()) if (k == d.kind) inCat = true;
        CHECK(inCat, "%s is in the policy catalogue", k.c_str());
        double r, lo, hi;
        CHECK(webDecoderSpec(k, r, lo, hi) && r >= 8000 && lo < hi, "%s has a spec", k.c_str());
    }
    for (const char* k : {"dmr", "p25", "nxdn", "tetra", "dpmr", "dstar", "ysf", "pocsag", "flex", "acars", "apt", "host"}) {
        double r, lo, hi;
        CHECK(!webDecoderSpec(k, r, lo, hi), "%s is not one of the web decoders", k);
    }
    CHECK(webDecoderFreqOk("adsb", 1090e6) && !webDecoderFreqOk("adsb", 1030e6) && !webDecoderFreqOk("adsb", 100e6), "ADS-B only near 1090 MHz");
    CHECK(webDecoderFreqOk("ais", 162.0e6) && !webDecoderFreqOk("ais", 145e6), "AIS only near 162 MHz");
    CHECK(webDecoderFreqOk("meteor", 137.9e6) && !webDecoderFreqOk("meteor", 144e6), "Meteor only near 137 MHz");
    CHECK(webDecoderFreqOk("wefax", 7.88e6) && !webDecoderFreqOk("wefax", 145e6), "fax on HF");
    CHECK(webDecoderFreqOk("rds", 93.9e6) && !webDecoderFreqOk("rds", 14e6), "RDS on the FM band");
    CHECK(!webDecoderFreqOk("ft8", NAN) && !webDecoderFreqOk("nothing", 14e6), "NaN and unknown kinds refused");
    WebHub h;
    const auto reg = registerWebDecoders(h);
    std::vector<std::string> dataKinds;
    for (const auto& k : kinds) if (decoderClass(k) == DecoderClass::Data) dataKinds.push_back(k);
    CHECK(reg == dataKinds && reg.size() == 10, "the data decoders are registered, in order (%zu)", reg.size());
    std::string c, w;
    AdmitRequest a; a.ip = "10.0.0.1";
    const int id = h.admit(a, c, w);
    const std::string wl = h.welcomeJson(id);
    for (const auto& k : dataKinds) CHECK(wl.find("\"kind\":\"" + k + "\"") != std::string::npos, "welcome offers %s", k.c_str());
    for (const char* k : {"\"cw\"", "\"rtty\""}) CHECK(wl.find(std::string("\"kind\":") + k) == std::string::npos, "welcome does not offer %s (not on the web allow-list)", k);
    for (const char* k : {"\"dmr\"", "\"pocsag\""}) CHECK(wl.find(std::string("\"kind\":") + k) == std::string::npos, "welcome does not offer %s", k);
    h.leave(id);
}

// ---------------------------------------------------------------- through the hub
struct Drained { std::vector<std::string> text; };
static Drained drainAll(WebHub& h, int id) {
    Drained d;
    std::vector<OutMsg> out; std::string kick;
    h.drain(id, out, kick);
    for (auto& m : out) if (!m.binary) d.text.push_back(m.data);
    return d;
}
static ClientMsg msg(const std::string& j) { ClientMsg m; std::string c, w; if (!parseClientMsg(j, m, c, w)) printf("bad test message %s: %s\n", j.c_str(), w.c_str()); return m; }

static void testEveryDecoderOnNoise() {
    std::mt19937 g(3);
    std::normal_distribution<float> nd(0.f, 0.01f);
    const double rate = 2.4e6;
    for (const auto& k : webDecoderKinds()) {
        if (decoderClass(k) != DecoderClass::Data) continue;   // CW / RTTY are not registered with the hub
        double r, lo, hi;
        webDecoderSpec(k, r, lo, hi);
        const double f = k == "adsb" ? 1090e6 : k == "ais" ? 162.0e6 : k == "aprs" ? 144.8e6 : k == "meteor" ? 137.9e6 : k == "rds" ? 93.9e6 : k == "wefax" ? 7.88e6 : 14.074e6;
        WebHub h;
        HubLimits L; L.maxDecoders = 4; h.setLimits(L);
        registerWebDecoders(h);
        std::string c, w;
        AdmitRequest a; a.ip = "10.0.0.1";
        const int id = h.admit(a, c, w);
        std::vector<cf32> x(48000);
        auto push = [&](int blocks) { for (int b = 0; b < blocks; b++) { for (auto& v : x) v = cf32(nd(g), nd(g)); h.pushIq(x.data(), x.size(), f, rate); h.processPending(); } };
        push(1);
        h.handle(id, msg("{\"type\":\"subscribe\",\"decoder\":\"" + k + "\",\"freq_hz\":" + std::to_string((long long)f) + "}"));
        h.start();
        push(10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        h.stop();
        Drained d = drainAll(h, id);
        bool err = false, ev = true, subbed = false;
        for (const auto& t : d.text) {
            if (t.find("\"type\":\"error\"") != std::string::npos) { err = true; printf("  %s: %s\n", k.c_str(), t.c_str()); }
            if (t.find("\"type\":\"event\"") != std::string::npos && !validJson(t)) ev = false;
            if (t.find("\"subscriptions\":[\"" + k) != std::string::npos) subbed = true;
        }
        CHECK(!err && ev && subbed, "%s on noise: no error (%d), events valid (%d), subscribed (%d)", k.c_str(), (int)!err, (int)ev, (int)subbed);
        // a frequency outside the decoder's band: refused, no instance
        h.handle(id, msg("{\"type\":\"unsubscribe\",\"decoder\":\"" + k + "\",\"freq_hz\":" + std::to_string((long long)f) + "}"));
        h.leave(id);
    }
}

static void testPolicyOff() {
    WebHub h;
    registerWebDecoders(h);
    std::string c, w;
    AdmitRequest a; a.ip = "10.0.0.1";
    const int id = h.admit(a, c, w);
    std::vector<cf32> x(24000, cf32(0.01f, 0));
    h.pushIq(x.data(), x.size(), 100e6, 2.4e6);
    h.processPending();
    std::string e;
    {
        std::lock_guard<std::mutex> g(h.policyMutex());
        h.policy().setDecoder("ais", false, "", e);
    }
    h.handle(id, msg("{\"type\":\"subscribe\",\"decoder\":\"ais\",\"freq_hz\":100000000}"));
    CHECK(drainAll(h, id).text.size() >= 1, "the host turned AIS off");
    Drained d = drainAll(h, id);
    h.handle(id, msg("{\"type\":\"subscribe\",\"decoder\":\"ais\",\"freq_hz\":100000000}"));
    bool refused = false;
    for (const auto& t : drainAll(h, id).text) if (t.find("not_allowed") != std::string::npos) refused = true;
    CHECK(refused, "a decoder the host turned off is refused");
    // a frequency outside the decoder's band is refused too (the factory returns no instance)
    h.handle(id, msg("{\"type\":\"subscribe\",\"decoder\":\"adsb\",\"freq_hz\":100000000}"));
    bool busy = false;
    for (const auto& t : drainAll(h, id).text) if (t.find("\"type\":\"error\"") != std::string::npos) busy = true;
    CHECK(busy, "ADS-B at 100 MHz cannot be started");
    h.leave(id);
}

static void testAdsbEndToEnd() {
    const double rate = 2.4e6;
    dect2::SynthConfig sc;
    sc.snrDb = 40;
    sc.modeOpt[0] = 8;
    sc.modeOpt[1] = 1;
    sc.modeVal[2] = 52.0; sc.modeVal[3] = 4.0;
    auto syn = dect2::makeAdsbSynth(sc, rate);
    CHECK(syn != nullptr, "ADS-B generator");
    if (!syn) return;
    WebHub h;
    registerWebDecoders(h);
    std::string c, w;
    AdmitRequest a; a.ip = "10.0.0.1";
    const int u1 = h.admit(a, c, w);
    a.ip = "10.0.0.2";
    const int u2 = h.admit(a, c, w);
    std::vector<cf32> x(48000);
    auto push = [&](double seconds) { for (size_t n = 0; n < (size_t)(rate * seconds); n += x.size()) { syn->generate(x.data(), x.size()); h.pushIq(x.data(), x.size(), 1090e6, rate); h.processPending(); } };
    push(0.05);
    h.handle(u1, msg("{\"type\":\"subscribe\",\"decoder\":\"adsb\",\"freq_hz\":1090000000}"));
    h.handle(u2, msg("{\"type\":\"subscribe\",\"decoder\":\"adsb\",\"freq_hz\":1090000400}"));   // the same kHz: one shared instance
    CHECK(h.statsJson().find("\"listeners\":2") != std::string::npos, "the two listeners share one ADS-B decoder: %s", h.statsJson().c_str());
    h.start();
    push(12);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    h.stop();
    Drained d1 = drainAll(h, u1), d2 = drainAll(h, u2);
    std::string last;
    for (const auto& t : d1.text) if (t.find("\"decoder\":\"adsb@1090000k\"") != std::string::npos) last = t;
    CHECK(!last.empty() && validJson(last), "an ADS-B event reached the listener");
    CHECK(numField(last, "aircraft_count") >= 3 && last.find("\"lat\":") != std::string::npos && last.find("\"icao\":\"") != std::string::npos,
          "aircraft with positions decoded: %s", last.substr(0, 400).c_str());
    bool both = false;
    for (const auto& t : d2.text) if (t.find("\"type\":\"aircraft\"") != std::string::npos) both = true;
    CHECK(both, "the second listener got the same events");
    // a late listener gets the current table at once
    a.ip = "10.0.0.3";
    const int u3 = h.admit(a, c, w);
    drainAll(h, u3);
    h.handle(u3, msg("{\"type\":\"subscribe\",\"decoder\":\"adsb\",\"freq_hz\":1090000000}"));
    bool snap = false;
    for (const auto& t : drainAll(h, u3).text) if (t.find("\"type\":\"aircraft\"") != std::string::npos && t.find("\"aircraft_count\":") != std::string::npos) snap = true;
    CHECK(snap, "a late listener gets the current table when it subscribes");
    h.leave(u1); h.leave(u2); h.leave(u3);
}

int main() {
    testText();
    testEncoders();
    testUsbAudio();
    testImageStreamer();
    testEventJson();
    testPolicySide();
    testPolicyOff();
    testEveryDecoderOnNoise();
    testAdsbEndToEnd();
    return testSummary("web_decoders");
}
