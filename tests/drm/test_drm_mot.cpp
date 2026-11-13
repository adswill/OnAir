// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MOT (ETSI EN 301 234, TS 101 499) parameter parsing and reassembly with hand made byte vectors, and the DRM packet layer (ES 201 980 clause 6.6.1):
// first / last flags, padded packets, continuity index, lost packets, CRC-16, other Packet Ids, orphans, and the application names of the SDC.
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_mot.h"
#include "dect2/dab/dab_journaline.h"
#include "dect2/drm/drm_data.h"
#include "dect2/drm/drm_fec.h"
#include "dect2/drm/drm_msg.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

typedef std::vector<uint8_t> Bytes;

// MJD 60000 = 2023-02-25: 17 bits 0xEA60. Time word: validity 1, MJD, LSI 0, confidence 0, UTC flag, hours 13 (01101), minutes 45 (101101)
//   1 | 0 1110 1010 0110 0000 | 0 0 0 | 01101 101101  ->  BA 98 03 6D (short), BA 98 0B 6D with the UTC flag set (long form)
static void testTime() {
    MotTime t;
    const uint8_t now[4] = {0, 0, 0, 0};
    CHECK(motParseTime(now, 4, t) == 4 && t.valid == false && t.now && motTimeText(t) == "now", "time now");
    const uint8_t sh[4] = {0xBA, 0x98, 0x03, 0x6D};
    CHECK(motParseTime(sh, 4, t) == 4 && t.valid && !t.longForm && t.mjd == 60000 && t.hour == 13 && t.minute == 45, "short time %d %d:%d", t.mjd, t.hour, t.minute);
    CHECK(motTimeText(t) == "2023-02-25 13:45", "time text '%s'", motTimeText(t).c_str());
    const uint8_t lg[6] = {0xBA, 0x98, 0x0B, 0x6D, 0x79, 0xF4};   // seconds 30, milliseconds 500
    CHECK(motParseTime(lg, 6, t) == 6 && t.longForm && t.second == 30 && t.ms == 500 && t.hour == 13, "long time %d s %d ms", t.second, t.ms);
    CHECK(motParseTime(lg, 5, t) == 0 && motParseTime(lg, 3, t) == 0, "long form with too few bytes");
}

// a header of an image/JPEG object of 291 bytes with the slide show parameters, written out byte by byte
static Bytes slideHeader() {
    Bytes h = {0x00, 0x00, 0x12, 0x30, 0x18, 0x04, 0x01};   // body 0x123, header size 48, content type 2 (image), subtype 1 (JPEG)
    const std::string name = "slide1.jpg", url = "http://a.b/x";
    h.push_back(0xCC); h.push_back(11); h.push_back(0x00); h.insert(h.end(), name.begin(), name.end());   // ContentName: PLI 3, length 11, character set 0
    h.push_back(0xE5); h.push_back(2); h.push_back(3); h.push_back(7);                                      // CategoryID 3 / SlideID 7
    h.push_back(0x85); for (uint8_t b : {0xBA, 0x98, 0x03, 0x6D}) h.push_back(b);                          // TriggerTime: PLI 2 (4 bytes)
    h.push_back(0xE7); h.push_back(12); h.insert(h.end(), url.begin(), url.end());                          // ClickThroughURL
    h.push_back(0xD7); h.push_back(3); h.push_back(0x00); h.push_back('H'); h.push_back('i');               // ContentDescription "Hi"
    return h;
}

static void testHeader() {
    const Bytes h = slideHeader();
    CHECK(h.size() == 48, "header bytes %zu", h.size());
    MotObject o;
    CHECK(motParseHeader(h.data(), h.size(), o), "parse");
    CHECK(o.bodySize == 0x123 && o.headerSize == 48 && o.contentType == 2 && o.subtype == 1, "core %zu %zu %d %d", o.bodySize, o.headerSize, o.contentType, o.subtype);
    CHECK(o.name == "slide1.jpg", "name '%s'", o.name.c_str());
    CHECK(o.categoryId == 3 && o.slideId == 7, "category %d slide %d", o.categoryId, o.slideId);
    CHECK(o.hasTrigger && o.trigger.valid && o.trigger.mjd == 60000 && o.trigger.hour == 13 && o.trigger.minute == 45, "trigger");
    CHECK(o.clickUrl == "http://a.b/x", "url '%s'", o.clickUrl.c_str());
    CHECK(o.caption == "Hi", "caption '%s'", o.caption.c_str());
    CHECK(o.params.size() == 5, "%zu parameters", o.params.size());
    CHECK(std::string(motContentTypeName(o.contentType, o.subtype)) == "image/JPEG", "type name");
    CHECK(std::string(motContentTypeName(2, 3)) == "image/PNG" && std::string(motContentTypeName(1, 2)) == "text/HTML", "type names");
    // a 15 bit length indicator (0x80 | high byte, low byte): ContentName of 130 bytes; PLI 0 and PLI 1 parameters in front
    Bytes g = {0x00, 0x00, 0x00, 0x10, 0x00, 0x04, 0x03};   // header size patched below
    g.push_back(0x09);                                        // PLI 0, parameter 0x09: no data
    g.push_back(0x44); g.push_back(0x42);                     // PLI 1, parameter 0x04: one byte
    g.push_back(0xCC); g.push_back(0x80); g.push_back(130); g.push_back(0x0F);
    for (int i = 0; i < 129; i++) g.push_back((uint8_t)('a' + i % 26));
    const size_t hs = g.size();
    g[3] = (uint8_t)(0x10 | (hs >> 9)); g[4] = (uint8_t)(hs >> 1); g[5] = (uint8_t)(((hs & 1) << 7) | 0x04);
    MotObject p;
    CHECK(motParseHeader(g.data(), g.size(), p) && p.headerSize == hs && p.name.size() == 129 && p.params.size() == 3 && p.subtype == 3, "long parameter: %zu bytes name, %zu params", p.name.size(), p.params.size());
    CHECK(p.params[0].id == 0x09 && p.params[0].data.empty() && p.params[1].id == 0x04 && p.params[1].data.size() == 1, "parameter classes");
    // a parameter that runs past the header end is ignored; a short header is refused
    Bytes bad = slideHeader();
    bad.resize(40);
    MotObject q;
    CHECK(motParseHeader(bad.data(), bad.size(), q) && q.name == "slide1.jpg" && q.clickUrl.empty(), "cut header");
    CHECK(!motParseHeader(h.data(), 6, q), "6 bytes are no header");
}

// an MSC data group: CRC flag, segment flag, user access flag with a 2 byte transport id; the MOT segmentation header in front of the segment
static Bytes group(int type, uint16_t tid, int segNo, bool last, const Bytes& seg) {
    Bytes g = {(uint8_t)(0x70 | type), 0x00, (uint8_t)((last ? 0x80 : 0) | (segNo >> 8)), (uint8_t)segNo, 0x12, (uint8_t)(tid >> 8), (uint8_t)tid, (uint8_t)(seg.size() >> 8), (uint8_t)seg.size()};
    g.insert(g.end(), seg.begin(), seg.end());
    const uint16_t c = dab::crc16(g.data(), (int)g.size());
    g.push_back((uint8_t)(c >> 8)); g.push_back((uint8_t)c);
    return g;
}
static Bytes slice(const Bytes& v, size_t a, size_t b) { return Bytes(v.begin() + (long)a, v.begin() + (long)std::min(b, v.size())); }

static void testReassembly() {
    MotDecoder d;
    std::vector<MotObject> got;
    d.setSink([&](const MotObject& o) { got.push_back(o); });
    Bytes h = slideHeader();
    Bytes body(0x123);
    for (size_t i = 0; i < body.size(); i++) body[i] = (uint8_t)(i * 7 + 3);
    // header in two segments, body in three, sent out of order, one repeated, one damaged, one with a wrong transport id
    const Bytes g0 = group(4, 0x0042, 1, false, slice(body, 100, 200));
    d.dataGroup(g0.data(), (int)g0.size());
    Bytes bad = group(4, 0x0042, 0, false, slice(body, 0, 100));
    bad[12] ^= 1;                                                          // a bit error: the CRC does not fit
    d.dataGroup(bad.data(), (int)bad.size());
    CHECK(d.stats().groupsBad == 1, "damaged group counted: %llu", (unsigned long long)d.stats().groupsBad);
    const Bytes other = group(4, 0x0099, 2, true, slice(body, 200, 400));
    d.dataGroup(other.data(), (int)other.size());
    const Bytes hb = group(3, 0x0042, 1, true, slice(h, 30, 48));
    d.dataGroup(hb.data(), (int)hb.size());
    const Bytes ha = group(3, 0x0042, 0, false, slice(h, 0, 30));
    d.dataGroup(ha.data(), (int)ha.size());
    d.dataGroup(g0.data(), (int)g0.size());                                // the repeat
    CHECK(got.empty() && d.stats().repeats >= 1, "nothing complete yet (%zu), repeats %llu", got.size(), (unsigned long long)d.stats().repeats);
    const Bytes b0 = group(4, 0x0042, 0, false, slice(body, 0, 100));
    d.dataGroup(b0.data(), (int)b0.size());
    CHECK(got.empty(), "segment 2 is missing");
    const Bytes b2 = group(4, 0x0042, 2, true, slice(body, 200, 400));
    d.dataGroup(b2.data(), (int)b2.size());
    CHECK(got.size() == 1 && got[0].tid == 0x42 && got[0].body == body && got[0].name == "slide1.jpg" && !got[0].fromDirectory, "object: %zu", got.size());
    d.dataGroup(b2.data(), (int)b2.size());
    d.dataGroup(ha.data(), (int)ha.size());
    CHECK(got.size() == 1, "a repeat of a finished object is not a new one");
    // the same transport id with another header: a new object (the carousel went on)
    Bytes h2 = h; h2[11] = 'X';                                      // name "sXide1.jpg"
    const Bytes n0 = group(3, 0x0042, 0, true, h2);
    d.dataGroup(n0.data(), (int)n0.size());
    const Bytes nb = group(4, 0x0042, 0, true, body);
    d.dataGroup(nb.data(), (int)nb.size());
    CHECK(got.size() == 2 && got[1].name == "sXide1.jpg", "new header under the same id: %zu", got.size());
    // a body of another size than the header says is dropped
    const Bytes sh = group(3, 0x0043, 0, true, h);
    d.dataGroup(sh.data(), (int)sh.size());
    const Bytes sb = group(4, 0x0043, 0, true, slice(body, 0, 100));
    d.dataGroup(sb.data(), (int)sb.size());
    CHECK(got.size() == 2, "wrong body size");
    // scrambled bodies and compressed directories are counted, not decoded
    const Bytes sc = group(5, 0x0044, 0, true, slice(body, 0, 10));
    const Bytes cd = group(7, 0x0045, 0, true, slice(body, 0, 10));
    d.dataGroup(sc.data(), (int)sc.size()); d.dataGroup(cd.data(), (int)cd.size());
    CHECK(d.stats().scrambled == 1 && d.stats().compressed == 1, "scrambled %llu compressed %llu", (unsigned long long)d.stats().scrambled, (unsigned long long)d.stats().compressed);
}

// directory mode: one directory object (type 6) announces two objects, their bodies (type 4) come before and after it
static void testDirectory() {
    MotDecoder d;
    std::vector<MotObject> got;
    d.setSink([&](const MotObject& o) { got.push_back(o); });
    auto header = [](const std::string& name, size_t bodySize, int ctype, int sub) {
        Bytes h(7, 0);
        const size_t hs = 7 + 2 + 1 + name.size();
        h[0] = (uint8_t)(bodySize >> 20); h[1] = (uint8_t)(bodySize >> 12); h[2] = (uint8_t)(bodySize >> 4);
        h[3] = (uint8_t)(((bodySize & 15) << 4) | (hs >> 9)); h[4] = (uint8_t)(hs >> 1);
        h[5] = (uint8_t)(((hs & 1) << 7) | (ctype << 1) | (sub >> 8)); h[6] = (uint8_t)sub;
        h.push_back(0xCC); h.push_back((uint8_t)(name.size() + 1)); h.push_back(0x00);
        h.insert(h.end(), name.begin(), name.end());
        return h;
    };
    const Bytes ha = header("index.html", 20, 1, 2), hb = header("logo.png", 12, 2, 3);
    Bytes dir = {0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00};   // size patched, 2 objects, no carousel period, segment size 100, no extension
    dir.push_back(0x00); dir.push_back(0x11); dir.insert(dir.end(), ha.begin(), ha.end());
    dir.push_back(0x00); dir.push_back(0x12); dir.insert(dir.end(), hb.begin(), hb.end());
    dir[3] = (uint8_t)dir.size();
    const Bytes ba(20, 'h'), bb(12, 'p');
    const Bytes b2 = group(4, 0x12, 0, true, bb);
    d.dataGroup(b2.data(), (int)b2.size());                                // a body before the directory is kept
    CHECK(got.empty(), "no object without a header");
    const Bytes d0 = group(6, 0x0001, 1, true, slice(dir, 40, dir.size()));
    const Bytes d1 = group(6, 0x0001, 0, false, slice(dir, 0, 40));
    d.dataGroup(d0.data(), (int)d0.size());
    d.dataGroup(d1.data(), (int)d1.size());
    CHECK(d.stats().directories == 1 && d.directory().size() == 2, "directory: %llu, %zu entries", (unsigned long long)d.stats().directories, d.directory().size());
    CHECK(d.directory().size() == 2 && d.directory()[0].name == "index.html" && d.directory()[1].name == "logo.png" && d.directory()[0].tid == 0x11, "entries");
    CHECK(got.size() == 1 && got[0].name == "logo.png" && got[0].fromDirectory && got[0].tid == 0x12, "the early body completes with the directory: %zu", got.size());
    const Bytes b1 = group(4, 0x11, 0, true, ba);
    d.dataGroup(b1.data(), (int)b1.size());
    CHECK(got.size() == 2 && got[1].name == "index.html" && got[1].body == ba && std::string(motContentTypeName(got[1].contentType, got[1].subtype)) == "text/HTML", "second object");
}

// the DRM packet layer
static Bytes packet(int len, bool first, bool last, int id, int ci, const Bytes& data, bool padded = false) {
    Bytes p((size_t)len + 3, 0);
    p[0] = (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | (id << 4) | (padded ? 8 : 0) | ci);
    size_t at = 1;
    if (padded) p[at++] = (uint8_t)data.size();
    std::memcpy(&p[at], data.data(), data.size());
    const uint32_t c = drm::crc16Bytes(p.data(), (size_t)len + 1);
    p[(size_t)len + 1] = (uint8_t)(c >> 8); p[(size_t)len + 2] = (uint8_t)c;
    return p;
}

static void testPackets() {
    const int L = 10;
    DrmPacketDemux dm;
    dm.configure(L, 1);
    std::vector<Bytes> units;
    auto feed = [&](const std::vector<Bytes>& pk) {
        Bytes lf;
        for (const Bytes& p : pk) lf.insert(lf.end(), p.begin(), p.end());
        dm.feed(lf.data(), lf.size(), [&](const uint8_t* g, size_t n) { units.push_back(Bytes(g, g + n)); });
    };
    const Bytes a(L, 'a'), b(L, 'b'), c(4, 'c');
    // a data unit over three packets, the last one padded
    feed({packet(L, true, false, 1, 0, a), packet(L, false, false, 1, 1, b), packet(L, false, true, 1, 2, c, true)});
    CHECK(units.size() == 1 && units[0].size() == 24 && units[0][0] == 'a' && units[0][10] == 'b' && units[0][23] == 'c', "unit over three packets: %zu", units.size());
    CHECK(dm.unitsOk == 1 && dm.packets == 3 && dm.lostPackets == 0, "counters");
    // a single packet unit; an empty (padding) packet between; the index runs through them
    feed({packet(L, true, true, 1, 3, Bytes(3, 'd'), true), packet(L, true, true, 1, 4, Bytes(), true), packet(L, true, true, 1, 5, Bytes(L, 'e'))});
    CHECK(units.size() == 3 && units[1] == Bytes(3, 'd') && units[2] == Bytes(L, 'e') && dm.emptyPackets == 1, "single and empty packets: %zu units, %llu empty", units.size(), (unsigned long long)dm.emptyPackets);
    // a lost middle packet (index 7 is missing): the unit is dropped, the loss counted, the next unit is fine
    feed({packet(L, true, false, 1, 6, a), packet(L, false, true, 1, 0, c, true)});
    CHECK(units.size() == 3 && dm.lostPackets == 1 && dm.unitsDropped == 1, "lost packet: units %zu lost %llu dropped %llu", units.size(), (unsigned long long)dm.lostPackets, (unsigned long long)dm.unitsDropped);
    feed({packet(L, true, true, 1, 1, Bytes(L, 'f'))});
    CHECK(units.size() == 4, "the unit after the loss");
    // a wrong CRC drops the packet, the unit and (through the index gap) counts a loss
    Bytes badp = packet(L, false, true, 1, 3, c, true);
    feed({packet(L, true, false, 1, 2, a)});
    badp[3] ^= 0x10;
    feed({badp});
    CHECK(dm.badPackets == 1 && dm.unitsDropped == 2 && units.size() == 4, "bad CRC: bad %llu dropped %llu", (unsigned long long)dm.badPackets, (unsigned long long)dm.unitsDropped);
    // a continuation without its start is an orphan; the 3 bit index wraps
    const uint64_t orph0 = dm.orphanPackets;   // the end of the unit that lost its middle was one already
    CHECK(orph0 == 1, "orphan after the loss: %llu", (unsigned long long)orph0);
    feed({packet(L, false, true, 1, 5, c, true)});
    CHECK(dm.orphanPackets == orph0 + 1 && units.size() == 4, "orphan: %llu", (unsigned long long)dm.orphanPackets);
    std::vector<Bytes> run;
    for (int i = 0; i < 10; i++) run.push_back(packet(L, true, true, 1, (6 + i) & 7, Bytes(L, (uint8_t)('0' + i))));
    feed(run);
    CHECK(units.size() == 14 && units[13] == Bytes(L, '9'), "ten units across the index wrap: %zu", units.size());
    // packets of another Packet Id are somebody else's; a first packet while a unit is open restarts it
    const uint64_t drop0 = dm.unitsDropped;
    feed({packet(L, true, false, 2, 0, a), packet(L, true, false, 1, 0, a), packet(L, true, true, 1, 1, b)});
    CHECK(dm.otherIdPackets == 1 && dm.unitsDropped == drop0 + 1 && units.size() == 15 && units[14] == Bytes(L, 'b'), "other id and restart");
    // a padded packet whose length does not fit is damaged
    Bytes fit = packet(L, true, true, 1, 3, a, true);
    fit[1] = 200;
    const uint32_t cc = drm::crc16Bytes(fit.data(), (size_t)L + 1);
    fit[(size_t)L + 1] = (uint8_t)(cc >> 8); fit[(size_t)L + 2] = (uint8_t)cc;
    const uint64_t bad0 = dm.badPackets;
    feed({fit});
    CHECK(dm.badPackets == bad0 + 1, "padded length too big");
}

static void testNames() {
    CHECK(std::string(drmAppName(0, 2)) == "MOT slide show" && std::string(drmAppName(0, 3)) == "MOT broadcast website", "app names");
    CHECK(std::string(drmAppName(0, 4)) == "TPEG" && std::string(drmAppName(0, 7)) == "EPG" && std::string(drmAppName(0, 0x44A)) == "Journaline", "app names 2");
    // the application information entity: round trip through the SDC writer and parser
    drm::SdcApp a;
    a.present = true; a.shortId = 2; a.streamId = 1; a.packet = true; a.dataUnit = true; a.packetId = 3; a.packetLen = 45; a.domain = 0;
    a.appData = {0x00, 0x02};
    std::vector<uint8_t> field;
    drm::sdcPutEntity(field, 5, 0, drm::sdcEntityApp(a));
    drm::SdcInfo info;
    CHECK(drm::sdcParse(field.data(), (int)field.size(), info) == 1, "parse");
    const drm::SdcApp& r = info.app[2];
    CHECK(r.present && r.packet && r.dataUnit && r.packetId == 3 && r.packetLen == 45 && r.streamId == 1 && r.dabUserApp() == 2, "app info round trip");
    // stream mode data service: said as such, not decoded
    drm::SdcApp s;
    s.present = true; s.shortId = 1; s.streamId = 0; s.packet = false; s.domain = 0; s.appData = {0x00, 0x03};
    DrmDataService svc;
    svc.configure(s);
    DrmDataView v;
    svc.fill(v);
    CHECK(!v.packet && !v.decoded && v.modeText == "stream mode data, not decoded" && v.appName == "MOT broadcast website", "stream mode: '%s'", v.modeText.c_str());
    svc.configure(a);
    svc.fill(v);
    CHECK(v.packet && v.dataUnit && !v.decoded == false && v.modeText.find("packet mode, data units, packet id 3") == 0, "packet mode: '%s' decoded %d", v.modeText.c_str(), (int)v.decoded);
}

int main() {
    testTime();
    testHeader();
    testReassembly();
    testDirectory();
    testPackets();
    testNames();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("drm mot ok\n");
    return 0;
}
