// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the file format layer (dect2::filesp, the Rust crate onair_files): CSV records / quoting / list splitting, SigMF meta files (built from
// random members, mutated, truncated, nested too deep, with duplicate keys, escapes and odd numbers) and both SigMF writers, WAV fmt chunks / classification /
// data end / names, TLE lines (valid sets with mutated columns and checksums) and TLE texts, the SQLite pieces of the MBTiles reader (varints, records, payload
// sizes, page headers, cells of all four kinds on random pages, tile rows, schema text, database header) and the results container. ONAIR_DIFF_ITERS (default
// 100000) inputs per family. Buffers are exactly as long as their content so a sanitizer sees a read past the end. The Rust C ABI is also called with null pointers
// and short arrays. Every result goes into a digest (tests/common/golden.h; doubles rounded to 1e-6) compared with the one recorded from the C++ layer the Rust code
// replaced.
#include "dect2/files/files_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include "dect2/rs/files.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::filesp;

static golden::Digest dig;
static long abiFails = 0;
#define CHECK(c, ...) do { if (!(c)) { abiFails++; printf("FAILED: " __VA_ARGS__); printf("\n"); } } while (0)
static std::mt19937 g(31337);
static uint32_t r32() { return g(); }
static uint64_t r64() { const uint64_t hi = (uint64_t)r32() << 32; return hi | r32(); }   // (two draws in one expression would be evaluated in an unspecified order)
using Buf = std::vector<uint8_t>;

static std::string pick(std::initializer_list<const char*> l) { return *(l.begin() + r32() % l.size()); }

// ------------------------------------------------------------------------------------------------------------------------------------------------------ CSV
static void csvFamily(long iters) {
    long recs = 0;
    for (long it = 0; it < iters; it++) {
        std::string t;
        const int n = (int)(r32() % 60);
        static const char alpha[] = {'a', 'b', 'c', ' ', ',', '"', '\n', '\r', ',', '"', '|', '\t', 'x'};
        for (int i = 0; i < n; i++) t += alpha[r32() % sizeof alpha];
        if (r32() % 20 == 0) for (int i = 0; i < 5; i++) t += (char)r32();
        auto a = A::csvParse(t);
        dig.add((uint64_t)a.size()); for (const auto& row : a) dig.add(row);
        recs += (long)a.size();
        std::string f;
        for (int i = 0, m = (int)(r32() % 12); i < m; i++) f += alpha[r32() % sizeof alpha];
        dig.add(A::csvQuote(f));
        const char sep = r32() % 2 ? '|' : ',';
        dig.add(A::splitList(t, sep));
        // quote then parse gives the field back (a property of both)
        if (!f.empty() && f.find('\r') == std::string::npos) { auto back = A::csvParse(A::csvQuote(f) + "\n"); CHECK(back.size() == 1 && back[0].size() == 1 && back[0][0] == f, "csv roundtrip it %ld", it); }
    }
    printf("  csv: %ld texts, %ld records\n", iters, recs);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ SigMF
static std::string jnum() {
    switch (r32() % 14) {
    case 0: return "0";
    case 1: return std::to_string(r32() % 100000);
    case 2: return std::to_string((long)(r32() % 3000000) - 1000000);
    case 3: { char b[64]; snprintf(b, sizeof b, "%.3f", (r32() % 10000000) / 1000.0); return b; }
    case 4: { char b[64]; const double q = (r32() % 1000) / 7.0; const int e = (int)(r32() % 20); snprintf(b, sizeof b, "%.9ge%d", q, e); return b; }
    case 5: if (r32() % 3 == 0) return std::to_string(r32() % 900) + "e6"; return pick({"1e6", "2.4e6", "1E3", "1e+3", "1e-3", "-5", "+7", ".5", "5.", "-.5e2", "100.5e6", "1e999", "-1e999", "1e-999", "00012", "1.5e", "3e+", "18446744073709551615", "18446744073709551616", "1e30", "123456789012345678901234567890"});
    case 6: if (r32() % 5) return std::to_string(r32() % 1000); return pick({"0x10", "inf", "nan", "NaN", "Infinity", "-", "+", ".", "1..2", "--1", "1e5e5", "0x"});
    default: return ORD_CAT(std::to_string(r32() % 1000), ".", std::to_string(r32() % 1000));   // ORD_*: draws in a fixed order (operands are not evaluated in one)
    }
}
static std::string jstr(size_t maxLen = 14) {
    std::string s = "\"";
    const size_t n = r32() % maxLen;
    for (size_t i = 0; i < n; i++) {
        switch (r32() % 12) {
        case 0: s += "\\n"; break;
        case 1: s += "\\\""; break;
        case 2: s += "\\\\"; break;
        case 3: s += "\\u00e9"; break;
        case 4: s += "\\u0041"; break;
        case 5: s += "\\u12"; break;
        case 6: s += "\\x"; break;
        case 7: s += (char)(r32() % 256); break;
        case 8: s += "\\t\\r"; break;
        default: s += (char)('a' + r32() % 26);
        }
    }
    return s + "\"";
}
static std::string gen(int depth);
static std::string genMeta() {
    auto dt = [&]() -> std::string {
        if (r32() % 8 == 0) return "\"" + pick({"cf64_le", "rf32_le", "cs16", "", "x"}) + "\"";
        return "\"" + pick({"cf32_le", "cf32_be", "ci16_le", "ci16_be", "cu16_le", "cu16_be", "ci8", "cu8", "ri8", "ru8", "CF32_LE", "ci8_le", "cu8_be", "ri16_be", "CI16"}) + "\"";
    };
    std::string s = "{";
    auto member = [&](const std::string& k, const std::string& v) { if (s.size() > 1) s += pick({",", ", ", ",\n"}); s += "\"" + k + "\":" + pick({"", " ", "\n"}) + v; };
    if (r32() % 12 != 0) {
        std::string gl = "{";
        auto gm = [&](const std::string& k, const std::string& v) { if (gl.size() > 1) gl += ", "; gl += "\"" + k + "\": " + v; };
        if (r32() % 10 != 0) gm("core:datatype", r32() % 15 == 0 ? jnum() : dt());
        if (r32() % 3) gm("core:sample_rate", r32() % 10 == 0 ? jstr() : jnum());
        if (r32() % 3) gm("core:description", r32() % 8 == 0 ? jnum() : jstr(30));
        if (r32() % 4 == 0) gm("core:dataset", jstr());
        if (r32() % 4 == 0) gm("core:header_bytes", jnum());
        if (r32() % 5 == 0) gm("core:trailing_bytes", jnum());
        if (r32() % 8 == 0) gm("core:datatype", dt());   // duplicate: the last one counts
        if (r32() % 8 == 0) gm("core:\\u0064atatype", dt());   // the same key, escaped
        if (r32() % 6 == 0) gm("x", gen(0));
        member("global", gl + "}");
    }
    if (r32() % 8 != 0) {
        std::string cs = "[";
        const int n = (int)(r32() % 3);
        for (int i = 0; i < n; i++) {
            std::string c = "{";
            if (r32() % 2) c += "\"core:sample_start\": " + jnum() + ", ";
            if (r32() % 3) c += "\"core:frequency\": " + jnum() + ", ";
            if (r32() % 5 == 0) c += "\"core:sample_rate\": " + jnum() + ", ";
            c += "\"core:datetime\": \"2026-01-01T00:00:00Z\"}";
            if (r32() % 15 == 0) c = jnum();
            cs += (i ? "," : "") + c;
        }
        member("captures", cs + "]");
    }
    if (r32() % 4 != 0) {
        std::string an = "[";
        const int n = (int)(r32() % 4);
        for (int i = 0; i < n; i++) {
            std::string a = "{";
            if (r32() % 2) a += "\"core:freq_lower_edge\": " + jnum() + ", ";
            if (r32() % 2) a += "\"core:freq_upper_edge\": " + jnum() + ", ";
            a += "\"core:label\": " + jstr() + "}";
            if (r32() % 12 == 0) a = "7";
            an += (i ? "," : "") + a;
        }
        member("annotations", an + "]");
    }
    return s + pick({"}", "}\n", "} trailing", "}}", ""});
}
static std::string gen(int depth) {
    if (depth > 70) return "1";
    switch (r32() % 7) {
    case 0: return jnum();
    case 1: return jstr();
    case 2: return pick({"true", "false", "null", "tru", "nul"});
    case 3: return ORD_CAT("[", gen(depth + 1), (r32() % 2 ? "," + gen(depth + 1) : ""), "]");
    case 4: return "{\"k\": " + gen(depth + 1) + "}";
    default: return jnum();
    }
}
static std::string mutate(std::string s) {
    const int k = (int)(r32() % 5);
    for (int i = 0; i < k && !s.empty(); i++) {
        switch (r32() % 4) {
        case 0: s[r32() % s.size()] = (char)r32(); break;
        case 1: s.resize(r32() % (s.size() + 1)); break;
        case 2: ORD_CALL(s.insert, s.begin() + r32() % (s.size() + 1), "{}[],:\"\\ "[r32() % 10]); break;
        default: s.erase(s.begin() + r32() % s.size());
        }
    }
    return s;
}
static void addMeta(const A::SigmfMeta& a) {
    dig.add(a.status, a.format, a.bigEndian, a.annotations, a.sampleRate, a.centerHz, a.sampleStart, a.headerBytes, a.trailingBytes, a.description, a.dataset, a.datatype);
}
static void sigmfFamily(long iters) {
    long ok = 0, bad[5] = {};
    for (long it = 0; it < iters; it++) {
        std::string t = genMeta();
        const int w = (int)(r32() % 10);
        if (w == 0) { std::string deep; const int d = 60 + (int)(r32() % 10); for (int i = 0; i < d; i++) deep += "["; for (int i = 0; i < d; i++) deep += "]"; t = "{\"global\":{\"core:datatype\":\"ci8\",\"x\":" + deep + "}}"; }
        else if (w <= 3) t = mutate(t);
        else if (w == 4) { Buf b(r32() % 80); for (auto& x : b) x = (uint8_t)r32(); t.assign((const char*)b.data(), b.size()); }
        // exactly sized heap copies: a read past the end shows under a sanitizer
        char* p = (char*)malloc(t.size() + 1); memcpy(p, t.data(), t.size());
        const std::string exact(p, t.size());
        free(p);
        auto a = A::sigmfParse(exact);
        addMeta(a);
        if (a.status == 0) ok++; else bad[a.status]++;
        // writers
        const double rates[] = {0, 2.4e6, 10e6, 61.44e6, 1e-3, 123456789.123, 1e15, -5, std::nan(""), INFINITY, -INFINITY, 1e300, 0.1 + 0.2, 2399999.9999999995};
        const double rate = rates[r32() % (sizeof rates / sizeof *rates)], center = rates[r32() % (sizeof rates / sizeof *rates)];
        std::string desc; for (int i = 0, m = (int)(r32() % 24); i < m; i++) desc += (char)(r32() % 3 ? 32 + r32() % 95 : r32() % 256);
        const std::string dtn = pick({"cf32_le", "ci8_le", "cu8", "ci16_be", "cu16_le"});
        const uint64_t ss = r32() % 3 ? r32() : r64();
        { dig.add(A::sigmfToolMeta(dtn, rate, center, ss, desc)); }
        const std::string when = r32() % 3 ? "2026-10-13T09:15:30Z" : "", ds = r32() % 3 ? "onair_x.cs8" : "";
        { dig.add(A::sigmfRecorderMeta(dtn, rate, center, when, ds, ss % 7 ? ss : 0, desc)); }
    }
    printf("  sigmf: %ld metas (%ld ok; bad: json %ld, no global %ld, no datatype %ld, unsupported %ld)\n", iters, ok, bad[1], bad[2], bad[3], bad[4]);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ WAV
static void wavFamily(long iters) {
    for (long it = 0; it < iters; it++) {
        Buf body(r32() % 40);
        for (auto& x : body) x = (uint8_t)r32();
        if (body.size() >= 16 && r32() % 2) { body[0] = r32() % 4 ? 1 : 3; body[1] = 0; body[2] = r32() % 3 ? 2 : 1; body[3] = 0; body[14] = (uint8_t)(r32() % 4 == 0 ? 32 : r32() % 2 ? 16 : 8); body[15] = 0; }
        if (body.size() >= 26 && r32() % 4 == 0) { body[0] = 0xFE; body[1] = 0xFF; }
        const uint32_t len = r32() % 3 ? (uint32_t)body.size() : r32() % 64;
        uint8_t* p = (uint8_t*)malloc(body.size() + 1); memcpy(p, body.data(), body.size());
        const A::WavFmt a = A::wavFmt(p, body.size(), len);
        free(p);
        dig.add(a.tag, a.channels, a.rate, a.bits);
        int fa = -1;
        const int ca = A::wavClassify(a, fa);
        dig.add(ca, fa);
        const uint64_t off = r32() % 3 ? r32() % 100 : r64(), size = r32() % 4 ? r32() % 1000000 : r64();
        const uint32_t dl = r32() % 5 == 0 ? 0 : r32() % 5 == 1 ? 0xFFFFFFFFu : r32();
        dig.add(A::wavDataEnd(off, dl, size));
        std::string name;
        for (int i = 0, m = (int)(r32() % 24); i < m; i++) name += pick({"1", "2", "5", "0", ".", "_", "MHz", "mhz", "a", "x", "00", ".5", "3.", "9"});
        if (r32() % 2) name += pick({"MHz", "mhz", "MHZ", "mHz"});
        dig.add(A::wavCenterFromName(name));
    }
    printf("  wav: %ld cases\n", iters);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ TLE
static std::string withChecksum(std::string l) {
    l.resize(68, ' ');
    int sum = 0;
    for (char c : l) { if (c >= '0' && c <= '9') sum += c - '0'; else if (c == '-') sum++; }
    return l + char('0' + sum % 10);
}
static void tleLines(std::string& l1, std::string& l2) {
    auto digits = [&](int n) { std::string s; for (int i = 0; i < n; i++) s += char('0' + r32() % 10); return s; };
    const std::string cat = r32() % 10 ? digits(5) : " 8888";
    std::string day = ORD_CAT(std::to_string(1 + r32() % 365), ".", digits(8));
    while (day.size() < 12) day = " " + day;
    l1 = ORD_CAT("1 ", cat, "U 98067A   ", digits(2), day, " -.", digits(8), "  00000-0 ", pick({"-11606-4", " 11606-4", " 00000-0", "-12345+1", "   123-5", "10000-0", "-1e5-3", " 1.5-3"}), " 0  ", digits(4));
    l2 = ORD_CAT("2 ", (r32() % 20 ? cat : digits(5)), " ", pick({" 51.6416", "  0.0000", "180.0000", "98.7654 ", "-1.2345 ", " 1e1.5  "}), " ", digits(3), ".", digits(4), " ", digits(7), " ", digits(3), ".", digits(4), " ", digits(3), ".", digits(4), " ", pick({"15.72125391", " 0.00000000", "14.12345678", "-1.5000000 ", "16.5       "}), digits(5));
    if (r32() % 5) { l1 = withChecksum(l1); l2 = withChecksum(l2); }
}
static void addTle(const A::TleData& a) {
    dig.add(a.status, a.name, a.norad, a.epochJd, a.bstar, a.inclDeg, a.raanDeg, a.ecc, a.argpDeg, a.meanAnomDeg, a.meanMotion);
}
static void tleFamily(long iters) {
    long ok = 0, unusable = 0;
    for (long it = 0; it < iters; it++) {
        std::string l1, l2;
        tleLines(l1, l2);
        const int w = (int)(r32() % 8);
        if (w == 0) l1 = mutate(l1);
        else if (w == 1) l2 = mutate(l2);
        else if (w == 2) { l1 = "  " + l1 + " \r\n"; l2 = "\t" + l2 + "  "; }
        else if (w == 3 && l1.size() > 30) l1[20 + r32() % 12] = "+-.eE x9"[r32() % 8];
        else if (w == 4 && l2.size() > 60) l2[8 + r32() % 50] = "+-.eE x9"[r32() % 8];
        const std::string name = r32() % 3 ? pick({"ISS (ZARYA)", " 0 NOAA 19 ", "0 X", "0  Y ", "0 ", "  ", "NOAA 15\r", "", "0X"}) : "";
        auto a = A::tleParse(l1, l2, name);
        addTle(a);
        if (a.status == 0) ok++; else if (a.status == 2) unusable++;
        // a text of several sets, names, junk and blank lines
        std::string text;
        for (int k = 0, m = (int)(r32() % 4); k < m; k++) {
            std::string a1, a2;
            tleLines(a1, a2);
            if (r32() % 2) text += ORD_CAT(pick({"ISS", "0 NOAA", "  X  ", "1abc", "2 junk"}), pick({"\n", "\r\n"}));
            text += ORD_CAT(a1, pick({"\n", "\r\n", "\n\n"}), (r32() % 8 ? a2 : mutate(a2)), pick({"\n", "", "\r\n"}));
            if (r32() % 4 == 0) text += pick({"\n", "junk line\n", "   \n"});
        }
        if (r32() % 10 == 0) text = mutate(text);
        auto ta = A::tleScan(text);
        dig.add((uint64_t)ta.size()); for (const auto& e : ta) addTle(e);
    }
    printf("  tle: %ld sets (%ld ok, %ld unusable orbits)\n", iters, ok, unusable);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ SQLite
static void putVarint(Buf& b, uint64_t v) {
    if (v >> 56) { uint8_t t[9]; t[8] = (uint8_t)v; v >>= 8; for (int i = 7; i >= 0; i--) { t[i] = (uint8_t)(0x80 | (v & 0x7F)); v >>= 7; } b.insert(b.end(), t, t + 9); return; }
    uint8_t t[9]; int n = 0;
    do { t[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v);
    for (int i = n - 1; i >= 0; i--) b.push_back(t[i] | (i ? 0x80 : 0));
}
static Buf genRecord() {
    Buf hdr, body;
    const int n = (int)(r32() % 6);
    for (int i = 0; i < n; i++) {
        const uint64_t t = r32() % 14 == 0 ? r32() % 300 : (uint64_t[]){0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 + 2 * (r32() % 5), 13 + 2 * (r32() % 5)}[r32() % 14];
        putVarint(hdr, t);
        size_t len = t >= 1 && t <= 6 ? (size_t[]){0, 1, 2, 3, 4, 6, 8}[t] : t == 7 ? 8 : t >= 12 ? (t - 12) / 2 : 0;
        for (size_t k = 0; k < len; k++) body.push_back((uint8_t)r32());
    }
    Buf r;
    putVarint(r, hdr.size() + 1 + (r32() % 20 == 0 ? r32() % 3 : 0));
    r.insert(r.end(), hdr.begin(), hdr.end());
    r.insert(r.end(), body.begin(), body.end());
    if (r32() % 6 == 0) r.resize(r32() % (r.size() + 1));
    if (r32() % 8 == 0 && !r.empty()) r[r32() % r.size()] = (uint8_t)r32();
    return r;
}
static void addCols(const std::vector<A::Col>& a) {
    dig.add((uint64_t)a.size());
    for (const auto& c : a) dig.add(c.isNull, c.isInt, c.isBlob, c.i, c.s, c.b);
}
static Buf genPage(int kind) {
    const size_t size = 40 + r32() % 200;
    Buf pg(size);
    for (auto& x : pg) x = (uint8_t)r32();
    const size_t base = r32() % 4 == 0 ? 100 : 0;
    if (pg.size() < base + 12) pg.resize(base + 12);
    pg[base] = (uint8_t)kind;
    const unsigned cells = r32() % 12;
    pg[base + 3] = (uint8_t)(cells >> 8); pg[base + 4] = (uint8_t)cells;
    // plausible cell pointers
    const size_t hdr = base + ((kind == 13 || kind == 10) ? 8 : 12);
    for (unsigned i = 0; i < cells && hdr + 2 * i + 2 <= pg.size(); i++) {
        const unsigned o = (unsigned)(r32() % 8 ? r32() % pg.size() : pg.size() - r32() % 6);
        pg[hdr + 2 * i] = (uint8_t)(o >> 8); pg[hdr + 2 * i + 1] = (uint8_t)o;
    }
    return pg;
}
static void sqliteFamily(long iters) {
    long recs = 0, cellsOk = 0;
    for (long it = 0; it < iters; it++) {
        // varint
        Buf v(r32() % 12);
        for (auto& x : v) x = (uint8_t)(r32() % 3 ? (r32() & 0xFF) : (r32() & 0x7F));
        uint8_t* vp = (uint8_t*)malloc(v.size() + 1); memcpy(vp, v.data(), v.size());
        uint64_t va = 1;
        const int na = A::varint(vp, vp + v.size(), va);
        free(vp);
        dig.add(na); if (na) dig.add(va);
        // record
        const Buf rec = genRecord();
        std::vector<A::Col> ca;
        const bool wb = r32() % 2;
        const bool ra = A::record(rec, ca, wb);
        dig.add(ra); if (ra) addCols(ca);
        recs += ra;
        // payload sizes
        const uint64_t total = r32() % 4 ? r32() % 70000 : (r32() % 2 ? (1u << 28) + r32() % 3 - 1 : (uint64_t)r32() << 3), usable = r32() % 6 == 0 ? 36 + r32() % 600 : (uint64_t[]){480, 1024, 4096, 65536 - 12, 4084}[r32() % 5];
        uint64_t la = 7; bool oa = true;
        const bool pa = A::payloadLocal(total, usable, r32() % 2, la, oa);
        const uint32_t seed = r32(); (void)seed;
        // the same index_tree flag for both
        const bool idx = r32() % 2;
        la = 7; oa = true;
        const bool pa2 = A::payloadLocal(total, usable, idx, la, oa);
        (void)pa;
        dig.add(pa2); if (pa2) dig.add(la, oa);
        // page header and cells
        const int kinds[] = {13, 5, 10, 2, 0, 7, 255};
        const int kind = kinds[r32() % 7];
        Buf pg = genPage(kind == 255 ? 13 : kind);
        const size_t base = pg[0] == (uint8_t)kind ? 0 : 100;
        uint8_t ta = 1; unsigned cna = 0; uint32_t rma = 0;
        const bool ha = A::pageHeader(pg, base, ta, cna, rma);
        dig.add(ha); if (ha) dig.add(ta, cna, rma);
        const int ck = r32() % 6 == 0 ? 6 : kind;
        for (unsigned i = 0; i <= cna + 1; i++) {
            A::CellInfo xa;
            const bool ba = A::cell(pg, ck, base, i, cna, xa);
            dig.add(ba); if (ba) dig.add(xa.child, xa.payloadOff, xa.total, xa.key);
            cellsOk += ba;
        }
        // tile row, picture kinds
        const int z = (int)(r32() % 40) - 3, x = (int)(r32() % 2000) - 20, y = (int)(r32() % 2000) - 20;
        int64_t rowa = 5;
        const bool ta2 = A::tileRow(z, x, y, rowa);
        dig.add(ta2); if (ta2) dig.add(rowa);
        Buf pic(r32() % 12);
        for (auto& b : pic) b = (uint8_t)r32();
        if (pic.size() >= 3 && r32() % 2) { pic[0] = 0xFF; pic[1] = 0xD8; pic[2] = 0xFF; }
        if (pic.size() >= 8 && r32() % 2) { pic[0] = 0x89; pic[1] = 'P'; pic[2] = 'N'; pic[3] = 'G'; }
        dig.add(A::isRaster(pic), A::isPng(pic));
        // schema text
        std::string sql = r32() % 4 ? "CREATE TABLE tiles (" : "CREATE TABLE `tiles` [";
        const char* names[] = {"zoom_level", "tile_column", "tile_row", "tile_data"};
        int order[4] = {0, 1, 2, 3};
        if (r32() % 4 == 0) { const size_t i1 = r32() % 4; const size_t i2 = r32() % 4; std::swap(order[i1], order[i2]); }
        const int cnt = r32() % 12 == 0 ? (int)(r32() % 4) : 4;
        for (int k = 0; k < cnt; k++) sql += ORD_CAT((k ? ", " : ""), pick({"", "\"", " "}), (r32() % 5 == 0 ? "ZOOM_LEVEL" : names[order[k]]), pick({"", " integer", " INTEGER NOT NULL", " blob"}));
        sql += pick({")", ", PRIMARY KEY (zoom_level, tile_column, tile_row))", ""});
        std::string sa;
        const bool sga = A::tilesSchemaOk(sql, sa);
        dig.add(sga); if (sga) dig.add(sa);
        std::string isql = r32() % 4 == 0 ? "" : pick({"CREATE INDEX i ON tiles (zoom_level, tile_column, tile_row)", "CREATE INDEX i ON tiles (\"zoom_level\",\"tile_column\",\"tile_row\")", "CREATE INDEX j ON tiles (zoom_level, tile_column)", "CREATE UNIQUE INDEX k ON tiles(tile_row)"});
        dig.add(A::indexCovers(isql, sga ? sa : sql));
        // database header
        Buf h(100 + (r32() % 20 == 0 ? 0 : 0));
        for (auto& b : h) b = (uint8_t)r32();
        if (r32() % 8) memcpy(h.data(), "SQLite format 3", 16);
        if (r32() % 2) { const unsigned ps = (unsigned[]){512, 1024, 4096, 65536, 1, 513, 256, 0, 2048}[r32() % 9]; h[16] = (uint8_t)(ps >> 8); h[17] = (uint8_t)ps; }
        if (r32() % 2) { h[18] = r32() % 3; h[19] = r32() % 3; h[20] = r32() % 3 ? 0 : (uint8_t)r32(); }
        if (r32() % 2) { h[56] = h[57] = h[58] = 0; h[59] = r32() % 4; }
        const size_t hn = r32() % 10 == 0 ? r32() % 100 : 100;
        const A::DbHeader da = A::dbHeader(h.data(), hn);
        dig.add(da.status, da.pageSize, da.reserved, da.usable);
    }
    printf("  sqlite: %ld rounds, %ld records decoded, %ld cells decoded\n", iters, recs, cellsOk);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ container
static void containerFamily(long iters) {
    for (long it = 0; it < iters; it++) {
        std::string s(r32() % 40, '\0');
        for (auto& c : s) c = (char)r32();
        dig.add(A::fnv1a(s), A::signatureHash(s));
        const uint64_t h = A::signatureHash(s);
        std::string blob = A::containerHeader(h);
        CHECK(blob.size() == 16, "container header it %ld", it); dig.add(blob);
        blob += std::string(r32() % 8, 'x');
        if (r32() % 3 == 0) blob[r32() % blob.size()] ^= (char)(1 + r32() % 255);
        if (r32() % 6 == 0) blob.resize(r32() % (blob.size() + 1));
        const uint64_t expect = r32() % 4 ? h : r32();
        dig.add(A::containerCheck(blob, expect));
    }
    printf("  container: %ld blobs\n", iters);
}

// the C ABI with hostile arguments: error values, no crash
static void abiMisuse() {
    uint8_t buf[64] = {};
    size_t sz[3];
    CHECK(!onair_files_csv_measure(nullptr, 0, nullptr), "measure null out");
    CHECK(onair_files_csv_measure(nullptr, 0, sz) && sz[0] == 0 && sz[1] == 0 && sz[2] == 0, "measure null text");
    CHECK(!onair_files_csv_parse((const uint8_t*)"a,b\n", 4, buf, 1, nullptr, 0, nullptr, 0), "parse short arrays");
    CHECK(onair_files_csv_parse(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0), "parse empty");
    CHECK(onair_files_csv_quote((const uint8_t*)"a,b", 3, buf, 2) == SIZE_MAX, "quote short");
    CHECK(onair_files_csv_quote_len(nullptr, 0) == 0, "quote len null");
    uint32_t ends[2];
    CHECK(onair_files_split_list((const uint8_t*)"a|b|c", 5, '|', ends, 2) == SIZE_MAX, "split short ends");
    CHECK(onair_files_split_list((const uint8_t*)"a|b|c", 5, '|', nullptr, 0) == 3, "split count");
    CHECK(onair_files_json_unescape(nullptr, 0, nullptr, 0) == 0, "unescape null");
    OnairFilesSigmf m;
    onair_files_sigmf_parse(nullptr, 0, &m);
    CHECK(m.status == 1, "sigmf null text");
    onair_files_sigmf_parse(nullptr, 0, nullptr);
    int32_t f, be;
    CHECK(!onair_files_sigmf_datatype(nullptr, 0, &f, &be), "datatype null");
    CHECK(onair_files_sigmf_write_tool(nullptr, 0, 1, 2, 3, nullptr, 0, nullptr, 0) > 100, "write tool measure");
    CHECK(onair_files_sigmf_write_recorder(nullptr, 0, 1, 2, nullptr, 0, nullptr, 0, 0, nullptr, 0, buf, 5) > 100, "write recorder short");
    OnairFilesWavFmt w;
    onair_files_wav_fmt(nullptr, 0, 0, &w);
    CHECK(w.tag == 0 && w.channels == 0, "wav fmt null");
    CHECK(onair_files_wav_classify(nullptr, &f) == 2, "wav classify null");
    CHECK(onair_files_wav_center_from_name(nullptr, 0) == 0, "wav name null");
    OnairFilesTle t;
    onair_files_tle_parse(nullptr, 0, nullptr, 0, nullptr, 0, &t);
    CHECK(t.status == 1, "tle null");
    CHECK(onair_files_tle_scan(nullptr, 0, nullptr, 0) == 0, "tle scan null");
    OnairFilesDbHeader dh;
    onair_files_sqlite_header(nullptr, 0, &dh);
    CHECK(dh.status == 1, "db header null");
    uint64_t v;
    CHECK(onair_files_sqlite_varint(nullptr, 0, &v) == 0, "varint null");
    CHECK(onair_files_sqlite_record(nullptr, 0, nullptr, 0) == SIZE_MAX, "record null");
    CHECK(onair_files_sqlite_payload_local(10, 10, false, &v) == 0, "payload tiny usable");
    uint32_t o[3];
    CHECK(!onair_files_sqlite_page_header(nullptr, 0, 0, o), "page header null");
    CHECK(!onair_files_sqlite_page_header(buf, 64, 100, o), "page header past end");
    OnairFilesCell c;
    CHECK(!onair_files_sqlite_cell(nullptr, 0, 13, 0, 0, 1, &c), "cell null");
    CHECK(!onair_files_sqlite_cell(buf, 64, 99, 0, 0, 1, &c), "cell bad kind");
    CHECK(!onair_files_sqlite_cell(buf, 64, 13, 0, (size_t)-1 / 4, (size_t)-1 / 4, &c), "cell huge index");
    int64_t row;
    CHECK(!onair_files_sqlite_tile_row(5, 99, 0, &row), "tile row range");
    CHECK(onair_files_sqlite_tiles_schema(nullptr, 5, buf, 64) == SIZE_MAX, "schema null");
    CHECK(onair_files_sqlite_tiles_schema((const uint8_t*)"x", 1, buf, 0) == SIZE_MAX, "schema short scratch");
    CHECK(!onair_files_sqlite_index_covers((const uint8_t*)"abc", 3, buf, 1, nullptr, 0), "index short scratch");
    CHECK(onair_files_container_check(nullptr, 0, 1) == SIZE_MAX, "container null");
    CHECK(!onair_files_container_header(1, buf, 8), "container short");
}

int main() {
    const long iters = getenv("ONAIR_DIFF_ITERS") ? atol(getenv("ONAIR_DIFF_ITERS")) : 100000;
    printf("files differential, %ld iterations per family\n", iters);
    csvFamily(iters);
    sigmfFamily(iters);
    wavFamily(iters);
    tleFamily(iters);
    sqliteFamily(iters);
    containerFamily(iters);
    abiMisuse();
    if (abiFails) return 1;
    return golden::check("files_proto_rs", dig, 0x88007ab9e0e04110ull, !getenv("ONAIR_DIFF_ITERS"));
}
