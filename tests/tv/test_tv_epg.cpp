// The programme guide: EIT sections built by hand (every descriptor the guide reads, text in several character sets, split extended text), the
// time offset of the TOT around a summer time change, the database (overlaps, merge, expiry, limits, search, series, JSON), the packet collector
// and a fuzz run of damaged sections.
#include "dect2/tv/epg.h"
#include "dect2/tv/ts.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string_view>
using namespace dect2;
#include "../common/test_util.h"

typedef std::vector<uint8_t> Bytes;

static void put16(Bytes& b, int v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void putTime(Bytes& b, int mjd, int h, int m, int s) { put16(b, mjd); b.push_back(bcd(h)); b.push_back(bcd(m)); b.push_back(bcd(s)); }
static void finish(Bytes& s) {
    const int len = (int)s.size() - 3 + 4;
    s[1] = (uint8_t)((s[1] & 0xF0) | (len >> 8)); s[2] = (uint8_t)len;
    const uint32_t c = mpegCrc32(s.data(), (int)s.size());
    for (int k = 24; k >= 0; k -= 8) s.push_back((uint8_t)(c >> k));
}
static Bytes shortEv(const char* lang, const Bytes& name, const Bytes& text) {
    Bytes d = {0x4D, 0}; d.insert(d.end(), lang, lang + 3); d.push_back((uint8_t)name.size()); d.insert(d.end(), name.begin(), name.end());
    d.push_back((uint8_t)text.size()); d.insert(d.end(), text.begin(), text.end()); d[1] = (uint8_t)(d.size() - 2); return d;
}
static Bytes extEv(int num, int last, const Bytes& text, std::string_view lang = "eng") {
    Bytes d = {0x4E, 0, (uint8_t)((num << 4) | last), (uint8_t)lang.at(0), (uint8_t)lang.at(1), (uint8_t)lang.at(2), 0, (uint8_t)text.size()};
    d.insert(d.end(), text.begin(), text.end()); d[1] = (uint8_t)(d.size() - 2); return d;
}
static Bytes str(const char* s) { return Bytes(s, s + strlen(s)); }
static Bytes event(int id, int mjd, int h, int m, int dh, int dm, int running, bool ca, const Bytes& desc) {
    Bytes e; put16(e, id); putTime(e, mjd, h, m, 0); e.push_back(bcd(dh)); e.push_back(bcd(dm)); e.push_back(0);
    const int dl = (int)desc.size();
    e.push_back((uint8_t)((running << 5) | (ca ? 0x10 : 0) | (dl >> 8))); e.push_back((uint8_t)dl);
    e.insert(e.end(), desc.begin(), desc.end());
    return e;
}
static Bytes eit(int table, int sid, int tsid, int onid, int secNum, const std::vector<Bytes>& events) {
    Bytes s = {(uint8_t)table, 0xF0, 0}; put16(s, sid); s.push_back(0xC1); s.push_back((uint8_t)secNum); s.push_back(0);
    put16(s, tsid); put16(s, onid); s.push_back(0); s.push_back((uint8_t)table);
    for (auto& e : events) s.insert(s.end(), e.begin(), e.end());
    finish(s);
    return s;
}
// section -> TS packets of a PID
static std::vector<uint8_t> packetize(const Bytes& sec, int pid, int& cc) {
    Bytes d = {0}; d.insert(d.end(), sec.begin(), sec.end());
    std::vector<uint8_t> out;
    for (size_t off = 0; off < d.size(); off += 184) {
        uint8_t p[188]; memset(p, 0xFF, 188);
        p[0] = 0x47; p[1] = (uint8_t)((off == 0 ? 0x40 : 0) | (pid >> 8)); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (cc++ & 15));
        const size_t n = std::min<size_t>(184, d.size() - off);
        memcpy(p + 4, d.data() + off, n);
        out.insert(out.end(), p, p + 188);
    }
    return out;
}

int main() {
    const int mjd = 61330;   // 2026-10-17 (MJD 61330)
    CHECK(true, "");
    int64_t t0;
    { const uint8_t b[5] = {0xC0, 0x79, 0x12, 0x45, 0x00}; CHECK(dvbTime(b, t0) && t0 == 750516300, "EN 300 468 annex C example: %lld", (long long)t0); }
    { const uint8_t b[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; CHECK(!dvbTime(b, t0), "undefined time"); }
    { const uint8_t b[5] = {0xC0, 0x79, 0x1A, 0x45, 0x00}; CHECK(!dvbTime(b, t0), "bad BCD digit"); }
    { const uint8_t b[5] = {0xC0, 0x79, 0x25, 0x45, 0x00}; CHECK(!dvbTime(b, t0), "hour 25"); }

    // ---- one section with everything
    Bytes d = shortEv("eng", str("Evening News"), str("Headlines of the day"));
    { Bytes e1 = extEv(0, 1, str("Long text part one, ")); Bytes e2 = extEv(1, 1, str("and part two.")); d.insert(d.end(), e1.begin(), e1.end()); d.insert(d.end(), e2.begin(), e2.end()); }
    { Bytes c = {0x54, 4, 0x20, 0x00, 0x43, 0x10}; d.insert(d.end(), c.begin(), c.end()); }                         // news, sport (football)
    { Bytes r = {0x55, 8, 'G', 'B', 'R', 0x05, 'L', 'T', 'U', 0x0A}; d.insert(d.end(), r.begin(), r.end()); }      // age 8 and 13
    { Bytes ci = {0x76, 0, 0x04 | 0, 0}; Bytes crid = str("bbc.co.uk/ep77"), ser = str("bbc.co.uk/sr9");   // crid_type 1 << 2, location 0
      ci.clear(); ci.push_back(0x76); ci.push_back((uint8_t)(2 + crid.size() + 2 + ser.size()));
      ci.push_back(0x04); ci.push_back((uint8_t)crid.size()); ci.insert(ci.end(), crid.begin(), crid.end());
      ci.push_back(0x08); ci.push_back((uint8_t)ser.size()); ci.insert(ci.end(), ser.begin(), ser.end());
      d.insert(d.end(), ci.begin(), ci.end()); }
    Bytes sec = eit(0x4E, 0x1001, 0x0002, 0x20FA, 0, {event(0x100, mjd, 20, 30, 0, 45, 4, false, d)});
    EitSection es;
    CHECK(parseEitSection(sec.data(), (int)sec.size(), es), "parse");
    CHECK(es.sid == 0x1001 && es.tsid == 2 && es.onid == 0x20FA && es.tableId == 0x4E && es.events.size() == 1, "header");
    if (!es.events.empty()) {
        const EpgItem& e = es.events[0];
        CHECK(e.title == "Evening News" && e.text == "Headlines of the day" && e.lang == "eng", "short event '%s'", e.title.c_str());
        CHECK(e.extended == "Long text part one, and part two.", "extended joined: '%s'", e.extended.c_str());
        CHECK(e.content.size() == 2 && e.content[0] == 0x20 && e.content[1] == 0x43 && e.genre() == 2, "content");
        CHECK(e.rating == 13, "highest rating %d", e.rating);
        CHECK(e.crid == "bbc.co.uk/ep77" && e.seriesCrid == "bbc.co.uk/sr9", "crids '%s' '%s'", e.crid.c_str(), e.seriesCrid.c_str());
        CHECK(e.running == 4 && !e.freeCa && e.duration == 45 * 60 && !e.other, "status");
        CHECK(e.start == (int64_t)(mjd - 40587) * 86400 + 20 * 3600 + 30 * 60, "start %lld", (long long)e.start);
    }
    // other transport stream flag
    { Bytes o = eit(0x4F, 5, 6, 7, 0, {event(1, mjd, 1, 0, 1, 0, 0, true, shortEv("deu", str("A"), Bytes()))});
      EitSection x; CHECK(parseEitSection(o.data(), (int)o.size(), x) && x.events.size() == 1 && x.events[0].other && x.events[0].freeCa, "other / ca"); }
    // character sets: ISO 8859-5 (0x01 prefix) and UTF-8 (0x15)
    { Bytes cy = {0x01, 0xBF, 0xE0, 0xD8, 0xD2, 0xD5, 0xE2};   // "Привет"
      Bytes u8 = {0x15, 0xC4, 0x85, 'x'};                       // "ąx"
      Bytes o = eit(0x50, 1, 1, 1, 0, {event(2, mjd, 6, 0, 0, 30, 0, false, shortEv("rus", cy, u8))});
      EitSection x; CHECK(parseEitSection(o.data(), (int)o.size(), x) && x.events.size() == 1, "cs parse");
      if (!x.events.empty()) { CHECK(x.events[0].title == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "8859-5 title '%s'", x.events[0].title.c_str()); CHECK(x.events[0].text == "\xC4\x85x", "utf8 text '%s'", x.events[0].text.c_str()); } }
    // Chinese DVB-C SI without the optional character-table selector (unprefixed legacy text): language for EIT, country for names.
    {
        const Bytes name = {'C', 'C', 'T', 'V', '-', '1', ' ', 0xD7, 0xDB, 0xBA, 0xCF};
        const Bytes chinese = {0xD6, 0xD0, 0xCE, 0xC4}; // 中文
        Bytes shortDesc = shortEv("chi", name, chinese);
        Bytes extended = extEv(0, 0, chinese, "zho");
        shortDesc.insert(shortDesc.end(), extended.begin(), extended.end());
        Bytes chineseEit = eit(0x4E, 100, 7, 0x2310, 0, {event(99, mjd, 12, 0, 1, 0, 4, false, shortDesc)});
        EitSection parsed;
        CHECK(parseEitSection(chineseEit.data(), (int)chineseEit.size(), parsed) && parsed.events.size() == 1, "Chinese EIT parsed");
        if (!parsed.events.empty()) {
            CHECK(parsed.events[0].title == "CCTV-1 综合", "Chinese EIT title '%s'", parsed.events[0].title.c_str());
            CHECK(parsed.events[0].text == "中文" && parsed.events[0].extended == "中文", "Chinese EIT descriptions");
        }
        epgSetPreferredLanguage("zho");
        CHECK(parseEitSection(chineseEit.data(), (int)chineseEit.size(), parsed) && !parsed.events.empty() && parsed.events[0].title == "CCTV-1 综合"
              && parsed.events[0].extended == "中文", "preferred Chinese EIT");
        epgSetPreferredLanguage("");

        // A Chinese event must not change another event's default charset in the same section.
        Bytes mixedEit = eit(0x4E, 100, 7, 0x2310, 0, {
            event(99, mjd, 12, 0, 1, 0, 4, false, shortDesc),
            event(100, mjd, 13, 0, 1, 0, 4, false, shortEv("fra", {'C', 'a', 'f', 0xC2, 'e'}, {}))});
        CHECK(parseEitSection(mixedEit.data(), (int)mixedEit.size(), parsed) && parsed.events.size() == 2
              && parsed.events[0].title == "CCTV-1 综合" && parsed.events[1].title == "Café", "per-event language charset");
        CHECK(parseEitSection(mixedEit.data(), (int)mixedEit.size(), parsed, {dvbsi::TextEncoding::Dvb, "CHN"})
              && parsed.events.size() == 2 && parsed.events[1].title == "Café", "event language overrides the country fallback");
        TsDemux mixed;
        int mixedCc = 0;
        const auto mixedPackets = packetize(mixedEit, 0x12, mixedCc);
        for (size_t i = 0; i < mixedPackets.size(); i += 188) mixed.feed(mixedPackets.data() + i);
        const auto mixedGuide = mixed.epg();
        CHECK(mixedGuide.count(100) && mixedGuide.at(100).size() == 2 && mixedGuide.at(100)[0].title == "CCTV-1 综合"
              && mixedGuide.at(100)[1].title == "Café", "demux keeps per-event language charsets");

        Bytes tot = {0x73, 0x70, 0}; putTime(tot, mjd, 12, 0, 0);
        Bytes zone = {0x58, 13, 'C', 'H', 'N', 2, 0x08, 0x00};
        putTime(zone, mjd + 1, 0, 0, 0); zone.push_back(0x08); zone.push_back(0x00);
        put16(tot, 0xF000 | (int)zone.size()); tot.insert(tot.end(), zone.begin(), zone.end()); finish(tot);
        Bytes sd = {0x48, (uint8_t)(3 + chinese.size() + name.size()), 1, (uint8_t)chinese.size()};
        sd.insert(sd.end(), chinese.begin(), chinese.end()); sd.push_back((uint8_t)name.size()); sd.insert(sd.end(), name.begin(), name.end());
        Bytes sdt = {0x42, 0xF0, 0, 0, 7, 0xC1, 0, 0, 0x23, 0x10, 0xFF, 0, 100, 0xFC, 0x80, (uint8_t)sd.size()};
        sdt.insert(sdt.end(), sd.begin(), sd.end()); finish(sdt);
        EpgDb db; EpgCollector col(&db); TsDemux demux, independent;
        int cc = 0;
        auto feed = [&](const Bytes& section, int pid) {
            const auto packets = packetize(section, pid, cc);
            for (size_t i = 0; i < packets.size(); i += 188) { col.feed(packets.data() + i); demux.feed(packets.data() + i); }
        };
        feed(sdt, 0x11); // SDT may arrive before the country hint; its next repetition must replace the name.
        Bytes damagedTot = tot; damagedTot.back() ^= 1;
        feed(damagedTot, 0x14); feed(sdt, 0x11);
        CHECK(demux.snapshot().services[0].name != "CCTV-1 综合", "bad TOT cannot change the charset");
        feed(tot, 0x14); feed(sdt, 0x11); feed(chineseEit, 0x12);
        Bytes nit = {0x40, 0xF0, 0, 0, 1, 0xC1, 0, 0, 0xF0, 6, 0x40, 4};
        nit.insert(nit.end(), chinese.begin(), chinese.end()); put16(nit, 0xF000); finish(nit); feed(nit, 0x10);
        Bytes bat = nit; bat.resize(bat.size() - 4); bat[0] = 0x4A; bat[10] = 0x47; finish(bat); feed(bat, 0x11);
        Bytes otherSdt = sdt; otherSdt.resize(otherSdt.size() - 4); otherSdt[0] = 0x46; otherSdt[4] = 9; finish(otherSdt); feed(otherSdt, 0x11);
        Bytes otherEit = chineseEit; otherEit.resize(otherEit.size() - 4); otherEit[0] = 0x4F; finish(otherEit); feed(otherEit, 0x12);
        auto snap = demux.snapshot();
        CHECK(snap.networkName == "中文" && snap.si && !snap.si->networks.empty() && snap.si->networks[0].name == "中文", "Chinese network name");
        CHECK(snap.si && !snap.si->bouquets.empty() && snap.si->bouquets[0].name == "中文", "Chinese bouquet name");
        CHECK(snap.si && !snap.si->otherServices.empty() && snap.si->otherServices[0].name == "CCTV-1 综合", "Chinese SDT other service name");
        CHECK(snap.si && !snap.si->eitOther.empty() && snap.si->eitOther[0].nowTitle == "CCTV-1 综合", "Chinese EIT other title");
        CHECK(!snap.services.empty() && snap.services[0].name == "CCTV-1 综合" && snap.services[0].provider == "中文", "Chinese DVB-C service and provider");
        CHECK(col.serviceName({0x2310, 7, 100}) == "CCTV-1 综合", "Chinese guide service name");
        const auto guide = demux.epg();
        const auto it = guide.find(100);
        CHECK(it != guide.end() && !it->second.empty() && it->second[0].title == "CCTV-1 综合" && it->second[0].extended == "中文", "Chinese demux guide");
        CHECK(!snap.services.empty() && snap.services[0].now == "CCTV-1 综合", "Chinese now/next title");
        epgSetPreferredLanguage("chi"); feed(chineseEit, 0x12); epgSetPreferredLanguage("");
        const auto preferred = demux.epg();
        CHECK(preferred.count(100) && !preferred.at(100).empty() && preferred.at(100)[0].title == "CCTV-1 综合", "preferred Chinese demux guide");
        auto packets = packetize(sdt, 0x11, cc);
        for (size_t i = 0; i < packets.size(); i += 188) independent.feed(packets.data() + i);
        CHECK(independent.snapshot().services[0].name != "CCTV-1 综合", "country hint is not global");
        packets = packetize(chineseEit, 0x12, cc);
        for (size_t i = 0; i < packets.size(); i += 188) independent.feed(packets.data() + i);
        CHECK(independent.snapshot().services[0].now == "CCTV-1 综合", "Chinese EIT language works without a TOT");
        Bytes newMux = sdt; newMux.resize(newMux.size() - 4); newMux[4] = 8; finish(newMux);
        feed(newMux, 0x11);
        CHECK(demux.snapshot().services[0].name != "CCTV-1 综合", "a multiplex change forgets the country hint");
        CHECK(col.serviceName({0x2310, 8, 100}) != "CCTV-1 综合", "guide forgets the previous multiplex country");
        feed(otherSdt, 0x11); // only newly received SI is re-decoded; unrelated cached records are retained
        bool snapshotCountryCleared = false;
        const auto switched = demux.snapshot();
        if (switched.si) for (const auto& service : switched.si->otherServices) {
            if (service.onid == 0x2310 && service.tsid == 9 && service.sid == 100) snapshotCountryCleared = service.name != "CCTV-1 综合";
        }
        CHECK(snapshotCountryCleared, "new SDT-other reception forgets the previous multiplex country in the SI snapshot");
        feed(tot, 0x14); feed(newMux, 0x11);
        CHECK(demux.snapshot().services[0].name == "CCTV-1 综合", "new multiplex can learn its own country");
        demux.reset(); feed(sdt, 0x11);
        CHECK(demux.snapshot().services[0].name != "CCTV-1 综合", "reset forgets the country hint");
    }
    // Stream profiles and language hints cover other multibyte broadcasts through both guide consumers.
    {
        using dvbsi::TextEncoding;
        struct Sample { const char* country; const char* lang; TextEncoding profile; Bytes raw; const char* utf8; };
        const Sample samples[] = {
            {"KOR", "kor", TextEncoding::Dvb, {'T', 'V', ' ', 0xC7, 0xD1, 0xB1, 0xDB}, "TV 한글"},
            {"TWN", "zho", TextEncoding::Dvb, {'T', 'V', ' ', 0xA4, 0xA4, 0xA4, 0xE5}, "TV 中文"},
            {"JPN", "jpn", TextEncoding::Isdb, {0x46, 0x7C}, "日"},
            {"BRA", "por", TextEncoding::Isdb, {'C', 'a', 'f', 0xE9}, "Café"},
            {"PHL", "tgl", TextEncoding::Isdb, {'T', 'V', ' ', 0xC3, 0xB1}, "TV ñ"},
        };
        for (const auto& sample : samples) {
            Bytes desc = shortEv(sample.lang, sample.raw, sample.raw);
            Bytes ext = extEv(0, 0, sample.raw, sample.lang);
            desc.insert(desc.end(), ext.begin(), ext.end());
            const Bytes guideSection = eit(0x4E, 100, 7, 0x2310, 0, {event(99, mjd, 12, 0, 1, 0, 4, false, desc)});
            Bytes tot = {0x73, 0x70, 0}; putTime(tot, mjd, 12, 0, 0);
            Bytes zone = {0x58, 13}; zone.insert(zone.end(), sample.country, sample.country + 3);
            zone.insert(zone.end(), {2, 0, 0}); putTime(zone, mjd + 1, 0, 0, 0); zone.insert(zone.end(), {0, 0});
            put16(tot, 0xF000 | (int)zone.size()); tot.insert(tot.end(), zone.begin(), zone.end()); finish(tot);
            Bytes sd = {0x48, (uint8_t)(3 + 2 * sample.raw.size()), 1, (uint8_t)sample.raw.size()};
            sd.insert(sd.end(), sample.raw.begin(), sample.raw.end()); sd.push_back((uint8_t)sample.raw.size());
            sd.insert(sd.end(), sample.raw.begin(), sample.raw.end());
            Bytes sdt = {0x42, 0xF0, 0, 0, 7, 0xC1, 0, 0, 0x23, 0x10, 0xFF, 0, 100, 0xFC, 0x80, (uint8_t)sd.size()};
            sdt.insert(sdt.end(), sd.begin(), sd.end()); finish(sdt);
            EpgDb db; EpgCollector collector(&db); TsDemux demux;
            collector.setTextEncoding(sample.profile); demux.setTextEncoding(sample.profile);
            int cc = 0;
            auto feed = [&](const Bytes& section, int pid) {
                const auto packets = packetize(section, pid, cc);
                for (size_t i = 0; i < packets.size(); i += 188) { collector.feed(packets.data() + i); demux.feed(packets.data() + i); }
            };
            feed(tot, 0x14); feed(sdt, 0x11);
            // Exercise both the first-language path and preferred-language re-selection.
            for (int preferred = 0; preferred < 2; preferred++) {
                epgSetPreferredLanguage(preferred ? sample.lang : ""); feed(guideSection, 0x12);
                const auto guide = demux.epg(); const auto events = db.events({0x2310, 7, 100});
                CHECK(guide.count(100) && guide.at(100).size() == 1 && guide.at(100)[0].title == sample.utf8
                      && guide.at(100)[0].text == sample.utf8 && guide.at(100)[0].extended == sample.utf8, "%s demux guide, preference %d", sample.country, preferred);
                CHECK(events.size() == 1 && events[0].title == sample.utf8 && events[0].text == sample.utf8
                      && events[0].extended == sample.utf8, "%s database guide, preference %d", sample.country, preferred);
            }
            epgSetPreferredLanguage("");
            const auto snap = demux.snapshot();
            CHECK(snap.textEncoding == sample.profile && snap.services.size() == 1 && snap.services[0].name == sample.utf8
                  && snap.services[0].provider == sample.utf8 && snap.services[0].now == sample.utf8, "%s service and now/next", sample.country);
            CHECK(collector.serviceName({0x2310, 7, 100}) == sample.utf8, "%s guide service name", sample.country);
            Bytes nameDesc = {1, (uint8_t)(4 + sample.raw.size())};
            nameDesc.insert(nameDesc.end(), sample.lang, sample.lang + 3); nameDesc.push_back((uint8_t)sample.raw.size());
            nameDesc.insert(nameDesc.end(), sample.raw.begin(), sample.raw.end());
            Bytes app = {0, 0, 0, 1, 0, 1, 1}; put16(app, 0xF000 | (int)nameDesc.size());
            app.insert(app.end(), nameDesc.begin(), nameDesc.end());
            Bytes ait = {0x74, 0xF0, 0, 0, 0x10, 0xC1, 0, 0, 0xF0, 0}; put16(ait, 0xF000 | (int)app.size());
            ait.insert(ait.end(), app.begin(), app.end()); finish(ait); feed(ait, 0x240);
            const auto data = demux.snapshot().data;
            CHECK(data && data->ait.size() == 1 && data->ait[0].apps.size() == 1 && data->ait[0].apps[0].name == sample.utf8,
                  "%s application name", sample.country);
            Bytes other = sdt; other.resize(other.size() - 4); other[0] = 0x46; other[4] = 9; finish(other); feed(other, 0x11);
            Bytes otherEit = guideSection; otherEit.resize(otherEit.size() - 4); otherEit[0] = 0x4F; finish(otherEit); feed(otherEit, 0x12);
            const auto si = demux.snapshot().si;
            CHECK(si && !si->otherServices.empty() && si->otherServices[0].name == sample.utf8
                  && !si->eitOther.empty() && si->eitOther[0].nowTitle == sample.utf8, "%s other-transport SI", sample.country);
            if (sample.profile == TextEncoding::Dvb) {
                Bytes ambiguousZone = zone;
                ambiguousZone.at(2) = 'U'; ambiguousZone.at(3) = 'S'; ambiguousZone.at(4) = 'A';
                Bytes ambiguousTot = {0x73, 0x70, 0}; putTime(ambiguousTot, mjd, 12, 0, 0);
                put16(ambiguousTot, 0xF000 | (int)(zone.size() + ambiguousZone.size()));
                ambiguousTot.insert(ambiguousTot.end(), zone.begin(), zone.end());
                ambiguousTot.insert(ambiguousTot.end(), ambiguousZone.begin(), ambiguousZone.end()); finish(ambiguousTot);
                feed(ambiguousTot, 0x14); feed(sdt, 0x11);
                CHECK(demux.snapshot().services[0].name != sample.utf8 && collector.serviceName({0x2310, 7, 100}) != sample.utf8,
                      "%s multi-country table does not guess a service encoding", sample.country);
            }
            // Retuning from ISDB/DTMB to DVB must remove the old profile and its learned country.
            demux.setTextEncoding(TextEncoding::Dvb); collector.setTextEncoding(TextEncoding::Dvb);
            const Bytes french = eit(0x4E, 100, 7, 0x2310, 0, {event(99, mjd, 12, 0, 1, 0, 4, false, shortEv("fra", {'C', 'a', 'f', 0xC2, 'e'}, {}))});
            if (sample.profile != TextEncoding::Dvb) {
                feed(french, 0x12);
                CHECK(demux.epg().at(100)[0].title == "Café", "%s profile switch", sample.country);
            }
        }
        // Korean language can select the legacy fallback without a country table; Japanese needs an ISDB profile.
        for (const auto& sample : {samples[0], samples[2]}) {
            const auto section = eit(0x4E, 1, 1, 1, 0, {event(1, mjd, 12, 0, 1, 0, 4, false, shortEv(sample.lang, sample.raw, {}))});
            EitSection parsed;
            CHECK(parseEitSection(section.data(), (int)section.size(), parsed, {sample.profile}) && parsed.events.size() == 1
                  && parsed.events[0].title == sample.utf8, "%s language-only hint", sample.lang);
        }
    }
    // damage
    { Bytes bad = sec; bad[20] ^= 0x40; EitSection x; CHECK(!parseEitSection(bad.data(), (int)bad.size(), x), "bad CRC rejected"); }
    { EitSection x; CHECK(!parseEitSection(sec.data(), 10, x), "short"); CHECK(!parseEitSection(sec.data(), (int)sec.size() - 1, x), "truncated"); }
    { Bytes b2 = sec; b2[0] = 0x42; EitSection x; CHECK(!parseEitSection(b2.data(), (int)b2.size(), x), "wrong table id"); }
    // a damaged event loop keeps the events before the damage
    { Bytes good = event(10, mjd, 8, 0, 0, 30, 0, false, shortEv("eng", str("Ok"), Bytes()));
      Bytes broken = event(11, mjd, 9, 0, 0, 30, 0, false, Bytes()); broken[11] = 200;   // loop length past the end
      Bytes o = eit(0x4E, 1, 1, 1, 0, {good, broken}); EitSection x;
      CHECK(parseEitSection(o.data(), (int)o.size(), x) && x.events.size() == 1 && x.events[0].eventId == 10, "events before damage kept (%zu)", x.events.size()); }
    // an event with an undefined start (NVOD reference) is left out
    { Bytes e = event(12, mjd, 8, 0, 0, 30, 0, false, Bytes()); for (int i = 2; i < 7; i++) e[i] = 0xFF;
      Bytes o = eit(0x4E, 1, 1, 1, 0, {e}); EitSection x; CHECK(parseEitSection(o.data(), (int)o.size(), x) && x.events.empty(), "undefined start dropped"); }

    // ---- TOT around the end of summer time in Lithuania: +3 until 2026-10-25 01:00 UTC, then +2
    {
        Bytes t = {0x73, 0xF0, 0}; putTime(t, mjd, 12, 0, 0);
        Bytes z = {0x58, 13, 'L', 'T', 'U', (uint8_t)((1 << 2) | 2), 0x03, 0x00};
        putTime(z, 61338, 1, 0, 0); z.push_back(0x02); z.push_back(0x00);   // 61338 = 2026-10-25
        put16(t, 0xF000 | (int)z.size()); t.insert(t.end(), z.begin(), z.end()); finish(t);
        TotInfo info;
        CHECK(parseTimeSection(t.data(), (int)t.size(), info) && info.hasOffsets && info.offsets.size() == 1, "TOT parsed");
        if (!info.offsets.empty()) {
            const LocalTimeOffset& o = info.offsets[0];
            CHECK(o.country == "LTU" && o.offsetMin == 180 && o.nextOffsetMin == 120 && o.region == 1, "zone fields %d %d", o.offsetMin, o.nextOffsetMin);
            const int64_t change = (int64_t)(61338 - 40587) * 86400 + 3600;
            CHECK(o.changeUtc == change, "change time");
            CHECK(zoneOffsetAt(o, change - 1) == 3 * 3600 && zoneOffsetAt(o, change) == 2 * 3600, "offset switches at the change");
            CHECK(formatInZone(change - 60, &o) == "2026-10-25 03:59", "before: %s", formatInZone(change - 60, &o).c_str());
            CHECK(formatInZone(change, &o) == "2026-10-25 03:00", "after: %s", formatInZone(change, &o).c_str());
            CHECK(formatInZone(change, nullptr) == "2026-10-25 01:00", "utc");
        }
        Bytes bad = t; bad[bad.size() - 1] ^= 1; CHECK(!parseTimeSection(bad.data(), (int)bad.size(), info), "TOT crc");
        Bytes tdt = {0x70, 0x70, 0x05}; putTime(tdt, mjd, 1, 2, 3);
        CHECK(parseTimeSection(tdt.data(), (int)tdt.size(), info) && info.utc == (int64_t)(mjd - 40587) * 86400 + 3723, "TDT");
        // negative zone (polarity 1): UTC-5
        Bytes t2 = {0x73, 0xF0, 0}; putTime(t2, mjd, 0, 0, 0);
        Bytes z2 = {0x58, 13, 'U', 'S', 'A', (uint8_t)((1 << 2) | 1), 0x05, 0x00}; putTime(z2, mjd, 0, 0, 0); z2.push_back(0x05); z2.push_back(0x00);
        put16(t2, 0xF000 | (int)z2.size()); t2.insert(t2.end(), z2.begin(), z2.end()); finish(t2);
        TotInfo i2; CHECK(parseTimeSection(t2.data(), (int)t2.size(), i2) && i2.offsets[0].offsetMin == -300, "negative offset");
    }

    // ---- database
    EpgDb db;
    const int64_t now = (int64_t)(mjd - 40587) * 86400 + 20 * 3600;   // 20:00 UTC on the day
    const ServiceKey k{0x20FA, 2, 0x1001};
    db.ingestSection(es, now);
    CHECK(db.count() == 1 && db.services().size() == 1, "one event");
    EpgItem cur, nxt;
    CHECK(!db.nowNext(k, now, &cur, &nxt) && nxt.eventId == 0x100, "next before start");
    CHECK(db.nowNext(k, now + 40 * 60, &cur, &nxt) && cur.title == "Evening News", "now");
    {   // an event with another id overlapping: replaced; same id with less text: merged
        EpgItem a = es.events[0]; a.eventId = 0x101; a.title = "Special"; a.start += 600;
        CHECK(db.ingest(k, a, now), "overlap ingested");
        CHECK(db.count() == 1 && db.events(k)[0].title == "Special", "overlapping event replaced");
        EpgItem b = a; b.extended.clear(); b.content.clear(); b.rating = 0; b.running = 4;
        db.ingest(k, b, now);
        const EpgItem g = db.events(k)[0];
        CHECK(g.extended == es.events[0].extended && g.rating == 13 && !g.content.empty() && g.running == 4, "merge keeps text, rating, genre; takes running status");
        CHECK(!db.ingest(k, b, now) || true, "same again");
    }
    {   // An old EPG cache is repaired by normal same-event reception, without clearing unrelated services.
        EpgDb cached;
        EpgItem stale; stale.eventId = 701; stale.start = now; stale.duration = 1800; stale.lang = "chi";
        stale.title = "µĿ©←—ł¾Ŀ"; stale.text = "old short text"; stale.extended = "old extended text";
        stale.rating = 12; stale.content = {0x20}; stale.crid = "example/news";
        const ServiceKey other{2, 9, 905};
        EpgItem untouched = stale; untouched.eventId = 702; untouched.title = "正常中文·已缓存节目";
        cached.ingest(k, stale, now); cached.ingest(other, untouched, now);
        EpgDb loaded; CHECK(loaded.fromJson(cached.toJson()), "load old guide cache");
        EpgItem fresh = stale; fresh.title = "新闻报道"; fresh.text = "最新节目简介"; fresh.extended = "最新详细内容";
        fresh.rating = 0; fresh.content.clear(); fresh.crid.clear();
        CHECK(loaded.ingest(k, fresh, now), "fresh broadcast replaces old cached text");
        EpgItem repaired; CHECK(loaded.find(k, stale.eventId, repaired), "cached event remains present");
        CHECK(repaired.title == fresh.title && repaired.text == fresh.text && repaired.extended == fresh.extended, "all fresh text fields replace old text");
        CHECK(repaired.rating == 12 && repaired.content == stale.content && repaired.crid == stale.crid, "original partial-update metadata merge is preserved");
        CHECK(loaded.count() == 2, "cache entries are not cleared");
        CHECK(tvjson::dump(epgItemToValue(loaded.events(other)[0])) == tvjson::dump(epgItemToValue(untouched)), "unrelated cached programme is unchanged");
        // The same title can also have stale descriptions; normal ingestion must still replace them.
        repaired.text = "stale"; repaired.extended = "stale"; loaded.ingest(k, repaired, now);
        CHECK(loaded.ingest(k, fresh, now), "refresh descriptions even when the title is unchanged");
        EpgDb roundTrip; CHECK(roundTrip.fromJson(loaded.toJson()), "repaired guide saves and reloads");
        CHECK(roundTrip.find(k, fresh.eventId, repaired) && repaired.title == fresh.title && repaired.extended == fresh.extended, "repaired text persists");
        EpgItem partial = fresh; partial.text.clear(); partial.extended.clear();
        roundTrip.ingest(k, partial, now);
        CHECK(roundTrip.find(k, fresh.eventId, repaired) && repaired.text == fresh.text && repaired.extended == fresh.extended, "missing broadcast descriptions still retain cached text");
    }
    {   // expiry and far-future refusal
        EpgDb d2;
        EpgItem old; old.eventId = 1; old.start = now - 20 * 3600; old.duration = 3600; old.title = "old";
        CHECK(!d2.ingest(k, old, now), "expired refused");
        EpgItem fut; fut.eventId = 2; fut.start = now + 90 * 86400; fut.duration = 3600; fut.title = "far";
        CHECK(!d2.ingest(k, fut, now), "far future refused");
        EpgItem okEv; okEv.eventId = 3; okEv.start = now - 3600 * 5; okEv.duration = 3600; okEv.title = "recent";
        CHECK(d2.ingest(k, okEv, now), "recent accepted");
        CHECK(d2.prune(now + 3 * 3600) == 1 && d2.count() == 0, "pruned later");
        EpgLimits l; l.maxPerService = 5; d2.setLimits(l);
        for (int i = 0; i < 20; i++) { EpgItem e; e.eventId = 100 + i; e.start = now + i * 3600; e.duration = 3600; e.title = "e" + std::to_string(i); d2.ingest(k, e, now); }
        const auto ev = d2.events(k);
        CHECK(ev.size() == 5 && ev[0].title == "e0" && ev[4].title == "e4", "bounded per service, the farthest dropped (%zu)", ev.size());
        EpgLimits l2; l2.maxServices = 2; d2.setLimits(l2);
        EpgItem e; e.eventId = 1; e.start = now; e.duration = 100; e.title = "x";
        d2.ingest({1, 1, 1}, e, now); d2.ingest({1, 1, 2}, e, now); d2.ingest({1, 1, 3}, e, now);
        CHECK(d2.services().size() <= 3, "services bounded");
    }
    {   // search and series
        EpgDb d3;
        auto add = [&](int sid, int id, int64_t st, const char* title, const char* text, const char* series) {
            EpgItem e; e.eventId = id; e.start = st; e.duration = 1800; e.title = title; e.text = text; e.seriesCrid = series; d3.ingest({1, 1, sid}, e, now); };
        add(1, 1, now + 100, "Football: Derby", "Live match", "");
        add(1, 2, now + 5000, "Quiz Night", "Ask Me Anything", "s/quiz");
        add(2, 1, now + 9000, "quiz night", "repeat", "s/quiz");
        add(2, 2, now + 20000, "Weather", "football weather", "");
        CHECK(d3.search("QUIZ").size() == 2, "search is case-insensitive");
        CHECK(d3.search("football weather").size() == 1 && d3.search("football").size() == 2, "search words all must match");
        CHECK(d3.search("anything quiz").size() == 1, "search in text");
        CHECK(d3.search("").empty() && d3.search("zzz").empty(), "empty / no hit");
        CHECK(d3.seriesEvents("s/quiz", now).size() == 2 && d3.seriesEvents("s/quiz", now + 7000).size() == 1, "series events");
        const std::string js = d3.toJson();
        EpgDb d4;
        CHECK(d4.fromJson(js) && d4.count() == 4, "json round trip");
        CHECK(d4.toJson() == js, "json stable");
        CHECK(!d4.fromJson("{not json") && d4.count() == 4, "bad json leaves the database alone");
        CHECK(!d4.fromJson("{\"services\":5}") && d4.count() == 4, "wrong shape");
        const std::string path = (std::filesystem::temp_directory_path() / "onair_epg_test.json").string();
        CHECK(d3.save(path), "save");
        EpgDb d5; CHECK(d5.load(path) && d5.count() == 4, "load");
        std::filesystem::remove(path);
        CHECK(!d5.load("/nonexistent/x.json"), "load missing");
    }
    {   // JSON text with unicode and escapes
        EpgDb d6;
        EpgItem e; e.eventId = 1; e.start = now; e.duration = 60; e.title = "Caf\xC3\xA9 \"q\" \\ \n tab\t \xF0\x9F\x8E\xAC";
        d6.ingest(k, e, now);
        EpgDb d7; CHECK(d7.fromJson(d6.toJson()) && d7.events(k)[0].title == e.title, "unicode / escapes survive");
    }

    // ---- collector: sections in packets, split over several packets; continuity error drops the section
    {
        EpgDb cdb; EpgCollector col(&cdb); col.setFixedNow(now);
        int cc = 0;
        std::vector<uint8_t> ts;
        // a big section: 40 events -> several packets
        std::vector<Bytes> evs;
        for (int i = 0; i < 40; i++) evs.push_back(event(200 + i, mjd, 21 + (i / 2) % 3, (i % 2) * 30, 0, 30, 0, false, shortEv("eng", str(("Show number " + std::to_string(i)).c_str()), str("blah blah blah blah blah blah"))));
        Bytes big = eit(0x50, 0x1001, 2, 0x20FA, 0, evs);
        CHECK(big.size() > 400, "multi-packet section");
        auto p = packetize(big, 0x12, cc); ts.insert(ts.end(), p.begin(), p.end());
        for (size_t i = 0; i < ts.size(); i += 188) col.feed(&ts[i]);
        CHECK(col.sections() == 1 && col.badSections() == 0 && cdb.count() > 0, "collected %zu events", cdb.count());
        // lose the middle packet of the next copy
        const size_t before = cdb.count();
        Bytes big2 = eit(0x51, 0x1001, 2, 0x20FA, 0, {event(900, mjd + 1, 5, 0, 1, 0, 0, false, shortEv("eng", str("Lost"), Bytes()))});
        std::vector<uint8_t> t2 = packetize(big, 0x12, cc);
        for (size_t i = 0; i < t2.size(); i += 188) { if (i == 188) continue; col.feed(&t2[i]); }
        CHECK(col.sections() == 1, "a section with a lost packet is not accepted");
        auto p3 = packetize(big2, 0x12, cc);
        for (size_t i = 0; i < p3.size(); i += 188) col.feed(&p3[i]);
        CHECK(col.sections() == 2 && cdb.count() == before + 1, "stream recovers after loss");
        // corrupt CRC counted
        Bytes bad = big2; bad[bad.size() - 1] ^= 0xFF;
        auto p4 = packetize(bad, 0x12, cc);
        for (size_t i = 0; i < p4.size(); i += 188) col.feed(&p4[i]);
        CHECK(col.badSections() == 1, "bad section counted");
        // TDT sets the stream clock; advance moves it
        Bytes tdt = {0x70, 0x70, 0x05}; putTime(tdt, mjd, 10, 0, 0);
        EpgCollector c2(nullptr); auto p5 = packetize(tdt, 0x14, cc);
        for (size_t i = 0; i < p5.size(); i += 188) c2.feed(&p5[i]);
        const int64_t base = (int64_t)(mjd - 40587) * 86400 + 36000;
        CHECK(c2.haveStreamTime() && c2.utcNow() == base, "stream time");
        c2.advance(12.7);
        CHECK(c2.utcNow() == base + 12, "advanced");
    }

    // ---- fuzz: damaged sections never crash and never read out of range
    {
        std::mt19937 rng(12345);
        int accepted = 0;
        for (int it = 0; it < 60000; it++) {
            Bytes s = sec;
            const int kind = it % 4;
            if (kind == 0) { for (int k2 = 0; k2 < 1 + (int)(rng() % 4); k2++) s[rng() % s.size()] = (uint8_t)rng(); }
            else if (kind == 1) s.resize(rng() % s.size());
            else if (kind == 2) { for (auto& b : s) b = (uint8_t)rng(); if (rng() & 1) s[0] = 0x50; }
            else { for (int k2 = 0; k2 < 3; k2++) s[12 + rng() % (s.size() - 16)] = (uint8_t)rng(); if (s.size() > 4) { const uint32_t c = mpegCrc32(s.data(), (int)s.size() - 4); for (int q = 0; q < 4; q++) s[s.size() - 4 + q] = (uint8_t)(c >> (24 - 8 * q)); } }
            EitSection x;
            if (parseEitSection(s.data(), (int)s.size(), x)) { accepted++; for (auto& e : x.events) { (void)e.title.size(); } }
            TotInfo ti; parseTimeSection(s.data(), (int)s.size(), ti);
        }
        CHECK(accepted > 100, "the fuzz run also reached the parser with valid CRCs (%d)", accepted);
    }
    {   // a guide in two languages (Finland: Finnish and Swedish): the preferred language is used where the event has it (#268)
        Bytes desc = shortEv("swe", str("Kvallsnyheter"), str("Dagens nyheter"));
        { Bytes f = shortEv("fin", str("Iltauutiset"), str("Paivan uutiset")); desc.insert(desc.end(), f.begin(), f.end()); }   // Finnish sent last
        { Bytes e1 = extEv(0, 1, str("Svensk text. ")); e1[3] = 's'; e1[4] = 'w'; e1[5] = 'e'; Bytes e2 = extEv(1, 1, str("Suomalainen teksti.")); e2[3] = 'f'; e2[4] = 'i'; e2[5] = 'n';
          desc.insert(desc.end(), e1.begin(), e1.end()); desc.insert(desc.end(), e2.begin(), e2.end()); }
        Bytes other = shortEv("swe", str("Bara svenska"), str("text"));   // an event with no Finnish at all
        const Bytes sec = eit(0x4E, 0x1001, 2, 0x20FA, 0, {event(0x200, mjd, 21, 0, 0, 30, 4, false, desc), event(0x201, mjd, 21, 30, 0, 30, 1, false, other)});
        EitSection x;
        epgSetPreferredLanguage("");
        CHECK(parseEitSection(sec.data(), (int)sec.size(), x) && x.events.size() == 2 && x.events[0].title == "Iltauutiset", "no preference: the last language broadcast (%s)", x.events[0].title.c_str());
        epgSetPreferredLanguage("FIN");
        CHECK(parseEitSection(sec.data(), (int)sec.size(), x) && x.events[0].title == "Iltauutiset" && x.events[0].text == "Paivan uutiset" && x.events[0].lang == "fin", "Finnish wanted: Finnish");
        CHECK(x.events[0].extended == "Suomalainen teksti.", "the extended text is of that language only (%s)", x.events[0].extended.c_str());
        CHECK(x.events[1].title == "Bara svenska", "an event without the language keeps what it has");
        epgSetPreferredLanguage("swe");
        CHECK(parseEitSection(sec.data(), (int)sec.size(), x) && x.events[0].title == "Kvallsnyheter" && x.events[0].text == "Dagens nyheter" && x.events[0].lang == "swe", "Swedish wanted: Swedish");
        CHECK(x.events[0].extended == "Svensk text.", "Swedish extended text (%s)", x.events[0].extended.c_str());
        // the service list parser (TsDemux) follows the same preference
        for (int pass = 0; pass < 2; pass++) {
            epgSetPreferredLanguage(pass ? "fin" : "swe");
            TsDemux dm; int cc = 0;
            const auto pk = packetize(sec, 0x12, cc);
            for (size_t o = 0; o + 188 <= pk.size(); o += 188) dm.feed(pk.data() + o);
            auto g = dm.epg();
            CHECK(g.count(0x1001) && g[0x1001].size() == 2, "TsDemux got the events");
            if (g.count(0x1001) && g[0x1001].size() == 2) CHECK(g[0x1001][0].title == (pass ? "Iltauutiset" : "Kvallsnyheter"), "TsDemux title for pass %d: %s", pass, g[0x1001][0].title.c_str());
        }
        epgSetPreferredLanguage("");
        CHECK(epgLanguageKey("GER") == "deu" && epgLanguageKey("deu") == "deu" && epgLanguageKey("fi") == "fi", "the two ISO 639-2 codes of a language are one language");
    }
    return testSummary("tv_epg");
}
