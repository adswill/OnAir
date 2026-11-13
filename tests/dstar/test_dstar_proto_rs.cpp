// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust D-STAR frame layer (rust/onair_dstar, dect2::dstar): CRC, header pack / unpack, flag texts, the header FEC (interleaver,
// scrambler, convolutional code, soft Viterbi with noisy / NaN input), the AMBE order, NMEA and D-PRS lines built, mutated and corrupted (the fix of a rejected
// line too), over ONAIR_DIFF_ITERS (default 200000) seeded inputs per family. Every result goes into a digest (tests/common/golden.h; doubles rounded to 1e-6,
// floats widened) compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/dstar/dstar_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::dstar;
static golden::Digest dig;

static long parsedNmea = 0, parsedDprs = 0;

static std::mt19937 g(97531);
static uint32_t r32() { return g(); }
static void addBytes(const void* p, size_t n) { dig.bytes(p, n); }
static void addFix(const A::Fix& a) {
    dig.add(a.lat, a.lon, a.altM, a.speedKmh, a.course, a.altValid, a.velValid, a.call, a.comment, a.aprs);
}
template <class F> static void sentinel(F& f) { f.lat = 1.5; f.lon = 2.5; f.altM = 3.5; f.speedKmh = 4.5; f.course = 77; f.altValid = true; f.call = "OLD"; f.comment = "OLDC"; }

static std::string rndStr(size_t n, const char* alphabet, size_t alen) { std::string s; for (size_t i = 0; i < n; i++) s += alphabet[g() % alen]; return s; }
#define RS(n, lit) rndStr((n), lit, sizeof(lit) - 1)

static std::string mutate(std::string s) {
    const int k = (int)(g() % 4);
    for (int i = 0; i < k && !s.empty(); i++) {
        switch (g() % 6) {
        case 0: s[g() % s.size()] = (char)g(); break;
        case 1: s[g() % s.size()] = "0123456789.,*$:!>/=@ANSEWRC-"[g() % 28]; break;
        case 2: ORD_CALL(s.erase, g() % s.size(), 1 + g() % 3); break;
        case 3: ORD_CALL(s.insert, g() % (s.size() + 1), RS(1 + g() % 3, "0123456789,.*/A=")); break;
        case 4: s.resize(g() % (s.size() + 1)); break;
        default: s += RS(g() % 10, "0123456789 \r\n/A="); break;
        }
    }
    return s;
}

static std::string num(double v, const char* fmt) { char b[64]; snprintf(b, sizeof b, fmt, v); return b; }
static std::string nmeaLine(bool fixChecksum) {
    std::string body;
    const char* talker[] = {"GP", "GN", "GL", "XX"};
    const char hemiLat = g() % 2 ? 'N' : 'S', hemiLon = g() % 2 ? 'E' : 'W';
    const double latDeg = (double)(g() % 90) * 100, latMin = (double)(g() % 6000) / 100.0;   // draws in separate statements: operands are not evaluated in a fixed order
    const std::string lat = num(latDeg + latMin, "%07.2f");
    const double lonDeg = (double)(g() % 180) * 100, lonMin = (double)(g() % 6000) / 100.0;
    const std::string lon = num(lonDeg + lonMin, "%08.2f");
    if (g() % 2) {
        body = ORD_CAT(talker[g() % 4], "RMC,123519,", (g() % 8 ? "A" : "V"), ",", lat, ",", hemiLat, ",", lon, ",", hemiLon, ",", (g() % 6 ? num((double)(g() % 1000) / 10.0, "%.1f") : ""), ",",
                       num((double)(g() % 3600) / 10.0, "%.1f"), ",230394,003.1,W");
    } else {
        body = ORD_CAT(talker[g() % 4], "GGA,123519,", lat, ",", hemiLat, ",", lon, ",", hemiLon, ",", (g() % 8 ? "1" : (g() % 2 ? "0" : "")), ",08,0.9,", (g() % 5 ? num((double)(g() % 20000) / 10.0 - 100, "%.1f") : ""), ",M,46.9,M,,");
    }
    std::string l = "$" + body + "*" + (fixChecksum ? A::nmeaChecksum(body) : RS(2, "0123456789ABCDEF"));
    return l;
}
static std::string dprsLine(bool fixCrc) {
    std::string pos;
    const char* lead[] = {"!", "=", "/", "@", "#"};
    const char* l = lead[g() % 5];
    pos += l;
    if (l[0] == '/' || l[0] == '@') pos += "092345z";
    char b[64];
    {
        const unsigned latD = g() % 90; const double latM = (double)(g() % 6000) / 100.0; const char ns = g() % 2 ? 'N' : 'S';
        const unsigned lonD = g() % 180; const double lonM = (double)(g() % 6000) / 100.0; const char ew = g() % 2 ? 'E' : 'W';
        snprintf(b, sizeof b, "%02u%05.2f%c%c%03u%05.2f%c%c", latD, latM, ns, '/', lonD, lonM, ew, '>');
    }
    pos += b;
    std::string c;
    if (g() % 2) { const unsigned crs = g() % 360, spd = g() % 120; snprintf(b, sizeof b, "%03u/%03u", crs, spd); c += b; }
    if (g() % 2) c += RS(g() % 12, "hello world mobile");
    if (g() % 2) { snprintf(b, sizeof b, "/A=%06u", g() % 100000); c += b; }
    if (g() % 3 == 0) c += RS(g() % 6, "xyz /A=12");
    if (g() % 4 == 0) c += (g() % 2) ? "\r" : " \r\n";
    std::string rest = "LY1TST-7>API282,DSTAR*:" + pos + c;
    if (g() % 6 == 0) rest = rest.substr(0, g() % (rest.size() + 1));
    unsigned crc = A::dprsCrc(rest + (g() % 2 ? "\r" : ""));
    if (!fixCrc) crc = r32() & 0xFFFF;
    snprintf(b, sizeof b, "$$CRC%04X,", crc);
    return b + rest;
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;

    addBytes(A::kFrameSync, 15); addBytes(A::kDataSync, 24); addBytes(A::kEndPattern, 48); addBytes(A::kSlowScramble, 3);
    for (int f0 = 0; f0 < 256; f0++) { uint8_t f[3] = {(uint8_t)f0, (uint8_t)g(), (uint8_t)g()}; dig.add(A::flagText(f)); }
    {   // the interleaver over every input position
        for (int i = 0; i < A::kHeaderBits; i++) {
            uint8_t in[660] = {}, a[660];
            in[i] = 1;
            A::interleave(in, a);
            addBytes(a, 660);
        }
    }

    for (long i = 0; i < iters; i++) {
        // CRC, checksum, byte / bit order, scrambler
        uint8_t d[200]; for (auto& x : d) x = (uint8_t)g();
        const size_t n = g() % 201;
        dig.add(A::crc(d, n));
        const std::string s((const char*)d, n);
        dig.add(A::dprsCrc(s), A::nmeaChecksum(s));
        const int nb = (int)(g() % 40);
        uint8_t b1[320];
        A::bytesToBits(d, nb, b1);
        addBytes(b1, (size_t)nb * 8);
        uint8_t y1[40];
        A::bitsToBytes(b1, nb, y1);
        addBytes(y1, (size_t)nb);
        const int sn = (int)(g() % 700);
        uint8_t c1[700];
        for (int k = 0; k < 700; k++) c1[k] = (uint8_t)(g() % 9 == 0 ? g() : g() & 1);
        A::scrambleBits(c1, sn);
        addBytes(c1, 700);
        const int cn = (int)(g() % 340);
        uint8_t e1[700] = {};
        A::convEncode(c1, cn, e1);
        addBytes(e1, 700);

        // header: fields -> bytes -> FEC -> noisy decode
        A::Header ha;
        for (int k = 0; k < 3; k++) ha.flag[k] = (uint8_t)g();
        const std::string f1 = g() % 3 ? RS(g() % 10, "ABCDEFGHIJ0123 /") : rndStr(g() % 10, "\x01\x7f\xff z", 5);
        ha.rpt2 = f1; ha.rpt1 = RS(g() % 10, "ABCD /"); ha.ur = RS(g() % 10, "CQ X"); ha.my = RS(g() % 10, "LY1TST ");
        ha.sfx = RS(g() % 6, "D75 ");
        uint8_t pa[41];
        A::headerPack(ha, pa);
        addBytes(pa, 41);
        uint8_t in41[41];
        if (g() % 2) memcpy(in41, pa, 41); else for (auto& x : in41) x = (uint8_t)g();
        if (g() % 4 == 0) in41[g() % 41] ^= (uint8_t)(1 << (g() % 8));
        A::Header ua;
        const bool oa = A::headerUnpack(in41, ua);
        dig.add(oa); addBytes(ua.flag, 3); dig.add(ua.rpt2, ua.rpt1, ua.ur, ua.my, ua.sfx);

        if (i % 4 == 0) {   // the whole header on the air, a quarter of the iterations (the decoder is the slow part)
            uint8_t hdr[41]; for (auto& x : hdr) x = (uint8_t)g();
            uint8_t tb[660];
            A::headerEncode(hdr, tb);
            addBytes(tb, 660);
            float soft[660];
            const int mode = (int)(g() % 5);
            const float sigma = (float)(g() % 30) / 10.f;
            for (int k = 0; k < 660; k++) {
                const float clean = tb[k] ? 1.f : -1.f;
                const float noise = ((float)(g() & 0xFFFF) / 65536.f - 0.5f) * 2.f * sigma;
                switch (mode) {
                case 0: soft[k] = clean; break;
                case 1: case 2: soft[k] = clean + noise; break;
                case 3: soft[k] = ((float)r32() / 4294967296.f - 0.5f) * 8.f; break;
                default: soft[k] = g() % 50 == 0 ? std::nanf("") : (g() % 40 == 0 ? INFINITY : clean + noise); break;
                }
            }
            uint8_t da[41];
            memset(da, 0, 41);
            const int ea = A::headerDecode(soft, da);
            dig.add(ea); addBytes(da, 41);
            float dil[660];
            A::deinterleave(soft, dil);
            addBytes(dil, sizeof dil);   // bit patterns (the NaN / infinity inputs pass through unchanged)
            // viterbi of a free length
            const int vn = 1 + (int)(g() % 400);
            uint8_t v1[400];
            const int ra = A::viterbi(soft, vn > 330 ? 330 : vn, v1);
            dig.add(ra); addBytes(v1, (size_t)(vn > 330 ? 330 : vn));
        }
        uint8_t ab[72]; for (auto& x : ab) x = (uint8_t)g();
        char fa[4][24];
        A::ambeFrame(ab, fa);
        addBytes(fa, sizeof fa);

        // GPS text: built lines (checksum / CRC right or wrong), then mutated
        std::string line = (g() % 2) ? nmeaLine(g() % 5 != 0) : dprsLine(g() % 5 != 0);
        if (g() % 3 == 0) line = mutate(line);
        if (g() % 40 == 0) line = RS(g() % 30, "$$CRC,*>:!0123456789ABCDEFNSEW/=@");
        A::Fix xa;
        sentinel(xa);
        const bool pn = line.compare(0, 2, "$$") == 0;
        const bool ra = pn ? A::parseDprs(line, xa) : A::parseNmea(line, xa);
        dig.add(ra); addFix(xa);
        if (ra) (pn ? parsedDprs : parsedNmea)++;
        // both parsers on both kinds of line
        A::Fix qa;
        sentinel(qa);
        dig.add(pn ? A::parseNmea(line, qa) : A::parseDprs(line, qa)); addFix(qa);
    }
    printf("dstar_proto_rs: %ld lines parsed as NMEA, %ld as D-PRS\n", parsedNmea, parsedDprs);
    if (!(parsedNmea > iters / 20 && parsedDprs > iters / 20)) { printf("the generated lines should parse often enough to be a useful test\n"); return 1; }
    return golden::check("dstar_proto_rs", dig, 0xb53a42c677ad13e3ull, !it);
}
