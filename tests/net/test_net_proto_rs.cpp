// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the network protocol layers (dect2::netp, the Rust crate onair_net): SHA-1 / base64 / WebSocket accept key and server frames, client WebSocket
// frames (valid, unmasked, fragmented, odd opcodes, every length class, cut short), URL text (percent decoding, query strings, Host headers, loopback names, constant
// time compare, reason phrases), flat JSON bodies (built from random members, mutated, truncated, odd numbers and escapes), HTTP request heads (request lines, header
// fields, Content-Length, chunked, duplicates, control characters), rtl_tcp greetings / commands / address strings, SpyServer words / commands / headers / rates, the
// SDR++ draw list (random menus, mutated, truncated), labels from ids, rate texts, command packets, and the list of network radios (parse, format, host clean-up).
// ONAIR_DIFF_ITERS (default 100000) inputs per family. Buffers are exactly as long as their content so a sanitizer sees a read past the end. The Rust C ABI is also
// called with null pointers and short arrays. Every result goes into a digest (tests/common/golden.h; doubles rounded to 1e-6) compared with the one recorded from the
// C++ layer the Rust code replaced.
#include "dect2/net/net_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include "dect2/rs/net.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::netp;

static golden::Digest dig;
static long abiFails = 0;
#define CHECK(c, ...) do { if (!(c)) { abiFails++; printf("FAILED: " __VA_ARGS__); printf("\n"); } } while (0)
static std::mt19937 g(4242);
static uint32_t r32() { return g(); }
using Buf = std::vector<uint8_t>;

static std::string rbytes(size_t n) { std::string s(n, '\0'); for (auto& c : s) c = (char)r32(); return s; }
static std::string pick(const char* const* v, int n) { return v[r32() % n]; }
// random text from an alphabet
static std::string rtext(const char* alpha, size_t maxLen) {
    const size_t n = strlen(alpha), len = r32() % (maxLen + 1);
    std::string s;
    for (size_t i = 0; i < len; i++) s += alpha[r32() % n];
    return s;
}
static std::string mutate(std::string s) {
    const int m = (int)(r32() % 5);
    for (int i = 0; i < m && !s.empty(); i++) {
        const size_t p = r32() % s.size();
        switch (r32() % 4) {
        case 0: s[p] = (char)r32(); break;
        case 1: s.erase(p, 1); break;
        case 2: s.insert(p, 1, (char)r32()); break;
        default: s.resize(p); break;
        }
    }
    return s;
}

// ------------------------------------------------------------------------------------------------------------------------------ hash, base64, WebSocket
static void hashFamily(long iters) {
    for (long it = 0; it < iters; it++) {
        std::string d = rbytes(r32() % 3 == 0 ? r32() % 200 : r32() % 70);
        dig.add(A::sha1(d));
        dig.add(A::base64(d));
        dig.add(A::wsAcceptKey(d));
        const int op = (int)(r32() % 20);
        const size_t n = r32() % 50 == 0 ? (size_t)(r32() % 70000) : r32() % 300;
        const std::string p = rbytes(n);
        dig.add(A::wsFrame(op, p));
    }
    printf("  hash / base64 / frames: %ld inputs\n", iters);
}

static std::string clientFrame(bool fin, int op, bool masked, size_t len, int lenClass) {
    std::string f;
    f.push_back((char)((fin ? 0x80 : 0) | (op & 15) | (r32() % 16 == 0 ? (int)(r32() & 0x70) : 0)));
    const uint8_t m = masked ? 0x80 : 0;
    if (lenClass == 0) f.push_back((char)(m | (len & 0x7F)));
    else if (lenClass == 1) { f.push_back((char)(m | 126)); f.push_back((char)(len >> 8)); f.push_back((char)len); }
    else { f.push_back((char)(m | 127)); for (int i = 0; i < 8; i++) f.push_back((char)(len >> (8 * (7 - i)))); }
    if (masked) f += rbytes(4);
    f += rbytes(len);
    return f;
}

static void wsFamily(long iters) {
    long frames = 0, closes = 0, needs = 0;
    for (long it = 0; it < iters; it++) {
        std::string in;
        const int cls = (int)(r32() % 8);
        if (cls < 6) {
            const int lc = (int)(r32() % 10);
            const size_t len = lc < 5 ? r32() % 126 : lc < 8 ? 100 + r32() % 4100 : r32() % 6000;
            in = ORD_CALL(clientFrame, r32() % 8 != 0, (int)(r32() % 16), r32() % 8 != 0, len, len < 126 && lc < 9 ? 0 : (lc == 9 ? 2 : 1));   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
            if (r32() % 4 == 0) in = mutate(in);
            if (r32() % 4 == 0) in += rbytes(r32() % 20);
        } else in = rbytes(r32() % 12);
        const A::WsClient a = A::wsParseClient(in);
        dig.add(a.kind, a.code, a.op, a.fin, a.used, a.payload);
        (a.kind == 2 ? frames : a.kind == 1 ? closes : needs)++;
    }
    printf("  client frames: %ld inputs (%ld frames, %ld close, %ld need more)\n", iters, frames, closes, needs);
}

// ------------------------------------------------------------------------------------------------------------------------------ URL text
static void urlFamily(long iters) {
    static const char* hosts[] = {"localhost", "127.0.0.1", "127.0.0.1:8765", "[::1]", "[::1]:80", "example.com:99", "127.x", "LOCALHOST", "", "[", "[]:", "a:b:c", ":", "127."};
    for (long it = 0; it < iters; it++) {
        const std::string s = rtext("%+ab01&=.:[]/X-_ Ff9eEzZ\x01\xff", 24);
        dig.add(A::urlDecode(s));
        static const char* keys[] = {"token", "a", "b", "", "x y", "k"};
        const std::string key = r32() % 3 ? pick(keys, 6) : rtext("ab%+ ", 5);
        const std::string q = r32() % 2 ? s : "a=1&token=" + rtext("abc%2B+", 8) + "&b&" + s;
        dig.add(A::queryParam(q, key));
        const std::string h = r32() % 2 ? pick(hosts, 14) : rtext("[]:0123456789.alo", 14);
        dig.add(A::hostOnly(h));
        const std::string ho = A::hostOnly(h);
        dig.add(A::loopbackName(h), A::loopbackName(ho));
        const std::string b = r32() % 2 ? h : mutate(h);
        dig.add(A::ctEqual(h, b));
        const int code = (int)(r32() % 700) - 50;
        dig.add(A::statusText(code));
    }
    for (int c = -2; c < 600; c++) dig.add(A::statusText(c));
    printf("  URL text: %ld inputs\n", iters);
}

// ------------------------------------------------------------------------------------------------------------------------------ flat JSON
static std::string jsonNumber() {
    static const char* n[] = {"0", "1", "-1", "474e6", "1.5", ".5", "5.", "-.5e-3", "1e", "1e+", "+5", "--1", "1e999", "1E5", "0x10", "1.2.3", "123456789012345678901234567890123", "00012", "-", ".", "1e-400", "9007199254740993", "4.9e-324"};
    return r32() % 3 ? pick(n, 23) : rtext("0123456789.eE+-", 12);
}
static std::string jsonString() {
    static const char* s[] = {"x", "", "a b", "x\\u00e4", "\\u0041", "\\u20ac", "\\ud83d\\ude00", "\\q", "\\", "\\u12", "\\u12G4", "a\\nb", "\\/\\\\\\\"", "\\b\\f\\r\\t"};
    std::string t = r32() % 3 ? pick(s, 14) : rtext("ab\\\"u0123 \t\x1f\xc3\xa4", 10);
    return "\"" + t + "\"";
}
static std::string jsonObject() {
    static const char* keys[] = {"hz", "telemetry", "spectrum", "bins", "a", "b", "c", "k\\u00e4y", "", "dup"};
    std::string o = "{";
    const int n = r32() % 20 == 0 ? 30 + (int)(r32() % 8) : (int)(r32() % 6);
    for (int i = 0; i < n; i++) {
        if (i) o += r32() % 30 ? "," : "";
        std::string k = n > 20 ? "k" + std::to_string(r32() % 40) : pick(keys, 10);
        o += ORD_CAT((r32() % 10 ? " " : ""), "\"", k, "\"", (r32() % 30 ? ":" : " "), (r32() % 5 ? "" : " "));
        switch (r32() % 5) {
        case 0: o += jsonNumber(); break;
        case 1: o += jsonString(); break;
        case 2: o += r32() % 2 ? "true" : "false"; break;
        case 3: o += r32() % 6 ? "null" : "nul"; break;
        default: o += jsonNumber(); break;
        }
    }
    o += r32() % 40 ? "}" : "";
    return ORD_CAT((r32() % 10 ? "" : " \t\r\n"), o, (r32() % 10 ? "" : " "));
}
static void addJson(const std::map<std::string, dect2::JsonVal>& a) {
    dig.add((uint64_t)a.size());
    for (const auto& kv : a) dig.add(kv.first, (int)kv.second.kind, kv.second.num, kv.second.str, kv.second.b);
}
static void jsonFamily(long iters) {
    long good = 0;
    for (long it = 0; it < iters; it++) {
        std::string t = jsonObject();
        if (r32() % 3 == 0) t = mutate(t);
        std::map<std::string, dect2::JsonVal> a;
        const bool oa = A::parseFlatJson(t, a);
        dig.add(oa); if (oa) addJson(a);
        good += oa;
    }
    printf("  flat JSON: %ld inputs (%ld accepted)\n", iters, good);
}

// ------------------------------------------------------------------------------------------------------------------------------ HTTP request head
static std::string httpRequest() {
    static const char* methods[] = {"GET", "POST", "PUT", "get", "", "G ET", "OPTIONS"};
    static const char* targets[] = {"/", "/api/v1/status", "/api/v1/ws?token=abc%20d", "/a?b?c", "x", "", "/a b", "/\x7f", "/\xc3\xa4", "/?", "/%00"};
    static const char* versions[] = {"HTTP/1.1", "HTTP/1.0", "HTTP/", "HTTP", "http/1.1", "HTTP/2"};
    static const char* names[] = {"Host", "host", "HOST", "Content-Length", "content-length", "Content-Type", "Transfer-Encoding", "Origin", "Authorization", "X-Odd", " Spaced ", "Upgrade", "Connection"};
    static const char* values[] = {"localhost:8765", "12", "0", "4096", "4097", "123456789", "1234567890", "", " 5 ", "abc", "-1", "chunked", "application/json", "Bearer tok", "1e3", "+5"};
    std::string r = ORD_CAT(pick(methods, 7), (r32() % 20 ? " " : "  "), pick(targets, 11), (r32() % 20 ? " " : "  "), pick(versions, 6));
    const int n = r32() % 30 == 0 ? 40 + (int)(r32() % 20) : (int)(r32() % 7);
    for (int i = 0; i < n; i++) {
        r += r32() % 40 ? "\r\n" : (r32() % 2 ? "\n" : "\r\n\r\n");
        r += ORD_CAT(pick(names, 13), (r32() % 15 ? ":" : ""), (r32() % 3 ? " " : ""), pick(values, 16), (r32() % 10 ? "" : " \r"));
    }
    if (r32() % 15 == 0) r += "\r\n";
    return r;
}
static void httpFamily(long iters) {
    long ok = 0;
    for (long it = 0; it < iters; it++) {
        std::string head = httpRequest();
        if (r32() % 4 == 0) head = mutate(head);
        A::HttpHead ha;
        const A::HttpError ea = A::httpHead(head, ha);
        dig.add(ea.status, ea.msg);
        if (!ea.status) {
            ok++;
            dig.add(ha.method, ha.path, ha.query, ha.contentLength, (uint64_t)ha.h.size());
            for (const auto& kv : ha.h) dig.add(kv.first, kv.second);
        }
        // the scan of what has arrived
        std::string buf = ORD_CAT(head, (r32() % 3 ? "\r\n\r\n" : r32() % 2 ? "\r\n\r" : ""), (r32() % 4 ? "" : rbytes(r32() % 30)));
        if (r32() % 200 == 0) buf = ORD_CAT(std::string(8180 + r32() % 40, 'a'), (r32() % 2 ? "\r\n\r\n" : ""));
        if (r32() % 200 == 0) buf = std::string(8187 + r32() % 12, 'a') + "\r\n\r\n";
        size_t ea1 = 7;
        const int sa = A::httpScan(buf, ea1);
        dig.add(sa, (uint64_t)ea1);
    }
    printf("  HTTP request heads: %ld inputs (%ld accepted)\n", iters, ok);
}

// ------------------------------------------------------------------------------------------------------------------------------ rtl_tcp, SpyServer
static void rtlFamily(long iters) {
    static const char* addrs[] = {"host:1234", "[::1]:1234", "[]:5", "[a]:5", ":80", "h:0", "h:65535", "h:65536", "h:99999999999", "h:4294967297", "h: 7", "h:+7", "h:-1", "h:", "h", "a:b:80", "[::1]", "h:12x", "h:18446744073709551617", "h:9223372036854775808"};
    for (long it = 0; it < iters; it++) {
        unsigned char h[12];
        for (auto& c : h) c = (unsigned char)r32();
        if (r32() % 2) { memcpy(h, r32() % 4 ? "RTL0" : "RTL1", 4); h[4] = h[5] = 0; h[6] = r32() % 3 ? 0 : (unsigned char)r32(); h[7] = (unsigned char)(r32() % 9); if (r32() % 3) { h[8] = h[9] = 0; } }
        const size_t n = r32() % 20 == 0 ? r32() % 12 : 12;
        A::RtlGreeting a;
        const bool oa = A::rtlGreeting(h, n, a);
        dig.add(oa); if (oa) dig.add(a.magic, a.tuner, a.nGains, a.other);
        const int t = (int)(r32() % 14) - 3;
        dig.add(A::rtlTunerGains(t), A::rtlTunerName(t));
        const uint8_t cmd = (uint8_t)r32(); const uint32_t v = r32();
        dig.add(A::rtlCommand(cmd, v));
        const std::string ad = r32() % 2 ? pick(addrs, 20) : rtext("[]:0123456789 +-ah", 14);
        std::string ha; int pa = -7;
        const bool sa = A::splitHostPort(ad, ha, pa);
        dig.add(sa); if (sa) dig.add(ha, pa);
    }
    for (int t = -5; t < 20; t++) dig.add(A::rtlTunerGains(t), A::rtlTunerName(t));
    printf("  rtl_tcp: %ld inputs\n", iters);
}

static void spyFamily(long iters) {
    for (long it = 0; it < iters; it++) {
        const std::string b = rbytes(r32() % 70);
        uint32_t da[12], ca[9];
        A::spyDeviceInfo((const unsigned char*)b.data(), (uint32_t)b.size(), da);
        A::spyClientSync((const unsigned char*)b.data(), (uint32_t)b.size(), ca);
        dig.bytes(da, sizeof da); dig.bytes(ca, sizeof ca);
        const uint32_t ty = r32() % 4 ? r32() % 4 : r32();
        dig.add(A::spyCommand(ty, (const unsigned char*)b.data(), (uint32_t)b.size()));
        dig.add(A::spyHello(), A::spySetSetting(ty, ca[0]));
        unsigned char h[20];
        for (auto& c : h) c = (unsigned char)r32();
        if (r32() % 2) { h[16] = (unsigned char)r32(); h[17] = (unsigned char)r32(); h[18] = r32() % 3 == 0 ? 0x10 : (unsigned char)(r32() % 0x12); h[19] = 0; }
        uint32_t t1 = 1, f1 = 2, s1 = 3;
        const bool oa = A::spyHeader(h, t1, f1, s1);
        dig.add(oa, t1, f1, s1);
        const uint32_t mr = r32() % 3 ? (uint32_t)(r32() % 40000000) : r32(), ds = r32() % 4 ? r32() % 30 : r32(), md = r32() % 4 ? r32() % 25 : r32();
        const auto ra = A::spyRates(mr, ds, md);
        dig.add(ra);
        dig.add(A::spyDevName(ty));
    }
    printf("  SpyServer: %ld inputs\n", iters);
}

// ------------------------------------------------------------------------------------------------------------------------------ SDR++
static void el(std::string& s, int type, const std::string& v = "", int32_t i = 0, float f = 0, bool b = false) {
    s.push_back((char)type);
    switch (type) {
    case 0: s.push_back((char)i); s.push_back((char)(r32() & 1)); break;
    case 1: s.push_back(b ? 1 : 0); break;
    case 2: for (int k = 0; k < 4; k++) s.push_back((char)((uint32_t)i >> (8 * k))); break;
    case 3: { uint32_t u; memcpy(&u, &f, 4); for (int k = 0; k < 4; k++) s.push_back((char)(u >> (8 * k))); break; }
    default: s.push_back((char)v.size()); s.push_back((char)(v.size() >> 8)); s += v; break;
    }
}
static std::string uiId() {
    static const char* ids[] = {"Bias T##_airspy_biast_Airspy Source", "##_airspy_lna_gain_Airspy Source", "##_rtlsdr_sr_RTL Source", "##sdrpp_server_src_sel", "Gain##_hackrf_amp_HackRF",
                                "##_airspy_mix_gain_Airspy Source", "##_airspy_vga_gain_Airspy Source", "Sample rate##_x_sr_Src", "##_sel_x_y", "##", "", "no hidden", "##_a_b_c_d_e_f_g",
                                "##_lin_gain_Source A", "##sens_if_x", "A##b", "##_ofs_Foo_bar", "##__a__b__", "##_airspy_unknownword_Name", "##_x_SR_y", "##_AIRSPY_bw_z"};
    return r32() % 3 ? pick(ids, 21) : rtext("ab_#LNAgsrBW if", 20);
}
static std::string uiList() {
    static const char* texts[] = {"Hello", "", "a", "Server ready"};
    std::string s;
    const int steps = (int)(r32() % 8);
    for (int k = 0; k < steps; k++) {
        const int st = r32() % 6 ? (int)(r32() % 3 ? 0x80 + (int)(r32() % 16) : (int)(r32() % 4)) : (int)(r32() & 0xff);
        el(s, 0, "", st);
        const bool wrong = r32() % 12 == 0;
        switch (st) {
        case 0x80: el(s, 4, uiId()); el(s, 2, "", (int)(r32() % 5) - 1); el(s, 4, ORD_CAT(std::string("2.4MHz\0 10 MSPS\0 250KHz\0", r32() % 3 ? 24 : 12), (r32() % 2 ? "" : rtext("ab\0", 5)))); if (wrong) el(s, 1); break;
        case 0x8b: case 0x84: el(s, 4, uiId()); el(s, 1, "", 0, 0, r32() & 1); if (wrong) el(s, 4, "x"); break;
        case 0x88: el(s, 4, uiId()); el(s, 2, "", (int)r32() % 100); el(s, 2, "", (int)(r32() % 20) - 10); el(s, 2, "", (int)(r32() % 100)); if (wrong) el(s, 3); break;
        case 0x89: case 0x8c: { el(s, 4, uiId()); const float v[] = {1.5f, -3.25f, 0.f, NAN, INFINITY}; for (int j = 0; j < 3 + (st == 0x89); j++) el(s, 3, "", 0, r32() % 7 ? (float)(int)(r32() % 100) / 4 : v[r32() % 5]); break; }
        case 0x8a: el(s, 4, uiId()); el(s, 2, "", (int)(r32() % 50) - 20); el(s, 2, "", (int)(r32() % 9) - 3); break;
        case 0x87: el(s, 4, r32() % 4 ? "Left label" : ""); break;
        case 0x8e: case 0x8f: { const int na = (int)(r32() % 4); for (int j = 0; j < na; j++) el(s, 4, pick(texts, 4)); break; }
        default: { const int na = (int)(r32() % 3); for (int j = 0; j < na; j++) { const int ty = (int)(r32() % 5); ORD_CALL(el, std::ref(s), ty == 0 ? 1 : ty, uiId(), (int)r32(), 1.f, r32() & 1); } break; }
        }
    }
    return s;
}
static void addUi(const std::vector<A::UiWidget>& a) {
    dig.add((uint64_t)a.size());
    for (const A::UiWidget& x : a) dig.add((int)x.kind, x.id, x.label, x.items, x.i, x.b, x.value, x.minV, x.maxV, x.step, x.disabled);
}
static void sdrppFamily(long iters) {
    long good = 0, widgets = 0;
    for (long it = 0; it < iters; it++) {
        std::string t = uiList();
        if (r32() % 4 == 0) t = mutate(t);
        std::vector<A::UiWidget> a;
        const bool oa = A::sdrppParseUi((const unsigned char*)t.data(), t.size(), a);
        dig.add(oa); if (oa) addUi(a);
        good += oa; widgets += (long)a.size();
        const std::string id = uiId();
        dig.add(A::sdrppIdFlags(id), A::sdrppLabelFromId(id));
        static const char* rates[] = {"10.0MHz", "2.5MHz", "250KHz", "2.048 MSPS", "48000", "inf", "nan", "0x10", "1e3", "5 Hz", "5 s/s", "5 ks/s", "5kSps", "0", "-3", "  7 mhz", "\t9", "1.5GHz", "1e999", "5xyz", "5 kk", "5 GHZ ", ""};
        const std::string rt = r32() % 2 ? pick(rates, 23) : rtext("0123456789.eE+-xXpP kKmMgGhHzZsS/inf\t", 12);
        dig.add(A::sdrppRateText(rt));
        const std::string arg = rbytes(r32() % 10);
        const uint32_t cmd = r32();
        dig.add(A::sdrppCommand(cmd, (const unsigned char*)arg.data(), arg.size()));
        const uint8_t ty = (uint8_t)(r32() % 4);
        const float f = (float)(int)r32() / 7;
        const std::string uid = r32() % 50 == 0 ? std::string(65530 + r32() % 20, 'x') : id;
        const int32_t iv = (int32_t)r32(); const bool bv = r32() & 1, sy = r32() & 1;
        dig.add(A::sdrppUiAction(uid, ty, iv, f, bv, sy));
        unsigned char h[8];
        for (auto& c : h) c = (unsigned char)r32();
        if (r32() % 2) { h[0] = (unsigned char)(r32() % 9); h[1] = h[2] = h[3] = 0; h[4] = (unsigned char)r32(); h[5] = (unsigned char)(r32() % 3); h[6] = (unsigned char)(r32() % 3 ? 0 : r32() % 0x30); h[7] = 0; }
        uint32_t ta = 5, ba = 6;
        const bool pa = A::sdrppPacketHeader(h, ta, ba);
        dig.add(pa, ta, ba);
        const uint8_t ec = (uint8_t)(r32() % 8);
        std::string ea = "a";
        dig.add(A::sdrppErrorText(ec, ea), ea);
    }
    printf("  SDR++: %ld inputs (%ld menus read, %ld widgets)\n", iters, good, widgets);
}

// ------------------------------------------------------------------------------------------------------------------------------ network radios
static std::string radioList() {
    static const char* keys[] = {"pluto", "rtltcp", "soapyremote", "spyserver", "hl2", "redpitaya", "sdrpp", "nrspst", "kiwisdr", "bogus", ""};
    static const char* hosts[] = {"192.168.2.1", " 192.168.2.1 ", "ip:pluto.local", "tcp://h", "ip:tcp://h", "tcp://ip:h", "ip:", "", "h,x", "a;b", "[::1]", "host name", "\tx\t", "ip: ip:h"};
    static const char* ports[] = {"0", "1234", "65535", "65536", "-1", "x", "", "99999999999", "4294967297", " 5", "+7", "12 34"};
    std::string s;
    const int n = (int)(r32() % 6);
    for (int i = 0; i < n; i++) {
        if (i) s += r32() % 20 ? ";" : ";;";
        s += ORD_CAT(pick(keys, 10), (r32() % 25 ? "," : ""), pick(hosts, 14), (r32() % 25 ? "," : ""), pick(ports, 12));
        if (r32() % 3 == 0) s += "," + (r32() % 2 ? std::string("driver=mirisdr,serial=1") : rtext("ab=,;x ", 8));
    }
    return s;
}
static void radiosFamily(long iters) {
    long items = 0;
    for (long it = 0; it < iters; it++) {
        std::string s = radioList();
        if (r32() % 5 == 0) s = mutate(s);
        const std::vector<A::Radio> a = A::radiosFromString(s);
        dig.add((uint64_t)a.size());
        for (const auto& x : a) dig.add(x.kind, x.host, x.port, x.remote);
        items += (long)a.size();
        // the formatter on lists with odd contents (not only parsed ones)
        std::vector<A::Radio> la;
        const int n = (int)(r32() % 4);
        for (int k = 0; k < n; k++) {
            A::Radio x; x.kind = r32() % 10; x.port = (int)(r32() % 5 == 0 ? (int)r32() : (int)(r32() % 70000) - 100);
            x.host = r32() % 2 ? "h" + rtext(" ,;:ip.tcp/", 8) : rtext("ab ip:tcp://,;", 12); x.remote = rtext("ab=,; ", 8);
            la.push_back(x);
        }
        dig.add(A::radiosToString(la));
        const std::string h = rtext("ab ip:tcp://,;\t", 16);
        dig.add(A::cleanHost(h));
    }
    for (uint32_t k = 0; k < 10; k++) {
        dig.add(A::radioTypeName(k), A::radioDefaultPort(k), A::radioDefaultHost(k));
        for (int p : {-5, 0, 1, 80, 70000}) dig.add(A::radioPort(k, p));
    }
    printf("  network radios: %ld lists (%ld items)\n", iters, items);
}

// ------------------------------------------------------------------------------------------------------------------------------ the C ABI with misuse
static void abiMisuse() {
    uint8_t tiny[1];
    CHECK(onair_net_base64((const uint8_t*)"abcd", 4, tiny, 1) == (size_t)-1, "base64 small buffer");
    CHECK(onair_net_base64(nullptr, 0, nullptr, 0) == 0, "base64 empty");
    CHECK(onair_net_ws_frame(1, (const uint8_t*)"abc", 3, tiny, 1) == (size_t)-1, "frame small buffer");
    onair_net_sha1(nullptr, 0, nullptr);
    onair_net_ws_accept(nullptr, 0, nullptr);
    OnairNetWsClient c;
    CHECK(onair_net_ws_parse_client(nullptr, 0, nullptr, 0, &c) == 0, "ws null");
    const uint8_t big[] = {0x81, 0xfe, 0x0f, 0xff, 1, 2, 3, 4};   // 4095 bytes announced, payload buffer too small
    uint8_t pl[4];
    uint8_t whole[8 + 4095] = {0x81, 0xfe, 0x0f, 0xff};
    CHECK(onair_net_ws_parse_client(whole, sizeof whole, pl, sizeof pl, &c) == 1 && c.code == 1009, "ws small payload buffer");
    (void)big;
    CHECK(onair_net_url_decode(nullptr, 0, nullptr, 0) == 0, "url null");
    CHECK(onair_net_url_decode((const uint8_t*)"a%41b", 5, tiny, 1) == 3 && tiny[0] == 'a', "url short buffer");
    bool found = true;
    CHECK(onair_net_query_param(nullptr, 0, (const uint8_t*)"a", 1, nullptr, 0, &found) == 0 && !found, "query null");
    CHECK(onair_net_flatjson_parse(nullptr, 0, nullptr, 0, nullptr, 0) == -1, "json null");
    OnairNetJsonEntry e[1];
    uint8_t ar[8];
    CHECK(onair_net_flatjson_parse((const uint8_t*)"{\"a\":1,\"b\":2}", 13, e, 1, ar, 8) == -1, "json short entries");
    onair_net_http_parse(nullptr, 0, nullptr, 0, nullptr);
    OnairNetHead hd;
    OnairNetField f[1];
    const char* two = "GET / HTTP/1.1\r\nA: 1\r\nB: 2";
    onair_net_http_parse((const uint8_t*)two, strlen(two), f, 1, &hd);
    CHECK(hd.err == 3, "http short field array");
    uint32_t st = 1;
    CHECK(onair_net_http_error(99, &st, nullptr, 0) == 0 && st == 0, "http error unknown");
    OnairNetGreeting gr;
    CHECK(!onair_net_rtltcp_greeting(nullptr, 0, &gr) && !onair_net_rtltcp_greeting((const uint8_t*)"RTL0", 4, &gr), "greeting short");
    CHECK(onair_net_rtltcp_gains(5, nullptr, 0) == 29, "gains count");
    onair_net_rtltcp_command(1, 2, nullptr);
    CHECK(!onair_net_split_host_port(nullptr, 0, nullptr, nullptr), "split null");
    onair_net_spy_device_info(nullptr, 0, nullptr);
    onair_net_spy_client_sync(nullptr, 0, nullptr);
    CHECK(onair_net_spy_command(0, nullptr, 0, tiny, 1) == (size_t)-1 && onair_net_spy_hello(tiny, 1) == (size_t)-1, "spy small buffer");
    uint32_t a1, a2, a3;
    CHECK(!onair_net_spy_header(nullptr, 0, &a1, &a2, &a3), "spy header null");
    CHECK(onair_net_spy_rates(10000000, 9, 0, nullptr, 0) == 9, "spy rates count");
    OnairNetUiCounts uc;
    onair_net_sdrpp_parse_ui(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, &uc);
    CHECK(uc.ok == 1 && uc.widgets == 0, "empty menu");
    const uint8_t cb[] = {0, 0x8b, 0, 4, 1, 0, 'x', 1, 1};
    onair_net_sdrpp_parse_ui(cb, sizeof cb, nullptr, 0, nullptr, 0, nullptr, 0, &uc);
    CHECK(uc.ok == 0, "menu with no room");
    CHECK(onair_net_sdrpp_rate_text(nullptr) == 0, "rate null");
    CHECK(onair_net_sdrpp_command(0, nullptr, 0, tiny, 1) == (size_t)-1 && onair_net_sdrpp_ui_action(nullptr, 0, 1, 0, 0, true, true, tiny, 1) == (size_t)-1, "sdrpp small buffer");
    CHECK(!onair_net_sdrpp_packet_header(nullptr, 0, &a1, &a2) && onair_net_sdrpp_error_text(9, nullptr, 0) == (size_t)-1, "sdrpp null");
    OnairNetRadio rr[1];
    CHECK(onair_net_radios_parse((const uint8_t*)"rtltcp,a,1;rtltcp,b,2", 21, rr, 1, ar, 8) == (size_t)-1, "radios short arrays");
    CHECK(onair_net_radios_parse(nullptr, 0, nullptr, 0, nullptr, 0) == 0, "radios empty");
    CHECK(onair_net_radios_format(nullptr, 0, nullptr, 0, nullptr, 0) == 0, "radios format empty");
    CHECK(onair_net_radio_clean_host((const uint8_t*)"a b", 3, tiny, 1) == (size_t)-1, "clean host small buffer");
    size_t sizes[8];
    onair_net_struct_sizes(sizes);
    onair_net_struct_sizes(nullptr);
    CHECK(onair_net_version() == 1, "version");
}

int main() {
    const long iters = getenv("ONAIR_DIFF_ITERS") ? atol(getenv("ONAIR_DIFF_ITERS")) : 100000;
    hashFamily(iters);
    wsFamily(iters);
    urlFamily(iters);
    jsonFamily(iters);
    httpFamily(iters);
    rtlFamily(iters);
    spyFamily(iters);
    sdrppFamily(iters);
    radiosFamily(iters);
    abiMisuse();
    if (abiFails) return 1;
    return golden::check("net_proto_rs", dig, 0xfcbb7b851760e7dcull, !getenv("ONAIR_DIFF_ITERS"));
}
