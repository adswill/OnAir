// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the C++ byte parsers that have no fuzz_<mode>_proto of their own, meant for DECT2_SANITIZE=ON: ATSC PSIP
// sections (MGT, VCT, EIT, ETT, STT, multiple string text, Huffman titles), CEA-608/708 captions, the ATSC 3.0 ALP reassembler, IPv4/UDP
// reassembly, LLS / AEAT / S-TSID XML, ROUTE/LCT packets and objects, MIME multipart, raw deflate and gzip, the XML reader, the RDS group
// decoder (PS, RadioText, RT+, EON, TMC, ODA) and the DVB-T2 baseband-frame unpacker. None may crash, hang or read out of bounds on any
// bytes at any length (every input is copied to an exactly sized heap block). Also a libFuzzer target with -DONAIR_LIBFUZZER (the first
// byte picks the parser). ONAIR_FUZZ_ITERS sets the iterations per parser (default 4000, ONAIR_TEST_LONG=1: 100000).
#include "dect2/atsc3/atsc3_alp.h"
#include "dect2/atsc3/atsc3_ip.h"
#include "dect2/atsc3/atsc3_route.h"
#include "dect2/common/inflate.h"
#include "dect2/common/xmlmini.h"
#include "dect2/fm/fm_rds.h"
#include "dect2/t2/t2fec.h"
#include "dect2/tv/bbunpack.h"
#include "dect2/tv/cc_decode.h"
#include "dect2/tv/psip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2;

namespace {

// exactly sized heap copy, so ASan sees every read past the end
struct Heap {
    uint8_t* p = nullptr;
    size_t n = 0;
    Heap(const uint8_t* d, size_t len) : n(len) { if (n) { p = (uint8_t*)malloc(n); memcpy(p, d, n); } }
    ~Heap() { free(p); }
};

uint32_t crcMpeg(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint32_t)d[i] << 24;
        for (int k = 0; k < 8; k++) c = c & 0x80000000u ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}
// a section of any bytes with section_length and CRC made right, so the parser goes past its checks
void fixSection(std::vector<uint8_t>& s) {
    if (s.size() < 8) s.resize(8);
    const size_t len = s.size() - 3;
    s[1] = (uint8_t)((s[1] & 0xF0) | ((len >> 8) & 0x0F));
    s[2] = (uint8_t)len;
    const uint32_t c = crcMpeg(s.data(), s.size() - 4);
    for (int i = 0; i < 4; i++) s[s.size() - 4 + i] = (uint8_t)(c >> (24 - 8 * i));
}

void psipOne(const uint8_t* d, size_t n) {
    Heap h(d, n);
    const int len = (int)n;
    psip::State st;
    std::vector<psip::Event> ev;
    uint32_t etm = 0;
    std::string txt;
    bool comp = false, ok = false;
    psip::parseMgt(h.p, len, st);
    psip::parseVctSources(h.p, len, st);
    psip::parseEit(h.p, len, st, ev);
    psip::parseEtt(h.p, len, st, &etm, &txt);
    psip::parseStt(h.p, len, st);
    psip::mss(h.p, len, &comp);
    if (n) {
        psip::psipHuffman(h.p, len, 1, &ok);
        psip::psipHuffman(h.p, len, 2, &ok);
    }
}

void ccOne(const uint8_t* d, size_t n) {
    Heap h(d, n);
    CaptionDecoder c;
    const int trip = (int)(n / 3);
    for (int i = 0; i < trip; i += 7) c.feed(h.p + 3 * i, std::min(7, trip - i), i * 0.033);
    c.finish();
}

void alpOne(const uint8_t* d, size_t n) {
    atsc3::AlpReassembler r;
    std::vector<atsc3::AlpPacket> out;
    size_t i = 0;
    while (i + 2 < n) {   // the input as a chain of baseband packets: 2 bytes size / pointer, then the payload
        const int size = std::min<int>((int)(n - i - 2), d[i] * 8 + 1);
        const int ptr = d[i + 1] == 0xFF ? 8191 : d[i + 1] % (size + 3);
        Heap h(d + i + 2, (size_t)size);
        r.push(h.p, size, ptr, out);
        if (d[i] == 0) r.reset();
        i += 2 + (size_t)size;
        out.clear();
    }
}

void ipOne(const uint8_t* d, size_t n) {
    atsc3::IpReceiver r;
    atsc3::UdpDatagram u;
    size_t i = 0;
    while (i < n) {
        const size_t size = std::min(n - i, (size_t)(i < n ? d[i] * 4 + 1 : 1));
        Heap h(d + i, size);
        r.push(h.p, size, u);
        i += size;
    }
    std::string s((const char*)d, n);
    uint32_t ip;
    atsc3::ipFromString(s, ip);
    Heap h(d, n);
    atsc3::parseLls(h.p, n);
    atsc3::parseAeat(s);
    atsc3::Stsid stsid;
    atsc3::parseStsid(s, stsid);
}

void routeOne(const uint8_t* d, size_t n) {
    Heap h(d, n);
    atsc3::LctPacket p;
    atsc3::RouteReceiver rr;
    std::vector<atsc3::RouteObject> done;
    if (atsc3::parseRoutePacket(h.p, n, p)) rr.push(p, done);
    // the same bytes as several packets of one session
    for (size_t i = 0; i + 4 < n; i += 1 + d[i] % 64) {
        Heap q(d + i, n - i);
        atsc3::LctPacket lp;
        if (atsc3::parseRoutePacket(q.p, q.n, lp)) rr.push(lp, done);
    }
    std::vector<uint8_t> doc(d, d + n);
    atsc3::parseMultipart(doc);
    atsc3::RouteService svc(0, 0xEFFF0001u, 5000);
    atsc3::UdpDatagram u;
    u.dstIp = 0xEFFF0001u;
    u.dstPort = 5000;
    u.payload.assign(d, d + n);
    svc.push(u);
    if (n > 8) { u.payload[0] = 0x10; u.payload[1] = 0x80; svc.push(u); }
}

void inflateOne(const uint8_t* d, size_t n) {
    Heap h(d, n);
    std::vector<uint8_t> out;
    size_t used = 0;
    inflateRaw(h.p, n, out, 1u << 20, &used);
    out.clear();
    gunzip(h.p, n, out, 1u << 20);
    crc32Ieee(h.p, n);
    parseXml(std::string((const char*)d, n));
}

void rdsOne(const uint8_t* d, size_t n) {
    RdsDecoder r;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint16_t b[4];
        uint8_t e[4];
        for (int k = 0; k < 4; k++) { b[k] = (uint16_t)(d[i + 2 * k] << 8 | d[i + 2 * k + 1]); e[k] = (uint8_t)((d[i] >> (2 * k)) % 3 == 2 ? 2 : 0); }
        r.onGroup(b, e, i * 0.0877);
    }
    // the bit path too: sync search, block decoding and error correction
    RdsDecoder rb;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 8; k++) rb.pushBit((d[i] >> k) & 1, (float)((d[i] >> (k ^ 3)) & 7) + 0.5f);
}

void bbOne(const uint8_t* d, size_t n) {
    BbUnpacker u;
    size_t pkts = 0;
    u.setSink([&](const uint8_t*) { pkts++; });
    size_t i = 0;
    while (i + 6 < n) {
        BbHeader h;
        h.tsGs = d[i] & 3; h.npd = d[i] >> 2 & 1; h.issyi = d[i] >> 3 & 1; h.hem = d[i] >> 4 & 1;
        h.upl = d[i + 1] % 4 == 0 ? 1504 : (d[i + 1] << 4);
        h.dfl = (d[i + 2] << 5) | (d[i + 3] & 31);
        h.syncd = (d[i + 4] << 5) | (d[i + 3] >> 3);
        h.sync = d[i + 5];
        h.crcOk = true;
        const size_t bits = std::min<size_t>((n - i - 6) * 8, (size_t)d[i + 2] * 64 + 80);
        std::vector<uint8_t> v(bits);
        for (size_t k = 0; k < bits; k++) v[k] = (d[i + 6 + k / 8] >> (k & 7)) & 1;
        u.push(v, h);
        if (d[i] & 0x80) u.lost();
        i += 6 + (bits + 7) / 8;
    }
    // the header parser on any 80 bits
    if (n >= 10) {
        uint8_t b80[80];
        for (int k = 0; k < 80; k++) b80[k] = (d[k / 8] >> (k & 7)) & 1;
        BbHeader h;
        parseBbHeader(b80, h);
    }
}

using Fn = void (*)(const uint8_t*, size_t);
struct Target { const char* name; Fn fn; };
const Target kTargets[] = {{"psip", psipOne}, {"captions", ccOne}, {"alp", alpOne}, {"ip/lls/xml", ipOne}, {"route", routeOne},
                           {"inflate/xml", inflateOne}, {"rds", rdsOne}, {"bbframe", bbOne}};

// valid seeds where a generator exists, so the mutations reach past the first length check
std::vector<std::vector<uint8_t>> seedsFor(int t, std::mt19937& g) {
    std::vector<std::vector<uint8_t>> s;
    auto rnd = [&](size_t n) { std::vector<uint8_t> v(n); for (auto& x : v) x = (uint8_t)g(); return v; };
    switch (t) {
    case 0:
        for (uint8_t tid : {0xC7, 0xC8, 0xC9, 0xCB, 0xCC, 0xCD}) {
            std::vector<uint8_t> v = rnd(40 + g() % 200);
            v[0] = tid; v[3] = 0; v[5] = 0xC1; v[6] = 0; v[7] = 0; v[8] = 0;
            fixSection(v);
            s.push_back(v);
        }
        break;
    case 1: {
        std::vector<uint8_t> v;
        for (int i = 0; i < 60; i++) { v.push_back((uint8_t)(0xFC | (i % 3 == 2 ? 2 : i % 2))); v.push_back((uint8_t)(0x20 + i)); v.push_back((uint8_t)(0x14 + i % 3)); }
        s.push_back(v);
        break;
    }
    case 2: {
        atsc3::AlpPacket p; p.data = rnd(300);
        std::vector<uint8_t> v;
        for (auto& seg : atsc3::alpSegments(p, 100)) { v.push_back((uint8_t)(seg.size() / 8)); v.push_back(0); v.insert(v.end(), seg.begin(), seg.end()); }
        s.push_back(v);
        s.push_back(atsc3::alpSingle(p));
        break;
    }
    case 3: {
        for (auto& pk : atsc3::makeUdpPackets(0x0A000001u, 0xEFFF0001u, 1234, 4937, rnd(900), 7, 300)) s.push_back(pk);
        const std::string xml = "<SLT bsid=\"1\"><Service serviceId=\"5\" shortServiceName=\"X\"><BroadcastSvcSignaling slsDestinationIpAddress=\"239.255.0.1\" slsDestinationUdpPort=\"5000\"/></Service></SLT>";
        s.push_back(atsc3::makeLls(1, 0, 1, xml));
        s.push_back(std::vector<uint8_t>(xml.begin(), xml.end()));
        break;
    }
    case 4: {
        atsc3::LctPacket p; p.tsi = 1; p.toi = 2; p.transferLength = 500; p.payload = rnd(200); p.hasSct = true; p.sct = 1234567;
        s.push_back(atsc3::makeRoutePacket(p, true));
        atsc3::MimePart m; m.headers["content-type"] = "text/xml"; m.body = rnd(50);
        s.push_back(atsc3::makeMultipart({m, m}, "bnd"));
        break;
    }
    case 5: {
        const char gz[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x01\x05\x00\xfa\xffhello\x86\xa6\x10\x36\x05\x00\x00\x00";
        s.push_back(std::vector<uint8_t>(gz, gz + sizeof gz - 1));
        const char fixed[] = "\xcb\x48\xcd\xc9\xc9\x57\x28\xcf\x2f\xca\x49\x01\x00";
        s.push_back(std::vector<uint8_t>(fixed, fixed + sizeof fixed - 1));
        const std::string x = "<?xml version=\"1.0\"?><a:b xmlns:a=\"u\" k='v'><c d=\"&amp;&#x41;\">t<![CDATA[z]]></c><!-- n --><e/></a:b>";
        s.push_back(std::vector<uint8_t>(x.begin(), x.end()));
        break;
    }
    default: s.push_back(rnd(400)); break;
    }
    return s;
}

void mutate(std::vector<uint8_t>& b, std::mt19937& g) {
    static const uint8_t kInteresting[] = {0, 1, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x40};
    const int k = 1 + (int)(g() % 6);
    for (int i = 0; i < k; i++) {
        if (b.empty()) b.push_back(0);
        switch (g() % 8) {
        case 0: b[g() % b.size()] = (uint8_t)g(); break;
        case 1: b[g() % b.size()] ^= (uint8_t)(1u << (g() % 8)); break;
        case 2: b.resize(g() % (b.size() + 1)); break;                                      // truncation
        case 3: b.insert(b.begin() + (long)(g() % (b.size() + 1)), (size_t)(1 + g() % 16), (uint8_t)g()); break;
        case 4: if (b.size() > 1) { const size_t a = g() % b.size(); b.erase(b.begin() + (long)a, b.begin() + (long)std::min(b.size(), a + 1 + g() % 32)); } break;
        case 5: b[g() % b.size()] = kInteresting[g() % 8]; break;
        case 6: { const size_t a = g() % b.size(), l = std::min(b.size() - a, (size_t)(1 + g() % 64)); std::vector<uint8_t> c(b.begin() + (long)a, b.begin() + (long)(a + l)); b.insert(b.begin() + (long)(g() % (b.size() + 1)), c.begin(), c.end()); break; }
        default: for (auto& x : b) if (g() % 16 == 0) x = (uint8_t)g();
        }
    }
}

} // namespace

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    if (n < 1) return 0;
    kTargets[d[0] % (sizeof kTargets / sizeof kTargets[0])].fn(d + 1, n - 1);
    return 0;
}
#else
int main() {
    const char* li = getenv("ONAIR_TEST_LONG");
    const long iters = getenv("ONAIR_FUZZ_ITERS") ? atol(getenv("ONAIR_FUZZ_ITERS")) : (li && *li == '1' ? 100000 : 4000);
    const int nt = (int)(sizeof kTargets / sizeof kTargets[0]);
    for (int t = 0; t < nt; t++) {
        std::mt19937 g(4242 + t);
        const auto seeds = seedsFor(t, g);
        for (const auto& s : seeds) kTargets[t].fn(s.data(), s.size());
        for (long it = 0; it < iters; it++) {
            std::vector<uint8_t> b = seeds[g() % seeds.size()];
            mutate(b, g);
            if (t == 0 && g() % 2) fixSection(b);   // PSIP: half with a right CRC
            kTargets[t].fn(b.data(), b.size());
            if (g() % 4 == 0) { std::vector<uint8_t> r(g() % 1500); for (auto& x : r) x = (uint8_t)g(); kTargets[t].fn(r.data(), r.size()); }
        }
        printf("  %-12s %ld iterations ok\n", kTargets[t].name, iters);
        fflush(stdout);
    }
    printf("fuzz_parsers: all passed\n");
    return 0;
}
#endif
