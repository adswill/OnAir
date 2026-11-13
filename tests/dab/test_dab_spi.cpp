// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB SPI (programme guide, ETSI TS 102 371 / TS 102 818): the binary decoder against the example of TS 102 371 annex C, the token table, times and
// durations, plain XML, merging of basic and advanced objects, now / next, XMLTV, persistence, the MOT directory carousel, malformed input and an
// optional smoke test with a real recording (skipped when the file is absent).
#include "dect2/common/inflate.h"
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/dab/dab_spi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <unistd.h>
#include <string>
#include <vector>

using namespace dect2;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef std::vector<uint8_t> Bytes;

// ---- a tiny encoder for the TS 102 371 tag-length-value structure
static void lenField(Bytes& o, size_t n) {
    if (n < 0xFE) o.push_back((uint8_t)n);
    else if (n < 0x10000) { o.push_back(0xFE); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)n); }
    else { o.push_back(0xFF); o.push_back((uint8_t)(n >> 16)); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)n); }
}
static Bytes tlv(int tag, const Bytes& v) { Bytes o{(uint8_t)tag}; lenField(o, v.size()); o.insert(o.end(), v.begin(), v.end()); return o; }
static Bytes cat(std::initializer_list<Bytes> l) { Bytes o; for (const Bytes& b : l) o.insert(o.end(), b.begin(), b.end()); return o; }
static Bytes str(const std::string& s) { return Bytes(s.begin(), s.end()); }
static Bytes cdata(const std::string& s) { return tlv(0x01, str(s)); }
static Bytes timeShort(int mjd, int h, int m, int lto = -999) {
    uint32_t w = ((uint32_t)mjd << 14) | ((lto != -999 ? 1u : 0u) << 12) | ((uint32_t)h << 6) | (uint32_t)m;
    Bytes b{(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
    if (lto != -999) b.push_back((uint8_t)((lto < 0 ? 0x20 : 0) | ((lto < 0 ? -lto : lto) & 31)));
    return b;
}
static Bytes bearerDab(int ecc, int eid, uint32_t sid, int scids = 0, bool sid32 = false) {
    Bytes b{(uint8_t)(0x40 | (sid32 ? 0x10 : 0) | scids), (uint8_t)ecc, (uint8_t)(eid >> 8), (uint8_t)eid};
    if (sid32) { b.push_back((uint8_t)(sid >> 24)); b.push_back((uint8_t)(sid >> 16)); }
    b.push_back((uint8_t)(sid >> 8)); b.push_back((uint8_t)sid);
    return b;
}

// TS 102 371 annex C, table C.1
static Bytes annexC() {
    const char* hexs =
        "02 52 21 50 24 16 80 04 33 BF C4 40 81 04 33 BF C4 80 25 08 80 06 40 E1 CE 15 C2 24 1C 36 81 03 FA E4 51 80 1B 63 72 69 64 3A 2F 2F 62 62 "
        "63 2E 63 6F 2E 75 6B 2F 34 39 36 39 37 35 38 39 38 38 11 04 01 02 50 4D 19 0C 2C 0A 80 04 33 BF C4 40 81 02 0E 10";
    Bytes b;
    for (const char* p = hexs; *p;) {
        if (*p == ' ') { p++; continue; }
        b.push_back((uint8_t)strtoul(std::string(p, 2).c_str(), nullptr, 16));
        p += 2;
    }
    return b;
}

static void testTime() {
    int64_t u; int off;
    CHECK(spiParseTime("2003-12-18T17:00:00Z", u, off) && off == 0 && u == 1071766800, "time Z: %lld", (long long)u);
    CHECK(spiParseTime("2003-12-18T18:00:00+01:00", u, off) && off == 60 && u == 1071766800, "time +01:00");
    CHECK(spiParseTime("2003-12-18T18:00:00+01", u, off) && off == 60 && u == 1071766800, "time +01");
    CHECK(spiParseTime("2026-10-17T20:45:30.250-03:30", u, off) && off == -210 && spiFormatTime(u, off) == "2026-10-17T20:45:30-03:30", "time fraction / negative offset: %s", spiFormatTime(u, off).c_str());
    CHECK(!spiParseTime("2003-13-18T17:00:00Z", u, off), "month 13");
    CHECK(!spiParseTime("garbage", u, off), "garbage time");
    CHECK(spiMjdToUtc(52991, 17, 0, 0) == 1071766800, "MJD 52991 = 2003-12-18");
    CHECK(spiMjdToUtc(40587, 0, 0, 0) == 0, "MJD 40587 = 1970-01-01");
    int s;
    CHECK(spiParseDuration("PT1H", s) && s == 3600, "PT1H");
    CHECK(spiParseDuration("PT1H30M15S", s) && s == 5415, "PT1H30M15S");
    CHECK(spiParseDuration("P1DT2H", s) && s == 93600, "P1DT2H");
    CHECK(spiParseDuration("PT90M", s) && s == 5400, "PT90M");
    CHECK(!spiParseDuration("1H", s) && !spiParseDuration("PT", s) && !spiParseDuration("PT1X", s), "bad durations");
}

static void testAnnexC() {
    SpiNode t;
    CHECK(spiParseBinary(annexC().data(), annexC().size(), t) && t.name == "epg", "annex C parses");
    const SpiNode* sch = t.child("schedule");
    CHECK(sch && sch->get("version") == "", "schedule without version in the example");
    const SpiNode* sc = sch ? sch->child("scope") : nullptr;
    CHECK(sc && sc->get("startTime") == "2003-12-18T17:00:00Z" && sc->get("stopTime") == "2003-12-18T18:00:00Z", "scope times: %s", sc ? sc->get("startTime").c_str() : "-");
    const SpiNode* ss = sc ? sc->child("serviceScope") : nullptr;
    CHECK(ss && ss->get("id") == "dab:ce1.ce15.c224.0", "service scope: %s", ss ? ss->get("id").c_str() : "-");
    const SpiNode* pr = sch ? sch->child("programme") : nullptr;
    CHECK(pr && pr->get("shortId") == "16442449" && pr->get("id") == "crid://bbc.co.uk/4969758988", "programme ids");
    CHECK(pr && pr->child("mediumName") && pr->child("mediumName")->text == "PM", "medium name");
    const SpiNode* loc = pr ? pr->child("location") : nullptr;
    const SpiNode* tm = loc ? loc->child("time") : nullptr;
    CHECK(tm && tm->get("time") == "2003-12-18T17:00:00Z" && tm->get("duration") == "PT3600S", "time element");
    SpiStore st;
    CHECK(st.addTree(t), "added");
    const SpiSnapshot s = st.snapshot();
    CHECK(s.events.size() == 1 && s.events[0].sid == 0xC224 && s.events[0].start == 1071766800 && s.events[0].duration == 3600, "event");
    CHECK(s.programmes.size() == 1 && s.programmes.begin()->second.title() == "PM", "programme title");
    const SpiNowNext nn = st.nowNext(0xC224, 1071766800 + 600);
    CHECK(nn.haveNow && !nn.haveNext && nn.now.progKey == "s16442449", "now");
    const SpiNowNext n2 = st.nowNext(0xC224, 1071766800 - 600);
    CHECK(!n2.haveNow && n2.haveNext, "next before the start");
    const SpiNowNext n3 = st.nowNext(0xC224, 1071766800 + 7200);
    CHECK(!n3.haveNow && !n3.haveNext, "nothing after the end");
    MotObject o;
    o.contentType = 7; o.subtype = 1; o.body = annexC();
    SpiStore st2;
    CHECK(st2.addObject(o) && st2.stats().piObjects == 1 && st2.stats().binary == 1, "as a MOT object");
}

// service information in binary: token table, default language, ensemble, services with 16 and 32 bit ids, logos, extended length
static Bytes siBinary() {
    Bytes mm1 = tlv(0x2B, cat({tlv(0x82, str("25")), tlv(0x83, {0x04}), tlv(0x84, {0x00, 0x20}), tlv(0x85, {0x00, 0x20}), tlv(0x80, str("image/png"))}));
    Bytes mm2 = tlv(0x2B, cat({tlv(0x82, str("http://logo.example/r.png")), tlv(0x83, {0x06}), tlv(0x84, {0x01, 0x40}), tlv(0x85, {0x00, 0xF0})}));
    Bytes svc1 = tlv(0x28, cat({tlv(0x2D, tlv(0x80, bearerDab(0xE1, 0xF09F, 0xF84A))), tlv(0x10, cdata("PAC")), tlv(0x11, cdata("\x01 FM")), tlv(0x12, cdata("\x01 Radio, the \x02 station")), tlv(0x13, cat({mm1, mm2}))}));
    Bytes svc2 = tlv(0x28, cat({tlv(0x2D, tlv(0x80, bearerDab(0xE1, 0xF09F, 0xE1F09FF0u, 0, true))), tlv(0x11, cdata("WestCast SPI"))}));
    Bytes ens = tlv(0x26, cat({tlv(0x80, {0xE1, 0xF0, 0x9F}), svc1, svc2}));
    Bytes tokens = tlv(0x04, cat({Bytes{0x01, 0x07}, str("Pacific"), Bytes{0x02, 0x04}, str("best")}));
    return tlv(0x03, cat({tlv(0x80, {0x00, 0x05}), tokens, tlv(0x06, str("en")), ens}));
}

static void testServiceInformation() {
    const Bytes b = siBinary();
    SpiNode t;
    CHECK(spiParseBinary(b.data(), b.size(), t), "SI parses");
    SpiStore st;
    CHECK(st.addTree(t), "SI added");
    const SpiSnapshot s = st.snapshot();
    CHECK(s.ecc == 0xE1 && s.eid == 0xF09F, "ensemble id");
    CHECK(s.services.size() == 2, "two services");
    auto it = s.services.find(0xF84A);
    CHECK(it != s.services.end() && it->second.shortName == "PAC" && it->second.mediumName == "Pacific FM" && it->second.longName == "Pacific Radio, the best station", "names with tokens: %s / %s",
          it != s.services.end() ? it->second.mediumName.c_str() : "-", it != s.services.end() ? it->second.longName.c_str() : "-");
    CHECK(it != s.services.end() && it->second.media.size() == 2 && it->second.media[0].url == "25" && it->second.media[0].type == "logo_colour_square" && it->second.media[0].w == 32 && it->second.media[0].mime == "image/png"
          && it->second.media[1].type == "logo_colour_rectangle" && it->second.media[1].w == 320 && it->second.media[1].h == 240, "logos");
    auto j = s.services.find(0xE1F09FF0u);
    CHECK(j != s.services.end() && j->second.display() == "WestCast SPI", "32 bit service id");
}

// programme information: basic + advanced, genre, enums, long form time with LTO, extended lengths
static Bytes programmeBinary(bool advanced) {
    const std::string longText(400, 'x');   // forces the extended length field
    Bytes pr;
    if (!advanced) {
        pr = tlv(0x1C, cat({tlv(0x81, {0x01, 0x02, 0x03}), tlv(0x82, {0x00, 0x01}), tlv(0x84, {0x01}), tlv(0x11, cdata("News")),
                            tlv(0x19, cat({tlv(0x2C, cat({tlv(0x80, timeShort(61330, 17, 0, 4)), tlv(0x81, {0x0E, 0x10})}))}))}));
    } else {
        pr = tlv(0x1C, cat({tlv(0x81, {0x01, 0x02, 0x03}), tlv(0x82, {0x00, 0x01}), tlv(0x83, {0x02}), tlv(0x12, cdata("The News Hour")), tlv(0x13, cat({tlv(0x1A, cdata("Headlines")), tlv(0x1B, cdata(longText))})),
                            tlv(0x14, cat({tlv(0x80, {0x03, 0x01, 0x02}), tlv(0x81, {0x01})}))}));
    }
    Bytes scope = tlv(0x24, cat({tlv(0x80, timeShort(61330, 15, 0)), tlv(0x81, timeShort(61330, 21, 0)), tlv(0x25, tlv(0x80, bearerDab(0xE1, 0xCE15, 0xC224))) }));
    return tlv(0x02, tlv(0x21, cat({tlv(0x80, {0x00, 0x01}), scope, pr})));
}

static void testProgrammeInformation() {
    SpiStore st;
    for (int adv = 0; adv < 2; adv++) {
        const Bytes b = programmeBinary(adv);
        SpiNode t;
        CHECK(spiParseBinary(b.data(), b.size(), t), "PI parses (%d)", adv);
        st.addTree(t);
    }
    const SpiSnapshot s = st.snapshot();
    CHECK(s.programmes.size() == 1 && s.events.size() == 1, "merged into one programme: %zu / %zu", s.programmes.size(), s.events.size());
    const SpiProgramme& p = s.programmes.begin()->second;
    CHECK(p.shortId == 0x010203 && p.title() == "News" && p.longName == "The News Hour" && p.shortDesc == "Headlines" && p.longDesc.size() == 400 && p.recommended, "merged fields");
    CHECK(p.genres.size() == 1 && p.genres[0] == "3.1.2", "genre %s", p.genres.empty() ? "-" : p.genres[0].c_str());
    const SpiEvent& e = s.events[0];
    // MJD 61330 = 2026-10-17; 17:00 UTC with a +2 h offset; the scope serviceScope gives the service
    CHECK(e.sid == 0xC224 && e.start == spiMjdToUtc(61330, 17, 0, 0) && e.duration == 3600 && e.offMin == 120, "event %08X %lld +%d off %d", (unsigned)e.sid, (long long)e.start, e.duration, e.offMin);
    CHECK(spiFormatTime(e.start, e.offMin) == "2026-10-17T19:00:00+02:00", "local time %s", spiFormatTime(e.start, e.offMin).c_str());
}

static const char* kXmlSi =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<serviceInformation xmlns=\"http://www.worlddab.org/schemas/spi/31\" version=\"3\">"
    "<services><service><bearer id=\"dab:ce1.ce15.c224.0\"/><mediumName xml:lang=\"en\">Radio &amp; Co</mediumName><longName>Radio &lt;Co&gt; &#233;</longName>"
    "<mediaDescription><multimedia type=\"logo_colour_square\" url=\"sq.png\" width=\"32\" height=\"32\"/></mediaDescription><genre href=\"urn:tva:metadata:cs:ContentCS:2002:3.6.8\"/></service>"
    "<service><bearer id=\"fm:ce1.c224.09580\"/><bearer id=\"dab:ce1.ce15.c225.0\"/><mediumName>Second</mediumName></service></services></serviceInformation>";
static const char* kXmlPi =
    "<epg xmlns=\"http://www.worlddab.org/schemas/spi/31\"><!-- c --><schedule version=\"1\"><scope startTime=\"2026-10-17T18:00:00+02:00\" stopTime=\"2026-10-17T20:00:00+02:00\">"
    "<serviceScope id=\"dab:ce1.ce15.c224.0\"/></scope>"
    "<programme shortId=\"101\" id=\"crid://x/1\" version=\"1\"><mediumName>First</mediumName><longName>First show</longName><mediaDescription><shortDescription>Short.</shortDescription>"
    "<longDescription><![CDATA[Long <b> & text]]></longDescription></mediaDescription><genre href=\"urn:tva:metadata:cs:ContentCS:2002:3.6.8\"/>"
    "<location><time time=\"2026-10-17T18:00:00+02:00\" duration=\"PT1H\"/></location></programme>"
    "<programme shortId=\"102\" id=\"crid://x/2\"><mediumName>Second</mediumName><location><time time=\"2026-10-17T19:00:00+02:00\" duration=\"PT1H\"/></location></programme>"
    "</schedule></epg>";

static void testXml() {
    SpiStore st;
    MotObject a, b;
    a.contentType = 5; a.body = str(kXmlSi);
    b.contentType = 5; b.body = str(kXmlPi);
    CHECK(st.addObject(a) && st.addObject(b), "xml objects accepted");
    const SpiSnapshot s = st.snapshot();
    auto it = s.services.find(0xC224);
    CHECK(it != s.services.end() && it->second.mediumName == "Radio & Co" && it->second.longName == "Radio <Co> \xC3\xA9" && it->second.genre == "3.6.8" && it->second.media.size() == 1, "service from XML");
    CHECK(s.services.size() == 2 && s.services.count(0xC225), "dab bearer only, fm skipped");
    CHECK(s.events.size() == 2, "events %zu", s.events.size());
    const int64_t t0 = spiMjdToUtc(61330, 16, 0, 0);   // 18:00 +02:00
    const SpiNowNext nn = st.nowNext(0xC224, t0 + 1800);
    CHECK(nn.haveNow && nn.haveNext && s.programmes.at(nn.now.progKey).title() == "First" && s.programmes.at(nn.next.progKey).title() == "Second", "now / next");
    CHECK(s.programmes.at("s101").longDesc == "Long <b> & text" && s.programmes.at("s101").shortDesc == "Short.", "descriptions with CDATA");
    CHECK(s.programmes.at("s101").genres.size() == 1 && s.programmes.at("s101").genres[0] == "3.6.8", "genre");
    const std::vector<SpiEvent> up = st.upcoming(0xC224, t0 + 4000);
    CHECK(up.size() == 1 && up[0].progKey == "s102", "upcoming");
    // XMLTV
    const std::string x = st.toXmltv();
    CHECK(x.find("<channel id=\"c224.dab\">") != std::string::npos && x.find("<display-name>Radio &amp; Co</display-name>") != std::string::npos, "xmltv channel");
    CHECK(x.find("<programme start=\"20261017180000 +0200\" stop=\"20261017190000 +0200\" channel=\"c224.dab\">") != std::string::npos, "xmltv programme");
    CHECK(x.find("<desc>Long &lt;b&gt; &amp; text</desc>") != std::string::npos && x.find("<category>3.6.8</category>") != std::string::npos, "xmltv escaping");
    SpiNode chk;
    CHECK(spiParseXml(x.data(), x.size(), chk) && chk.name == "tv", "xmltv is well-formed XML");
    size_t channels = 0, progs = 0;
    for (const SpiNode& k : chk.kids) { channels += k.name == "channel"; progs += k.name == "programme"; }
    CHECK(channels == 1 && progs == 2, "xmltv counts %zu / %zu", channels, progs);
    CHECK(st.toXmltv(t0 + 3700, t0 + 7200).find("<title>First") == std::string::npos, "xmltv range filter");
    // prune
    st.prune(t0 + 86400 * 2);
    CHECK(st.snapshot().events.empty() && st.snapshot().programmes.empty(), "pruned");
}

static void testPersistence() {
    SpiStore st;
    MotObject a, b;
    a.contentType = 5; a.body = str(kXmlSi);
    b.contentType = 5; b.body = str(kXmlPi);
    st.addObject(a); st.addObject(b);
    SpiStore st2;
    CHECK(st2.deserialize(st.serialize()), "deserialize");
    CHECK(st2.serialize() == st.serialize(), "round trip is identical");
    CHECK(st2.snapshot().programmes.at("s101").longDesc == "Long <b> & text", "text with special characters");
    CHECK(!st2.deserialize("not a cache"), "rejects another file");
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / ("spi_test_" + std::to_string(getpid()))).string();
    CHECK(spiSave(st, dir, 0xCE15), "save");
    SpiStore st3;
    CHECK(spiLoad(st3, dir, 0xCE15, spiMjdToUtc(61330, 16, 0, 0)), "load");
    CHECK(st3.snapshot().events.size() == 2, "events after load");
    SpiStore st4;
    CHECK(!spiLoad(st4, dir, 0x1234, 0), "no file for another ensemble");
    for (int i = 0; i < 40; i++) spiSave(st, dir, (uint16_t)(0x1000 + i));
    spiCleanCache(dir, 0, 14, 10);
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) { (void)e; n++; }
    CHECK(n == 10, "cache bounded to 10 files, has %zu", n);
    fs::remove_all(dir);
}

// ---- MOT directory mode carousel through DabSpiDecoder
static Bytes group(int type, uint16_t tid, int segNo, bool last, const Bytes& seg) {
    Bytes g = {(uint8_t)(0x40 | 0x20 | 0x10 | type), 0x00, (uint8_t)((last ? 0x80 : 0) | (segNo >> 8)), (uint8_t)segNo, 0x12, (uint8_t)(tid >> 8), (uint8_t)tid,
               (uint8_t)(seg.size() >> 8), (uint8_t)seg.size()};
    g.insert(g.end(), seg.begin(), seg.end());
    const uint16_t c = dabgen::crc16(g.data(), (int)g.size());
    g.push_back((uint8_t)(c >> 8)); g.push_back((uint8_t)c);
    return g;
}
static Bytes motHead(size_t bodySize, int ctype, int sub, const std::string& name) {
    Bytes nm = {0x0F}; nm.insert(nm.end(), name.begin(), name.end());
    Bytes ext = {(uint8_t)(0xC0 | 0x0C), (uint8_t)nm.size()};
    ext.insert(ext.end(), nm.begin(), nm.end());
    const size_t hs = 7 + ext.size();
    Bytes h = {(uint8_t)(bodySize >> 20), (uint8_t)(bodySize >> 12), (uint8_t)(bodySize >> 4), (uint8_t)((bodySize << 4) | (hs >> 9)), (uint8_t)(hs >> 1),
               (uint8_t)(((hs & 1) << 7) | (ctype << 1) | (sub >> 8)), (uint8_t)sub};
    h.insert(h.end(), ext.begin(), ext.end());
    return h;
}

static void testCarousel() {
    const Bytes si = siBinary(), pi = programmeBinary(false), pi2 = programmeBinary(true);
    // a zlib-less gzip (stored block) of the advanced object
    Bytes gz = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF, 0x01, (uint8_t)(pi2.size() & 255), (uint8_t)(pi2.size() >> 8), (uint8_t)(~pi2.size() & 255), (uint8_t)((~pi2.size() >> 8) & 255)};
    gz.insert(gz.end(), pi2.begin(), pi2.end());
    const uint32_t crc = crc32Ieee(pi2.data(), pi2.size());
    for (int i = 0; i < 4; i++) gz.push_back((uint8_t)(crc >> (8 * i)));
    for (int i = 0; i < 4; i++) gz.push_back((uint8_t)(pi2.size() >> (8 * i)));
    struct Ent { uint16_t tid; Bytes head, body; };
    std::vector<Ent> ents = {{100, motHead(si.size(), 7, 0, "0"), si}, {101, motHead(pi.size(), 7, 1, "1"), pi}, {102, motHead(gz.size(), 7, 1, "2"), gz}, {103, motHead(4, 2, 3, "3"), Bytes{1, 2, 3, 4}}};
    // the directory: 4 bytes size (flag 0), 2 bytes count, 3 bytes period, 2 bytes segment size, 2 bytes extension length, entries
    Bytes dir = {0, 0, 0, 0, 0, (uint8_t)ents.size(), 0, 0, 10, 0x20, 0, 0, 0};
    for (const Ent& e : ents) { dir.push_back((uint8_t)(e.tid >> 8)); dir.push_back((uint8_t)e.tid); dir.insert(dir.end(), e.head.begin(), e.head.end()); }
    DabSpiDecoder dec;
    int taps = 0;
    dec.setObjectTap([&](const MotObject&) { taps++; });
    auto sendCarousel = [&](bool withDir) {
        // body segments in two parts for the first object
        if (withDir) { const Bytes g = group(6, 7, 0, true, dir); dec.dataGroup(g.data(), g.size()); }
        for (const Ent& e : ents) {
            const size_t half = e.body.size() / 2;
            if (e.body.size() > 8) {
                const Bytes a = group(4, e.tid, 0, false, Bytes(e.body.begin(), e.body.begin() + half)), b = group(4, e.tid, 1, true, Bytes(e.body.begin() + half, e.body.end()));
                dec.dataGroup(a.data(), a.size()); dec.dataGroup(b.data(), b.size());
            } else { const Bytes g = group(4, e.tid, 0, true, e.body); dec.dataGroup(g.data(), g.size()); }
        }
    };
    sendCarousel(false);
    CHECK(dec.progress().percent < 0 && dec.progress().expected == 0, "no directory yet: progress unknown");
    sendCarousel(true);
    DabSpiDecoder::Progress p = dec.progress();
    CHECK(p.expected == 4, "directory announces %d", p.expected);
    sendCarousel(true);
    p = dec.progress();
    CHECK(p.have == 4 && p.percent > 99.9, "all received: %d of %d", p.have, p.expected);
    const SpiSnapshot s = dec.store().snapshot();
    CHECK(s.services.size() == 2 && s.events.size() == 1 && s.programmes.begin()->second.longName == "The News Hour", "model from the carousel: %zu services %zu events", s.services.size(), s.events.size());
    int sub; Bytes img;
    CHECK(dec.image("3", sub, img) && sub == 3 && img.size() == 4, "picture kept");
    const uint64_t ch = dec.progress().changeCount;
    sendCarousel(true);
    CHECK(dec.progress().changeCount == ch, "a repeat of the carousel changes nothing");
    CHECK(dec.store().stats().compressed == 1, "gzip object decoded");
    dec.clearStore();
    CHECK(dec.store().snapshot().services.empty() && !dec.image("3", sub, img), "cleared");
    (void)taps;
}

// ---- malformed input: never a crash, always bounded
static void testFuzz() {
    std::mt19937 rng(1234);
    const Bytes seeds[] = {annexC(), siBinary(), programmeBinary(false), programmeBinary(true)};
    for (const Bytes& seed : seeds) {
        for (size_t n = 0; n <= seed.size(); n++) {   // every truncation
            SpiNode t;
            spiParseBinary(seed.data(), n, t);
        }
        for (int i = 0; i < 6000; i++) {
            Bytes m = seed;
            const int k = 1 + (int)(rng() % 6);
            for (int j = 0; j < k; j++) {
                const size_t pos = rng() % m.size();
                switch (rng() % 4) {
                case 0: m[pos] = (uint8_t)rng(); break;
                case 1: m[pos] ^= (uint8_t)(1u << (rng() % 8)); break;
                case 2: m[pos] = 0xFF; break;
                default: if (m.size() > 4) m.erase(m.begin() + (long)pos); break;
                }
            }
            SpiNode t;
            if (spiParseBinary(m.data(), m.size(), t)) { SpiStore st; st.addTree(t); st.toXmltv(); st.serialize(); }
        }
    }
    for (int i = 0; i < 3000; i++) {   // random bytes, with a plausible start
        Bytes m(rng() % 300);
        for (uint8_t& b : m) b = (uint8_t)rng();
        if (!m.empty() && (i & 1)) m[0] = 0x02 + (i & 2 ? 1 : 0);
        SpiNode t;
        spiParseBinary(m.data(), m.size(), t);
        MotObject o; o.contentType = 7; o.body = m;
        SpiStore st; st.addObject(o);
    }
    // nesting that would overflow the stack, a huge extended length, an endless element count
    Bytes deep;
    for (int i = 0; i < 5000; i++) { deep.push_back(0x1C); deep.push_back(0xFE); deep.push_back(0xFF); deep.push_back(0xFF); }
    deep.insert(deep.begin(), {0x02, 0xFF, 0xFF, 0xFF, 0xFF});
    SpiNode t;
    CHECK(!spiParseBinary(deep.data(), deep.size(), t), "absurd lengths rejected");
    Bytes nest = {0x02};
    Bytes inner;
    for (int i = 0; i < 200; i++) inner = tlv(0x1C, inner);
    nest = tlv(0x02, inner);
    CHECK(!spiParseBinary(nest.data(), nest.size(), t), "deep nesting rejected");
    std::string xml = "<epg>";
    for (int i = 0; i < 3000; i++) xml += "<schedule>";
    CHECK(!spiParseXml(xml.data(), xml.size(), t), "deep XML rejected");
    // truncated / mutated XML
    const std::string src = kXmlPi;
    for (size_t n = 0; n <= src.size(); n += 3) { SpiNode x; spiParseXml(src.data(), n, x); }
    for (int i = 0; i < 4000; i++) {
        std::string m = src;
        for (int j = 0; j < 4; j++) m[rng() % m.size()] = (char)rng();
        SpiNode x;
        if (spiParseXml(m.data(), m.size(), x)) { SpiStore st; st.addTree(x); }
    }
    // a store never grows without a bound
    SpiStore st;
    for (int i = 0; i < 70; i++) {
        std::string x = "<epg><schedule><scope startTime=\"2026-10-17T00:00:00Z\" stopTime=\"2026-10-18T00:00:00Z\"><serviceScope id=\"dab:ce1.ce15.c224.0\"/></scope>";
        for (int k = 0; k < 1000; k++) x += "<programme shortId=\"" + std::to_string(i * 1000 + k + 1) + "\"><mediumName>p</mediumName><location><time time=\"2026-10-17T00:00:00Z\" duration=\"PT1M\"/></location></programme>";
        x += "</schedule></epg>";
        MotObject o; o.contentType = 5; o.body = str(x);
        st.addObject(o);
    }
    CHECK(st.snapshot().programmes.size() <= 20000 && st.snapshot().events.size() <= 60000, "store bounded: %zu programmes", st.snapshot().programmes.size());
}

// optional: the owner's recording of the WestCast ensemble (not in the repository); set DAB_SPI_RECORDING to a .cs8 file at 3 Msps
static void testRecording() {
    const char* path = getenv("DAB_SPI_RECORDING");
    if (!path || !*path) { printf("recording smoke test skipped (DAB_SPI_RECORDING not set)\n"); return; }
    FILE* f = fopen(path, "rb");
    if (!f) { printf("recording smoke test skipped (file absent)\n"); return; }
    DabReceiver r;
    r.configure(3000000.0);
    r.audio().setSilent(true);
    std::vector<int8_t> q(2 * 65536);
    std::vector<cf32> x(65536);
    size_t n, total = 0;
    bool sel = false;
    while (total < 3000000ull * 100 && (n = fread(q.data(), 2, 65536, f)) > 0) {   // the first 100 seconds
        total += n;
        for (size_t i = 0; i < n; i++) x[i] = cf32(q[2 * i] / 127.f, q[2 * i + 1] / 127.f);
        r.feed(x.data(), n);
        if (!sel) {
            const DabEnsemble e = r.ensemble();
            for (const auto& kv : e.services) if (kv.second.spi)
                for (const DabComponent& c : kv.second.comps) {
                    auto pk = c.tmid == 3 ? e.packet.find(c.scid) : e.packet.end();
                    if (pk != e.packet.end() && e.subs.count(pk->second.subId)) { r.selectSpi(pk->second.subId, pk->second.address); sel = true; }
                }
        }
    }
    fclose(f);
    CHECK(sel, "the ensemble signals an SPI service");
    const SpiSnapshot s = r.spi().store().snapshot();
    CHECK(s.services.size() >= 10, "service information decoded: %zu services", s.services.size());
    printf("recording: %zu services, %zu programmes, %zu events\n", s.services.size(), s.programmes.size(), s.events.size());
}

int main() {
    testTime();
    testAnnexC();
    testServiceInformation();
    testProgrammeInformation();
    testXml();
    testPersistence();
    testCarousel();
    testFuzz();
    testRecording();
    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("dab spi: all checks passed\n");
    return 0;
}
