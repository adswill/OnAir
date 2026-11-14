// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SondeHub upload (dect2/net/sondehub.h): the names SondeHub expects for every sonde type OnAir decodes, the telemetry and listener JSON
// (field names, units, what is left out, the rules that keep a frame back), the times, gzip (checked with stb_image's own inflate and the
// CRC-32 check value), the batch interval and duplicates, the answer text, and the upload thread against a MOCK HTTP server on localhost
// (never the real SondeHub): the listener PUT, the gzip PUT with its headers, a 500 retried, a 400 shown and not retried, nothing while off.
#include "dect2/net/sondehub.h"
#include "dect2/common/timecompat.h"
#include "../../core/src/net/tcp_client.h"
#include "../common/test_util.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace dect2;

// ---- a small strict JSON checker (independent of the code under test) ----
struct J {
    const std::string& s; size_t p = 0;
    explicit J(const std::string& x) : s(x) {}
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) p++; }
    bool lit(const char* l) { const size_t n = std::strlen(l); if (s.compare(p, n, l) == 0) { p += n; return true; } return false; }
    bool str() {
        if (p >= s.size() || s[p] != '"') return false;
        for (p++; p < s.size(); p++) {
            const unsigned char c = (unsigned char)s[p];
            if (c == '"') { p++; return true; }
            if (c < 0x20) return false;
            if (c == '\\') { p++; if (p >= s.size()) return false; if (s[p] == 'u') { if (p + 4 >= s.size()) return false; p += 4; } else if (!std::strchr("\"\\/bfnrt", s[p])) return false; }
        }
        return false;
    }
    bool number() {
        const size_t b = p;
        if (p < s.size() && s[p] == '-') p++;
        while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '+' || s[p] == '-')) p++;
        return p > b && std::isdigit((unsigned char)s[p - 1]);
    }
    bool value() {
        ws();
        if (p >= s.size()) return false;
        bool ok;
        if (s[p] == '{') {
            p++; ws();
            if (p < s.size() && s[p] == '}') { p++; return true; }
            for (;;) { ws(); if (!str()) return false; ws(); if (p >= s.size() || s[p] != ':') return false; p++; if (!value()) return false; ws(); if (p < s.size() && s[p] == ',') { p++; continue; } if (p < s.size() && s[p] == '}') { p++; return true; } return false; }
        } else if (s[p] == '[') {
            p++; ws();
            if (p < s.size() && s[p] == ']') { p++; return true; }
            for (;;) { if (!value()) return false; ws(); if (p < s.size() && s[p] == ',') { p++; continue; } if (p < s.size() && s[p] == ']') { p++; return true; } return false; }
        } else if (s[p] == '"') ok = str();
        else ok = lit("true") || lit("false") || lit("null") || number();
        return ok;
    }
    bool valid() { const bool v = value(); ws(); return v && p == s.size(); }
};
static bool validJson(const std::string& s) { J j(s); return j.valid(); }
static bool has(const std::string& s, const std::string& x) { return s.find(x) != std::string::npos; }
// the text of a top-level field: "\"alt\":1234.5" -> "1234.5"
static std::string field(const std::string& js, const std::string& key) {
    const std::string k = "\"" + key + "\":";
    size_t p = js.find(k);
    if (p == std::string::npos) return "<none>";
    p += k.size();
    size_t e = p;
    if (js[p] == '"') { e = js.find('"', p + 1); return js.substr(p + 1, e - p - 1); }
    if (js[p] == '[') { e = js.find(']', p); return js.substr(p, e - p + 1); }
    while (e < js.size() && js[e] != ',' && js[e] != '}') e++;
    return js.substr(p, e - p);
}

static SondeInfo rs41(double t) {
    SondeInfo s;
    s.kind = SondeRs41; s.type = "RS41"; s.subtype = "RS41-SGP"; s.serial = "S1234567";
    s.frame = 4321; s.hasTime = true; s.unixTime = t;
    s.hasPos = true; s.lat = 52.81523; s.lon = 9.92481; s.altM = 12345.67;
    s.hasVel = true; s.vSpeed = 5.12; s.hSpeed = 14.3; s.headingDeg = -10;
    s.sats = 9; s.hasTemp = true; s.tempC = -52.34; s.hasHumidity = true; s.humidity = 3.0; s.hasPressure = true; s.pressureHpa = 187.234;
    s.batteryV = 2.9; s.burstKillS = 30000; s.freqHz = 403.5005e6; s.snrDb = 18.4f;
    return s;
}

static void testIdentify() {
    sondehub::Ident id; std::string why;
    SondeInfo s = rs41(1.7e9);
    CHECK(sondehub::identify(s, id) && id.manufacturer == "Vaisala" && id.type == "RS41" && id.subtype == "RS41-SGP" && id.serial == "S1234567" && id.frame == 4321 && !id.slow, "RS41");
    s.kind = SondeRs92; s.type = "RS92"; s.subtype = "RS92-SGP"; s.serial = "P1234567";
    CHECK(sondehub::identify(s, id) && id.type == "RS92" && id.subtype == "RS92-SGP" && id.serial == "P1234567", "RS92");
    s.kind = SondeDfm; s.type = "DFM"; s.subtype = "DFM-17"; s.serial = "21034567"; s.frame = 12; s.unixTime = 1700000000.6;
    CHECK(sondehub::identify(s, id) && id.manufacturer == "Graw" && id.type == "DFM" && id.subtype == "DFM17" && id.serial == "21034567" && id.frame == 1700000000 && id.slow, "DFM: subtype DFM17, frame = sonde time");
    s.kind = SondeM10; s.type = "M10"; s.subtype = "M10+"; s.serial = "802-2-11234"; s.frame = 5;
    CHECK(sondehub::identify(s, id) && id.manufacturer == "Meteomodem" && id.type == "M10" && id.subtype.empty() && id.serial == "802-2-11234" && id.frame == 1700000000, "M10: type M10, frame = time");
    s.kind = SondeM20; s.type = "M20"; s.subtype = "M20"; s.serial = "202-3-01234"; s.frame = -1;
    CHECK(sondehub::identify(s, id) && id.type == "M20" && id.frame == 1700000000, "M20");
    s.kind = SondeLms6; s.type = "LMS6"; s.subtype = "LMS6-1680"; s.serial = "12345"; s.frame = 77; s.freqHz = 403.0e6;
    CHECK(sondehub::identify(s, id) && id.manufacturer == "Lockheed Martin" && id.type == "LMS6-400" && id.serial == "12345" && id.frame == 77, "LMS6 at 403 MHz is LMS6-400 (%s)", id.type.c_str());
    s.freqHz = 1680.0e6;
    CHECK(sondehub::identify(s, id) && id.type == "LMS6-1680", "LMS6 at 1680 MHz");
    s.freqHz = 0;
    CHECK(sondehub::identify(s, id) && id.type == "LMS6-1680", "LMS6 without a frequency: the decoder's subtype");
    s.kind = SondeImet54; s.type = "iMet-54"; s.subtype = "iMet-54"; s.serial = "iMet54-55012345";
    CHECK(sondehub::identify(s, id) && id.manufacturer == "Intermet Systems" && id.type == "iMet-54" && id.serial == "55012345", "iMet-54 serial (%s)", id.serial.c_str());
    s.kind = SondeMrz; s.type = "MRZ"; s.subtype = "MRZ-N1 (lat/lon)"; s.serial = "MRZ-5667-39155";
    CHECK(sondehub::identify(s, id) && id.manufacturer == "Meteo-Radiy" && id.type == "MRZ" && id.subtype == "MRZ-N1" && id.serial == "5667-39155", "MRZ");
    s.serial = "MRZ-?-39155";
    CHECK(!sondehub::identify(s, id, &why) && has(why, "not known"), "MRZ with a half-known serial is not sent");
    s.kind = SondeMeisei; s.type = "iMS-100"; s.subtype = "iMS-100"; s.serial = "1234567";
    CHECK(sondehub::identify(s, id) && id.manufacturer == "Meisei" && id.type == "iMS-100" && id.serial == "1234567" && id.slow, "iMS-100");
    s.kind = SondeImet; s.type = "iMet"; s.serial = "iMet-403000";
    CHECK(!sondehub::identify(s, id, &why) && has(why, "iMet-1/4"), "iMet-1/4 not sent");
    s.kind = SondeRs41; s.serial = "";
    CHECK(!sondehub::identify(s, id, &why), "no serial: not sent");
    s.serial = "S12\"34";
    CHECK(!sondehub::identify(s, id, &why), "serial with a quote: not sent");
    s.kind = SondeUnknown; s.serial = "X1";
    CHECK(!sondehub::identify(s, id), "unknown type: not sent");
}

static void testTimes() {
    CHECK(sondehub::isoTime(1700000000.0) == "2023-11-14T22:13:20.000000Z", "iso %s", sondehub::isoTime(1700000000.0).c_str());
    CHECK(sondehub::isoTime(1700000000.25) == "2023-11-14T22:13:20.250000Z", "iso fraction %s", sondehub::isoTime(1700000000.25).c_str());
    CHECK(sondehub::isoTime(946684799.9999999) == "1999-12-31T23:59:59.999999Z", "iso rounding stays in the second (%s)", sondehub::isoTime(946684799.9999999).c_str());
    CHECK(sondehub::httpDate(1700000000.0) == "Tue, 14 Nov 2023 22:13:20 GMT", "http date %s", sondehub::httpDate(1700000000.0).c_str());
    CHECK(sondehub::httpDate(1709251200.0) == "Fri, 01 Mar 2024 00:00:00 GMT", "http date leap year %s", sondehub::httpDate(1709251200.0).c_str());
}

static void testTelemetry() {
    const double now = 1700000000.0;
    SondeHubConfig c; c.call = "DL1ABC"; c.antenna = "5/8 \"vertical\"";
    SondeHubStation st; st.set = true; st.lat = 52.5; st.lon = 13.4; st.altM = 40;
    std::string why;
    const std::string js = sondehub::telemetryJson(c, st, rs41(now - 1.5), now, &why);
    CHECK(!js.empty() && validJson(js), "valid JSON: %s %s", js.c_str(), why.c_str());
    const char* mand[] = {"software_name", "software_version", "uploader_callsign", "time_received", "manufacturer", "type", "serial", "frame", "datetime", "lat", "lon", "alt"};
    for (const char* k : mand) CHECK(field(js, k) != "<none>", "mandatory field %s", k);
    CHECK(field(js, "software_name") == "OnAir" && field(js, "software_version") == sondehub::softwareVersion(), "OnAir names itself");
    CHECK(field(js, "uploader_callsign") == "DL1ABC" && field(js, "manufacturer") == "Vaisala" && field(js, "type") == "RS41" && field(js, "subtype") == "RS41-SGP", "names");
    CHECK(field(js, "time_received") == "2023-11-14T22:13:20.000000Z" && field(js, "datetime") == "2023-11-14T22:13:18.500000Z", "times: received from the clock, datetime from the sonde");
    CHECK(field(js, "frame") == "4321", "frame");
    CHECK(std::fabs(std::atof(field(js, "lat").c_str()) - 52.81523) < 1e-6 && std::fabs(std::atof(field(js, "lon").c_str()) - 9.92481) < 1e-6, "lat/lon decimal degrees");
    CHECK(field(js, "alt") == "12345.7", "alt metres (%s)", field(js, "alt").c_str());
    CHECK(field(js, "vel_v") == "5.12" && field(js, "vel_h") == "14.30" && field(js, "heading") == "350.0", "velocities m/s, heading 0-360 (%s %s %s)", field(js, "vel_v").c_str(), field(js, "vel_h").c_str(), field(js, "heading").c_str());
    CHECK(field(js, "temp") == "-52.3" && field(js, "humidity") == "3.0" && field(js, "pressure") == "187.23", "PTU units");
    CHECK(field(js, "sats") == "9" && field(js, "batt") == "2.90" && field(js, "burst_timer") == "30000", "sats, batt, burst timer");
    CHECK(field(js, "frequency") == "403.5005", "frequency in MHz (%s)", field(js, "frequency").c_str());
    CHECK(field(js, "snr") == "18.4", "snr dB");
    CHECK(field(js, "uploader_position") == "[52.50000,13.40000,40.0]", "uploader position list (%s)", field(js, "uploader_position").c_str());
    CHECK(has(js, "\"uploader_antenna\":\"5/8 \\\"vertical\\\"\""), "antenna escaped: %s", js.c_str());
    CHECK(!has(js, "rssi") && !has(js, "xdata") && !has(js, "tx_frequency"), "fields OnAir does not have are left out");
    // privacy: no position without the box, none when not set
    SondeHubConfig np = c; np.sharePos = false;
    CHECK(!has(sondehub::telemetryJson(np, st, rs41(now), now), "uploader_position"), "no position without Send my position");
    SondeHubStation unset;
    CHECK(!has(sondehub::telemetryJson(c, unset, rs41(now), now), "uploader_position"), "no position when the station is not set");
    // left out when the sonde did not send it
    SondeInfo m = rs41(now); m.hasTemp = m.hasHumidity = m.hasPressure = m.hasVel = false; m.sats = -1; m.batteryV = -1; m.burstKillS = -1; m.freqHz = 0; m.snrDb = 0; m.subtype = "";
    const std::string jm = sondehub::telemetryJson(c, st, m, now);
    CHECK(validJson(jm) && !has(jm, "temp") && !has(jm, "humidity") && !has(jm, "pressure") && !has(jm, "vel_v") && !has(jm, "sats") && !has(jm, "batt") && !has(jm, "frequency") && !has(jm, "subtype") && !has(jm, "burst_timer"), "optional fields left out: %s", jm.c_str());
    // the rules
    auto refused = [&](SondeInfo s, const SondeHubConfig& cc, const char* expect) {
        std::string w; const std::string j = sondehub::telemetryJson(cc, st, s, now, &w);
        CHECK(j.empty() && has(w, expect), "refused for '%s' (got '%s')", expect, w.c_str());
    };
    SondeInfo b = rs41(now); b.hasPos = false; refused(b, c, "no GPS position");
    b = rs41(now); b.lat = 0; b.lon = 0; refused(b, c, "no GPS position");
    b = rs41(now); b.lat = 91; refused(b, c, "no GPS position");
    b = rs41(now); b.sats = 3; refused(b, c, "3 satellites");
    b = rs41(now); b.altM = 60000; refused(b, c, "altitude");
    b = rs41(now); b.hasTime = false; refused(b, c, "date and time");
    b = rs41(now - 7200); refused(b, c, "an hour");
    b = rs41(now + 4000); refused(b, c, "an hour");
    SondeHubConfig nc = c; nc.call = "  "; refused(rs41(now), nc, "callsign");
    b = rs41(now); b.kind = SondeImet; refused(b, c, "iMet-1/4");
    SondeInfo four = rs41(now); four.sats = 4;
    CHECK(!sondehub::telemetryJson(c, st, four, now).empty(), "4 satellites are a fix");
    // listener
    const std::string lj = sondehub::listenerJson(c, st);
    CHECK(validJson(lj) && field(lj, "uploader_position") == "[52.50000,13.40000,40.0]" && field(lj, "mobile") == "false" && field(lj, "software_name") == "OnAir", "listener %s", lj.c_str());
    CHECK(!has(lj, "uploader_contact_email"), "no e-mail unless given");
    SondeHubConfig mc = c; mc.mobile = true; mc.email = "a@b.c";
    const std::string lm = sondehub::listenerJson(mc, st);
    CHECK(field(lm, "mobile") == "true" && field(lm, "uploader_contact_email") == "a@b.c", "chase car listener");
    const std::string ln = sondehub::listenerJson(np, st);
    CHECK(validJson(ln) && has(ln, "\"uploader_position\":null"), "position null when not shared (%s)", ln.c_str());
    CHECK(sondehub::jsonArray({}) == "[]" && validJson(sondehub::jsonArray({js, jm})), "array");
    CHECK(sondehub::validUploader("DL1ABC") && !sondehub::validUploader("AB") && !sondehub::validUploader(std::string("A\nBC")), "uploader name rule");
}

static std::string gunzip(const std::string& g, bool& ok) {
    ok = false;
    if (g.size() < 18 || (uint8_t)g[0] != 0x1f || (uint8_t)g[1] != 0x8b || g[2] != 8 || g[3] != 0) return {};
    int n = 0;
    char* out = stbi_zlib_decode_noheader_malloc(g.data() + 10, (int)g.size() - 18, &n);
    if (!out) return {};
    std::string s(out, (size_t)n); free(out);
    auto le = [&](size_t at) { return (uint32_t)(uint8_t)g[at] | (uint32_t)(uint8_t)g[at + 1] << 8 | (uint32_t)(uint8_t)g[at + 2] << 16 | (uint32_t)(uint8_t)g[at + 3] << 24; };
    ok = le(g.size() - 8) == sondehub::crc32(s.data(), s.size()) && le(g.size() - 4) == (uint32_t)s.size();
    return s;
}

static void testGzip() {
    CHECK(sondehub::crc32("123456789", 9) == 0xCBF43926u, "CRC-32 check value %08X", sondehub::crc32("123456789", 9));
    std::vector<std::string> objs;
    SondeHubConfig c; c.call = "DL1ABC"; SondeHubStation st; st.set = true; st.lat = 52.5; st.lon = 13.4;
    for (int i = 0; i < 40; i++) { SondeInfo s = rs41(1700000000.0 + i); s.frame = 4000 + i; s.altM = 1000 + 5 * i; objs.push_back(sondehub::telemetryJson(c, st, s, 1700000000.0 + i)); }
    const std::string js = sondehub::jsonArray(objs);
    const std::string gz = sondehub::gzip(js);
    bool ok = false;
    const std::string back = gunzip(gz, ok);
    CHECK(ok && back == js, "gzip round trip with CRC and size (%zu -> %zu bytes)", js.size(), gz.size());
    CHECK(gz.size() * 4 < js.size(), "40 frames compress at least 4 times (%zu -> %zu)", js.size(), gz.size());
    const std::string e = sondehub::gzip("");
    bool eok = false;
    CHECK(gunzip(e, eok).empty() && eok, "empty input is a valid gzip member");
    const std::string one = sondehub::gzip("x");
    bool ook = false;
    CHECK(gunzip(one, ook) == "x" && ook, "one byte");
}

static void testBatch() {
    sondehub::Batch b;
    CHECK(!b.due(0, 15), "empty batch never due");
    CHECK(b.add("S1", 1, "{}", false, 100.0) && b.add("S1", 2, "{}", false, 100.5) && !b.add("S1", 2, "{}", false, 101) && b.duplicates == 1, "a repeated frame number is a duplicate");
    CHECK(b.add("S2", 2, "{}", false, 101), "same frame number, other sonde: kept");
    CHECK(!b.due(114.9, 15) && b.due(115.0, 15), "due 15 s after the first frame");
    CHECK(b.intervalS(15) == 15, "plain interval");
    const auto v = b.take(115);
    CHECK(v.size() == 3 && b.size() == 0, "take gives all");
    CHECK(b.add("D1", 1700000000, "{}", true, 116) && b.intervalS(15) == 22 && !b.due(115 + 21.9, 15) && b.due(115 + 22, 15), "DFM in the batch: 1.5 x 15 = 22 s");
    CHECK(b.intervalS(30) == 30 && b.intervalS(60) == 60 && b.intervalS(10) == 15, "with DFM: at most 30 s, never shorter than the interval (%g %g %g)", b.intervalS(30), b.intervalS(60), b.intervalS(10));
    b.take(140);
    CHECK(b.intervalS(15) == 15, "the longer interval ends with the batch");
    CHECK(b.add("S1", 3, "{}", false, 141) && !b.due(154.9, 15) && b.due(155, 15), "the next interval counts from the last upload");
    sondehub::Batch f;
    size_t n = 0;
    for (size_t i = 0; i < sondehub::Batch::kMax + 10; i++) n += f.add("S", (long long)i, "{}", false, 0) ? 1 : 0;
    CHECK(n == sondehub::Batch::kMax && f.overflow == 10, "the queue is bounded");
}

static void testAnswer() {
    const std::string a = "{\"message\":\"some issues\",\"errors\":[{\"error_message\":\"z-check failed for DFM 123\",\"payload\":{}}],\"warnings\":[{\"warning_message\":\"old software\"}]}";
    const std::string t = sondehub::answerText(a);
    CHECK(has(t, "some issues") && has(t, "z-check failed for DFM 123") && has(t, "old software"), "answer text: %s", t.c_str());
    CHECK(sondehub::answerText("Bad request\r\n") == "Bad request", "plain text answer");
    CHECK(sondehub::answerText(std::string(1000, 'x')).size() < 420, "long answers are cut");
}

static void testReply() {
    const std::string rej = "{\"message\":\"some or all payloads could not be processed\",\"errors\":[{\"error_message\":\"Serial number is not valid for DFM\",\"payload\":{\"serial\":\"123\",\"type\":\"DFM\"}},{\"error_message\":\"z-check failed - payload GPS may not be valid for M3423108.\",\"payload\":{\"serial\":\"M3423108\"}}]}";
    const sondehub::Reply r = sondehub::analyseReply(rej);
    CHECK(r.rejected == 1 && r.zcheck == 1 && r.serials.size() == 2 && r.serials[0] == "123" && has(r.text, "not valid"), "reply: 1 refused, 1 z-check (%d %d)", r.rejected, r.zcheck);
    const sondehub::Reply ok = sondehub::analyseReply("Telemetry Saved into Database Successfuly");
    CHECK(ok.rejected == 0 && ok.zcheck == 0 && ok.serials.empty(), "a plain 200 text has no errors");
}

// every sonde type OnAir decodes: the JSON has the mandatory fields with the types of SondeHub's swagger (strings, integer frame, numbers)
static void testSchemaPerType() {
    const double now = 1700000000.0;
    SondeHubConfig c; c.call = "DL1ABC";
    SondeHubStation st; st.set = true; st.lat = 52.5; st.lon = 13.4; st.altM = 40;
    struct T { SondeKind k; const char* type; const char* sub; const char* ser; const char* man; const char* wtype; const char* wser; };
    const T ts[] = {
        {SondeRs41, "RS41", "RS41-SG", "T1234567", "Vaisala", "RS41", "T1234567"},
        {SondeRs92, "RS92", "RS92-SGP", "M1234567", "Vaisala", "RS92", "M1234567"},
        {SondeDfm, "DFM", "DFM-09", "21034567", "Graw", "DFM", "21034567"},
        {SondeM10, "M10", "M10", "123-4-56789", "Meteomodem", "M10", "123-4-56789"},
        {SondeM20, "M20", "M20", "123-4-56789", "Meteomodem", "M20", "123-4-56789"},
        {SondeLms6, "LMS6", "LMS6", "LMS6-12345", "Lockheed Martin", "LMS6-400", "12345"},
        {SondeImet54, "iMet-54", "iMet-54", "iMet54-55012345", "Intermet Systems", "iMet-54", "55012345"},
        {SondeMrz, "MRZ", "MRZ-N1", "MRZ-5667-39155", "Meteo-Radiy", "MRZ", "5667-39155"},
        {SondeMeisei, "iMS-100", "iMS-100", "1234567", "Meisei", "iMS-100", "1234567"},
    };
    for (const T& t : ts) {
        SondeInfo s = rs41(now - 1); s.kind = t.k; s.type = t.type; s.subtype = t.sub; s.serial = t.ser; s.freqHz = 403.5e6;
        std::string why;
        const std::string js = sondehub::telemetryJson(c, st, s, now, &why);
        CHECK(!js.empty() && validJson(js), "%s: JSON (%s)", t.type, why.c_str());
        for (const char* k : {"software_name", "software_version", "uploader_callsign", "time_received", "manufacturer", "type", "serial", "datetime"})
            CHECK(has(js, std::string("\"") + k + "\":\""), "%s: %s is a string", t.type, k);
        for (const char* k : {"frame", "lat", "lon", "alt"}) {
            const std::string v = field(js, k);
            CHECK(v != "<none>" && !v.empty() && v[0] != '"' && (std::isdigit((unsigned char)v[0]) || v[0] == '-'), "%s: %s is a number (%s)", t.type, k, v.c_str());
        }
        CHECK(field(js, "frame").find('.') == std::string::npos, "%s: frame is an integer", t.type);
        for (const char* k : {"time_received", "datetime"}) {
            const std::string v = field(js, k);
            CHECK(v.size() == 27 && v[10] == 'T' && v[19] == '.' && v.back() == 'Z', "%s: %s is ISO 8601 UTC with microseconds (%s)", t.type, k, v.c_str());
        }
        CHECK(field(js, "manufacturer") == t.man && field(js, "type") == t.wtype && field(js, "serial") == t.wser, "%s: names %s/%s/%s", t.type, field(js, "manufacturer").c_str(), field(js, "type").c_str(), field(js, "serial").c_str());
        CHECK(field(js, "sats").find('.') == std::string::npos && std::atof(field(js, "frequency").c_str()) > 100 && std::atof(field(js, "frequency").c_str()) < 2000, "%s: sats integer, frequency in MHz", t.type);
        const std::string up = field(js, "uploader_position");
        CHECK(up.front() == '[' && std::count(up.begin(), up.end(), ',') == 2, "%s: uploader_position has 3 numbers", t.type);
        CHECK(js.front() == '{' && js.back() == '}' && validJson(sondehub::jsonArray({js})), "%s: goes out as an array of objects", t.type);
    }
}

// ---- the mock SondeHub ----
struct Req { std::string method, path, body; std::vector<std::pair<std::string, std::string>> headers; std::string header(const std::string& k) const { for (auto& h : headers) { std::string a = h.first; for (auto& c : a) c = (char)std::tolower((unsigned char)c); if (a == k) return h.second; } return {}; } };
struct Mock {
    sock_t ls = kBadSock; int port = 0;
    std::mutex mu; std::vector<Req> reqs;
    std::function<std::pair<int, std::string>(const Req&, int)> answer;   // (request, how many to this path so far)
    std::atomic<bool> quit{false};
    std::thread th;
    bool start() {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 8) != 0) return false;
        socklen_t l = sizeof a; getsockname(ls, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        sockSetNonBlocking(ls, true);
        th = std::thread([this] { run(); });
        return true;
    }
    void run() {
        while (!quit.load()) {
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            sockSetNonBlocking(c, false);
            sockRecvTimeout(c, 2000);
            std::string in; char b[4096];
            size_t hend = std::string::npos, need = 0;
            while (true) {
                const long long k = sockRecv(c, b, sizeof b);
                if (k <= 0) break;
                in.append(b, (size_t)k);
                if (hend == std::string::npos && (hend = in.find("\r\n\r\n")) != std::string::npos) {
                    const size_t cl = in.find("Content-Length: ");
                    need = cl != std::string::npos && cl < hend ? (size_t)std::atol(in.c_str() + cl + 16) : 0;
                }
                if (hend != std::string::npos && in.size() >= hend + 4 + need) break;
            }
            Req r;
            if (hend != std::string::npos) {
                const std::string head = in.substr(0, hend);
                const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
                r.method = head.substr(0, sp1); r.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
                size_t p = head.find("\r\n");
                while (p != std::string::npos) {
                    const size_t e = head.find("\r\n", p + 2);
                    const std::string ln = head.substr(p + 2, e == std::string::npos ? std::string::npos : e - p - 2);
                    const size_t col = ln.find(": ");
                    if (col != std::string::npos) r.headers.push_back({ln.substr(0, col), ln.substr(col + 2)});
                    p = e;
                }
                r.body = in.substr(hend + 4, need);
            }
            int nth = 0;
            { std::lock_guard<std::mutex> l(mu); for (auto& q : reqs) nth += q.path == r.path; reqs.push_back(r); }
            const auto ans = answer ? answer(r, nth) : std::make_pair(200, std::string("OK"));
            const std::string out = "HTTP/1.1 " + std::to_string(ans.first) + " X\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(ans.second.size()) + "\r\nConnection: close\r\n\r\n" + ans.second;
            sockSendAll(c, out.data(), out.size());
            sockClose(c);
        }
    }
    std::vector<Req> got() { std::lock_guard<std::mutex> l(mu); return reqs; }
    void stop() { quit = true; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }
};

static bool waitFor(const std::function<bool()>& f, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}

static void testUploader() {
    netInit();
    Mock m;
    if (!m.start()) { CHECK(false, "mock server"); return; }
    m.answer = [](const Req& r, int nth) -> std::pair<int, std::string> {
        if (r.path == "/sondes/telemetry" && nth == 0) return {500, "{\"message\":\"busy\"}"};   // the first upload: busy once, then fine
        if (r.path == "/sondes/telemetry" && nth == 3) return {202, "{\"message\":\"some or all payloads could not be processed\",\"errors\":[{\"error_message\":\"Serial number invalid\",\"payload\":{\"serial\":\"S1234567\"}},{\"error_message\":\"Altitude = 0\",\"payload\":{\"serial\":\"S1234567\"}}]}"};
        if (r.path == "/sondes/telemetry" && nth == 2) return {400, "{\"message\":\"bad data\",\"errors\":[{\"error_message\":\"serial wrong\"}]}"};
        return {200, "{\"message\":\"ok\"}"};
    };
    SondeHub& h = SondeHub::get();
    SondeHubTiming t; t.uploadS = 1; t.retryWaitS = 0.1;
    h.setTiming(t);
    SondeHubConfig c; c.call = "DL1ABC"; c.server = "http://127.0.0.1:" + std::to_string(m.port); c.antenna = "Yagi";
    SondeHubStation st; st.set = true; st.lat = 52.8; st.lon = 9.9; st.altM = 60;
    h.setStation(st);
    // off: nothing at all
    h.configure(c);   // c.enabled is false
    for (int i = 0; i < 5; i++) h.frame(rs41((double)unixNow()), (double)unixNow());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(m.got().empty() && !h.wanted() && h.status().state == "off", "nothing is sent while off");
    c.enabled = true;
    h.configure(c);
    CHECK(waitFor([&] { for (auto& r : m.got()) if (r.path == "/listeners") return true; return false; }, 4000), "the station record is sent at the start");
    const double now = (double)unixNow();
    for (int i = 0; i < 6; i++) { SondeInfo s = rs41(now - 5 + i); s.frame = 100 + i; h.frame(s, now); }
    SondeInfo dup = rs41(now); dup.frame = 105; h.frame(dup, now);   // a repeat of frame 105
    SondeInfo bad = rs41(now); bad.sats = 2; bad.frame = 200; h.frame(bad, now);
    CHECK(waitFor([&] { return h.status().sent == 6; }, 8000), "6 frames taken after a 500 and a retry (sent %llu)", (unsigned long long)h.status().sent);
    const auto rq = m.got();
    int telem = 0;
    for (const Req& r : rq) {
        if (r.path == "/listeners") {
            CHECK(r.method == "PUT" && r.header("content-encoding").empty() && r.header("content-type") == "application/json", "listener: PUT, plain JSON");
            CHECK(validJson(r.body) && field(r.body, "uploader_callsign") == "DL1ABC" && field(r.body, "mobile") == "false" && field(r.body, "uploader_antenna") == "Yagi", "listener body %s", r.body.c_str());
        }
        if (r.path == "/sondes/telemetry") {
            telem++;
            CHECK(r.method == "PUT" && r.header("content-encoding") == "gzip" && r.header("content-type") == "application/json", "telemetry: PUT gzip JSON");
            CHECK(r.header("user-agent") == sondehub::userAgent() && has(r.header("user-agent"), "OnAir-"), "user agent %s", r.header("user-agent").c_str());
            const std::string d = r.header("date");
            CHECK(d.size() == 29 && d.compare(d.size() - 4, 4, " GMT") == 0 && d[3] == ',', "Date header RFC 7231: '%s'", d.c_str());
            bool ok = false;
            const std::string js = gunzip(r.body, ok);
            CHECK(ok && validJson(js) && js[0] == '[', "body gunzips to a JSON array");
            size_t n = 0; for (size_t p = 0; (p = js.find("\"serial\":", p)) != std::string::npos; p++) n++;
            CHECK(n == 6, "6 frames in the upload (duplicate and the bad fix left out): %zu", n);
        }
    }
    CHECK(telem == 2, "one 500 and one retry (%d telemetry requests)", telem);
    SondeHubStatus s = h.status();
    CHECK(s.reconnects == 1 && s.uploads == 1 && s.skipped == 1 && has(s.skipWhy, "satellites") && s.lastError.empty(), "status: 1 retry, 1 upload, 1 kept back (%llu %llu %llu '%s' '%s')", (unsigned long long)s.reconnects, (unsigned long long)s.uploads, (unsigned long long)s.skipped, s.skipWhy.c_str(), s.lastError.c_str());
    CHECK(has(s.lastLine, "6 frames of 1 sonde") && has(s.lastLine, "HTTP 200"), "last line: %s", s.lastLine.c_str());
    CHECK(has(h.lastJson(), "\"frame\":100") && validJson(h.lastJson()), "what was sent is kept for the panel");
    // a 400: shown, not retried, the frames counted as dropped
    for (int i = 0; i < 3; i++) { SondeInfo x = rs41(now + i); x.frame = 300 + i; h.frame(x, now); }
    CHECK(waitFor([&] { return h.status().dropped == 3; }, 6000), "a refused upload drops its frames (dropped %llu)", (unsigned long long)h.status().dropped);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    s = h.status();
    int t3 = 0; for (auto& r : m.got()) t3 += r.path == "/sondes/telemetry";
    CHECK(t3 == 3 && has(s.lastError, "HTTP 400") && has(s.lastError, "serial wrong"), "400 not retried and its text shown (%d, %s)", t3, s.lastError.c_str());
    // a 202 that refuses 2 of the 3 frames: those are not counted as sent, the reply is kept and shown
    const uint64_t sentBefore = h.status().sent;
    for (int i = 0; i < 3; i++) { SondeInfo x = rs41(now + i); x.frame = 500 + i; h.frame(x, now); }
    CHECK(waitFor([&] { return h.status().rejected == 2; }, 6000), "a 202 with errors counts the refused frames (rejected %llu)", (unsigned long long)h.status().rejected);
    s = h.status();
    CHECK(s.sent == sentBefore + 1 && has(s.lastError, "refused 2 of 3") && has(s.lastError, "S1234567") && has(s.lastError, "Serial number invalid") && has(s.lastReply, "some or all payloads"), "reported to the user: %s", s.lastError.c_str());
    // off again: frames are ignored
    c.enabled = false; h.configure(c);
    const size_t before = m.got().size();
    for (int i = 0; i < 3; i++) { SondeInfo x = rs41(now + i); x.frame = 400 + i; h.frame(x, now); }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(m.got().size() == before, "nothing after turning it off");
    // chase car: the live position every chaseEveryS as mobile
    t.chaseEveryS = 0.3; h.setTiming(t);
    c.enabled = true; c.mobile = true; h.configure(c);
    st.live = true; h.setStation(st);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    int mob = 0; for (auto& r : m.got()) if (r.path == "/listeners" && field(r.body, "mobile") == "true") mob++;
    CHECK(mob >= 2 && mob <= 6, "chase car position repeated (%d in 1.4 s at 0.3 s)", mob);
    h.stop();
    m.stop();
}

int main() {
    testIdentify();
    testTimes();
    testTelemetry();
    testGzip();
    testBatch();
    testAnswer();
    testReply();
    testSchemaPerType();
    testUploader();
    return testSummary("sonde_hub");
}
