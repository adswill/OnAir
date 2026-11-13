// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B feeds from other receivers, offline registrations and countries, range statistics.
#include "dect2/adsb/adsb_feedin.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/adsb/adsb_reg.h"
#include "dect2/adsb/adsb_track.h"
#include "../../core/src/net/tcp_client.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void testCountries() {
    const struct { uint32_t a; const char* iso; } k[] = {
        {0xA00001, "US"}, {0xA835AF, "US"}, {0xC01234, "CA"}, {0x0D1234, "MX"}, {0xE48123, "BR"}, {0xE01234, "AR"}, {0x400123, "GB"}, {0x3C6444, "DE"},
        {0x380123, "FR"}, {0x4840D6, "NL"}, {0x40621D, "GB"}, {0x300123, "IT"}, {0x340123, "ES"}, {0x4B8123, "TR"}, {0x100001, "RU"}, {0x503D00, "LT"},
        {0x780000, "CN"}, {0x840123, "JP"}, {0x800123, "IN"}, {0x7C1B20, "AU"}, {0xC80001, "NZ"}, {0x896123, "AE"}, {0x738ABC, "IL"}, {0x710001, "SA"},
        {0x718001, "KR"}, {0x008123, "ZA"},
    };
    for (const auto& e : k) CHECK(strcmp(adsbCountry(e.a).iso, e.iso) == 0, "%06X -> %s, wanted %s", (unsigned)e.a, adsbCountry(e.a).iso, e.iso);
    CHECK(strcmp(adsbCountry(0x000001).iso, "") == 0 && strcmp(adsbCountry(0x000001).name, "unknown") == 0, "unallocated");
    CHECK(strcmp(adsbCountry(0xF00000).iso, "") == 0, "unallocated high");
    CHECK(strcmp(adsbCountry(0x4CA000).iso, "IE") == 0 && strcmp(adsbCountry(0x4CAFFF).iso, "IE") == 0 && strcmp(adsbCountry(0x4CB000).iso, "") == 0, "block edges");
}

static void testNNumbers() {
    std::string r;
    uint32_t a = 0;
    CHECK(adsbNNumberFromIcao(0xA00001, r) && r == "N1", "A00001 -> %s", r.c_str());
    CHECK(adsbNNumberFromIcao(0xADF7C7, r) && r == "N99999", "ADF7C7 -> %s", r.c_str());
    CHECK(!adsbNNumberFromIcao(0xADF7C8, r) && !adsbNNumberFromIcao(0xA00000, r) && !adsbNNumberFromIcao(0xAE0000, r), "outside the range");
    CHECK(adsbNNumberFromIcao(0xA835AF, r) && r == "N628TS", "A835AF -> %s", r.c_str());   // a well known aircraft
    CHECK(adsbIcaoFromNNumber("N628TS", a) && a == 0xA835AF, "N628TS -> %06X", (unsigned)a);
    CHECK(adsbIcaoFromNNumber("N1", a) && a == 0xA00001 && adsbIcaoFromNNumber("n99999", a) && a == 0xADF7C7, "ends");
    // N1000: bare N1, 600 suffixes, then N10, 600 suffixes, then N100, 600 suffixes, then N1000
    CHECK(adsbIcaoFromNNumber("N1000", a) && a == 0xA00001 + 3 * 601, "N1000 -> %06X", (unsigned)a);
    CHECK(adsbNNumberFromIcao(a, r) && r == "N1000", "N1000 back: %s", r.c_str());
    CHECK(!adsbIcaoFromNNumber("N0", a) && !adsbIcaoFromNNumber("N1I", a) && !adsbIcaoFromNNumber("N1O", a) && !adsbIcaoFromNNumber("N123456", a) &&
          !adsbIcaoFromNNumber("N12345A", a) && !adsbIcaoFromNNumber("N1ABC", a) && !adsbIcaoFromNNumber("C1", a) && !adsbIcaoFromNNumber("N", a), "bad registrations");
    // every address of the block: decode, encode, same address; and every registration is different
    std::set<std::string> seen;
    int bad = 0;
    for (uint32_t i = 0xA00001; i <= 0xADF7C7; i++) {
        uint32_t back = 0;
        if (!adsbNNumberFromIcao(i, r) || !adsbIcaoFromNNumber(r, back) || back != i) { if (++bad <= 3) printf("round trip %06X -> %s -> %06X\n", (unsigned)i, r.c_str(), (unsigned)back); continue; }
        seen.insert(r);
    }
    CHECK(bad == 0, "%d round trips failed", bad);
    CHECK(seen.size() == 0xADF7C7 - 0xA00001 + 1, "%zu different registrations", seen.size());
    // Canada and Australia
    CHECK(adsbCanadaFromIcao(0xC00001, r) && r == "C-FAAA" && adsbIcaoFromCanada("C-GZZZ", a) && a == 0xC00001 + 2 * 17576 - 1, "Canada ends");
    bad = 0;
    for (uint32_t i = 0xC00001; i <= 0xC00000 + 2 * 17576; i++) { uint32_t b = 0; if (!adsbCanadaFromIcao(i, r) || !adsbIcaoFromCanada(r, b) || b != i) bad++; }
    for (uint32_t i = 0x7C0000; i < 0x7C0000 + 17576; i++) { uint32_t b = 0; if (!adsbAustraliaFromIcao(i, r) || !adsbIcaoFromAustralia(r, b) || b != i) bad++; }
    CHECK(bad == 0, "Canada / Australia round trips: %d", bad);
    CHECK(adsbAustraliaFromIcao(0x7C0000, r) && r == "VH-AAA" && !adsbAustraliaFromIcao(0x7C5000, r), "Australia");
    CHECK(adsbAlgorithmicRegistration(0xA835AF) == "N628TS" && adsbAlgorithmicRegistration(0x4840D6).empty(), "algorithmic");
    CHECK(adsbCategoryText("A5") == "Heavy (> 300 000 lb)" && adsbCategoryText("Z9").empty(), "categories");
}

static void testCsv() {
    AdsbRegDb db;
    size_t skipped = 99;
    const std::string text =
        "icao,registration,type,operator\r\n"
        "4840D6,PH-BQA,B744,KLM\r\n"
        "A835AF,N628TS,GLF6,\"Smith, Jones & Co\"\r\n"
        "\r\n"
        "# a comment\n"
        "ZZZZZZ,XX-XXX,B738,Nobody\n"          // not hex
        "123\n"                                  // one field
        "1234567,TOO-LONG,A320,Nobody\n"         // seven digits
        "485020,\"PH-OPEN,A320,Unclosed\n"       // unterminated quote
        ",,,\n"                                  // nothing
        "4B1814,HB-JHA,A320,Swiss\n";
    const size_t n = db.loadText(text, &skipped);
    CHECK(n == 3 && skipped == 5, "read %zu skipped %zu", n, skipped);
    const AdsbRegEntry* e = db.find(0x4840D6);
    CHECK(e && e->registration == "PH-BQA" && e->type == "B744" && e->operatorName == "KLM", "KLM entry");
    e = db.find(0xA835AF);
    CHECK(e && e->operatorName == "Smith, Jones & Co", "quoted comma: %s", e ? e->operatorName.c_str() : "");
    CHECK(db.find(0x485020) == nullptr && db.find(0x123) == nullptr, "malformed lines left out");
    CHECK(db.registration(0x4B1814) == "HB-JHA" && db.registration(0xA00001) == "N1" && db.registration(0x400000).empty(), "registration()");
    AdsbRegDb db2;
    CHECK(db2.loadText("4840D6;PH-BQA;B744;0;Boeing 747-406;1989;;;KLM\n4840D7;PH-BQB;B744\n", &skipped) == 2 && skipped == 0, "semicolon format");
    CHECK(db2.find(0x4840D6) && db2.find(0x4840D6)->operatorName == "KLM", "semicolon operator");
    CHECK(!db2.loadFile("/nonexistent/aircraft.csv"), "missing file");
}

static std::string bytesOf(const char* hex) {
    std::string s;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) { unsigned v; sscanf(hex + i, "%2x", &v); s += (char)v; }
    return s;
}
static void testBeast() {
    // 1A 33, time stamp 11 22 33 44 1A 55 (the 1A is doubled), signal 5A, the identification frame of KLM1023
    const std::string a = bytesOf("1A33" "112233441A1A55" "5A" "8D4840D6202CC371C32CE0576098");
    // a short frame (DF11 all-call, 7 bytes), then a Mode A/C frame that is dropped, then a long one again
    const std::string b = bytesOf("1A32" "000000000001" "20" "5D4840D6202CC3");
    const std::string c = bytesOf("1A31" "000000000002" "20" "1234");
    const std::string d = bytesOf("1A33" "000000000003" "30" "8D40621D58C382D690C8AC2863A7");
    std::string all = "garbage" + a + b + c + std::string("\x1A\x1A\x07", 3) + d;   // junk before and between frames
    BeastParser p;
    std::vector<BeastFrame> out;
    for (size_t i = 0; i < all.size(); i += 5) p.push((const uint8_t*)all.data() + i, std::min<size_t>(5, all.size() - i), out);   // in small pieces
    CHECK(out.size() == 3, "%zu frames", out.size());
    if (out.size() == 3) {
        CHECK(out[0].bits == 112 && out[0].signal == 0x5A && out[0].ts12 == 0x112233441A55ULL, "first frame header: ts %llX", (unsigned long long)out[0].ts12);
        CHECK(adsb::toHex(out[0].bytes, 112) == "8D4840D6202CC371C32CE0576098", "first frame bytes: %s", adsb::toHex(out[0].bytes, 112).c_str());
        CHECK(out[1].bits == 56 && out[1].signal == 0x20 && out[1].bytes[0] == 0x5D, "short frame");
        CHECK(out[2].bits == 112 && adsb::toHex(out[2].bytes, 112) == "8D40621D58C382D690C8AC2863A7", "last frame after junk");
    }
    // a truncated stream gives nothing, and the next frame is still found
    BeastParser q;
    std::vector<BeastFrame> o2;
    const std::string cut = a.substr(0, 12) + d;
    q.push((const uint8_t*)cut.data(), cut.size(), o2);
    CHECK(o2.size() == 1 && o2[0].ts12 == 3, "recovery after a cut frame: %zu", o2.size());
}

static void testSbs() {
    AdsbExtUpdate u;
    CHECK(sbsParseLine("MSG,3,1,1,4840D6,1,2026/10/13,12:00:00.000,2026/10/13,12:00:00.000,,38000,,,52.26578,3.93891,,,0,0,0,0", u) && u.icao == 0x4840D6 &&
          u.hasAlt && u.altFt == 38000 && u.hasPos && std::fabs(u.lat - 52.26578) < 1e-9 && std::fabs(u.lon - 3.93891) < 1e-9 && !u.hasSpeed && u.hasGround && !u.ground, "position line");
    CHECK(sbsParseLine("MSG,1,1,1,4840D6,1,2026/10/13,12:00:00.000,2026/10/13,12:00:00.000,KLM1023 ,,,,,,,,,,,", u) && u.callsign == "KLM1023" && !u.hasAlt && !u.hasPos, "identification line");
    CHECK(sbsParseLine("MSG,4,1,1,4840D6,1,x,x,x,x,,,450.5,182.9,,,-832,,,,,", u) && u.hasSpeed && u.speedKt == 450.5 && u.hasHeading && u.hasVrate && u.vrateFpm == -832, "velocity line");
    CHECK(sbsParseLine("MSG,6,1,1,4840D6,1,x,x,x,x,,,,,,,,7700,-1,-1,0,0", u) && u.hasSquawk && u.squawk == 7700, "squawk line");
    CHECK(!sbsParseLine("STA,,1,1,4840D6,1,x,x,x,x,RM", u) && !sbsParseLine("MSG,3,1,1,XYZ,1", u) && !sbsParseLine("MSG,3,1,1,000000,1", u) && !sbsParseLine("MSG,3", u) && !sbsParseLine("", u), "lines that are not aircraft");
    CHECK(sbsParseLine("MSG,3,1,1,4840D6,1,x,x,x,x,,5000,,,95.0,3.0,,,,,,", u) && !u.hasPos, "latitude out of range is not a position");
    CHECK(sbsParseLine("MSG,3,1,1,4840D6,1,x,x,x,x,,abc,,,,,,,,,,", u) && !u.hasAlt, "text in a number field");
}

static void testLan() {
    for (const char* h : {"localhost", "127.0.0.1", "192.168.1.20", "10.0.0.5", "172.16.3.4", "172.31.255.1", "169.254.1.1", "raspberrypi.local", "pi.lan", "::1", "fe80::1", "fd12::5", "[::1]"})
        CHECK(adsbHostIsLan(h), "%s should be local", h);
    for (const char* h : {"8.8.8.8", "172.32.0.1", "172.15.0.1", "example.com", "1.2.3.4", "192.169.1.1", "", "300.1.1.1"})
        CHECK(!adsbHostIsLan(h), "%s should not be local", h);
    std::string host, err; int port = 0;
    CHECK(adsbSplitHostPort(" 192.168.1.20:30005 ", host, port, err) && host == "192.168.1.20" && port == 30005, "split");
    CHECK(adsbSplitHostPort("[::1]:30003", host, port, err) && host == "::1" && port == 30003, "split IPv6");
    CHECK(!adsbSplitHostPort("192.168.1.20", host, port, err) && !adsbSplitHostPort("pi:0", host, port, err) && !adsbSplitHostPort("pi:99999", host, port, err) && !adsbSplitHostPort(":30005", host, port, err) && !adsbSplitHostPort("pi:abc", host, port, err), "bad host:port");
}

static AdsbFrame frame(const char* hex, double t) {
    AdsbFrame f;
    f.bits = 112;
    for (int i = 0; i < 14; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); f.bytes[i] = (uint8_t)v; }
    f.df = adsb::getBits(f.bytes, 1, 5);
    f.icao = adsb::getBits(f.bytes, 9, 24);
    f.levelDbfs = -30; f.snrDb = 20; f.timeSec = t;
    adsb::decodeMsg(f.bytes, 112, f.msg);
    f.msg.icao = f.icao;
    return f;
}
static AdsbAircraft get(const AdsbTracker& tr, uint32_t icao, double now) {
    AdsbTelemetry t;
    tr.snapshot(t, now);
    for (auto& a : t.aircraft) if (a.icao == icao) return a;
    AdsbAircraft none; none.icao = 0xFFFFFFFF; return none;
}

static void testMerge() {
    AdsbTracker tr;
    tr.setReference(52.0, 4.0);
    // this receiver hears the identification; another receiver sends the even and odd position frames and an SBS update for the same address
    tr.apply(frame("8D4840D6202CC371C32CE0576098", 1.0), 0);
    CHECK(get(tr, 0x4840D6, 1.0).srcId == 0 && tr.aircraftCount() == 1, "local");
    tr.apply(frame("8D40621D58C382D690C8AC2863A7", 2.0), 1);
    tr.apply(frame("8D40621D58C386435CC412692AD6", 3.0), 1);
    AdsbAircraft a = get(tr, 0x40621D, 3.0);
    CHECK(a.hasPos && a.srcId == 1 && a.posKind == 2 && std::fabs(a.lat - 52.26578) < 1e-4, "position from source 1: %d %.5f", a.srcId, a.lat);
    CHECK(tr.good() == 1 && tr.aircraftFrom(1) == 1 && tr.aircraftFrom(0) == 1, "other receivers' frames are not counted as local ones (good %llu)", (unsigned long long)tr.good());
    // the same aircraft from source 2 with a newer position: one record, the newer position wins and the source follows
    AdsbExtUpdate u;
    u.icao = 0x40621D; u.hasPos = true; u.lat = 52.30; u.lon = 4.00; u.hasAlt = true; u.altFt = 37000; u.callsign = "TEST1";
    tr.applyExternal(u, 2, 4.0);
    a = get(tr, 0x40621D, 4.0);
    CHECK(tr.aircraftCount() == 2, "no duplicate: %zu", tr.aircraftCount());
    CHECK(a.srcId == 2 && std::fabs(a.lat - 52.30) < 1e-9 && a.altFt == 37000 && a.callsign == "TEST1", "newest wins: src %d lat %.4f", a.srcId, a.lat);
    // then this receiver hears a position of its own, later still: it wins back
    u.lat = 52.31; u.lon = 4.01;
    tr.applyExternal(u, 0, 5.0);
    CHECK(get(tr, 0x40621D, 5.0).srcId == 0 && std::fabs(get(tr, 0x40621D, 5.0).lat - 52.31) < 1e-9, "local update wins back");
    CHECK(a.messages >= 3, "messages are counted per aircraft: %u", a.messages);
    // an update without a position leaves the position alone
    AdsbExtUpdate v; v.icao = 0x40621D; v.hasSpeed = true; v.speedKt = 400;
    tr.applyExternal(v, 1, 6.0);
    a = get(tr, 0x40621D, 6.0);
    CHECK(std::fabs(a.lat - 52.31) < 1e-9 && a.hasSpeed && a.srcId == 1, "velocity only");
    // (0,0) is not a position
    AdsbExtUpdate z; z.icao = 0xABCDEF; z.hasPos = true;
    tr.applyExternal(z, 1, 7.0);
    CHECK(!get(tr, 0xABCDEF, 7.0).hasPos, "null island");
}

static void testStats() {
    AdsbTracker tr;
    tr.setReference(52.0, 4.0);
    AdsbExtUpdate u;
    u.icao = 0x111111; u.hasPos = true; u.lat = 53.0; u.lon = 4.0; u.hasAlt = true; u.altFt = 41000;   // 60 nm north
    tr.applyExternal(u, 1, 1.0);
    u.icao = 0x222222; u.lat = 52.0; u.lon = 5.0; u.altFt = 12000;                                      // about 36.9 nm east
    tr.applyExternal(u, 1, 2.0);
    const AdsbRangeStats& s = tr.stats();
    CHECK(s.positions == 2 && s.maxAltFt == 41000 && s.hasMaxAlt, "positions %llu alt %d", (unsigned long long)s.positions, s.maxAltFt);
    CHECK(std::fabs(s.sectorMaxNm[0] - 60.0) < 1.0, "north sector %.1f", s.sectorMaxNm[0]);
    CHECK(std::fabs(s.sectorMaxNm[8] - 36.9) < 1.0, "east sector %.1f", s.sectorMaxNm[8]);
    CHECK(s.rangeHist[6] == 1 && s.rangeHist[3] == 1, "histogram %u %u", s.rangeHist[6], s.rangeHist[3]);
    CHECK(std::fabs(s.maxRangeNm - 60.0) < 1.0, "max range %.1f", s.maxRangeNm);
    tr.tick(35.0);
    CHECK(tr.stats().rateSeries.size() == 3 && tr.stats().countSeries.size() == 3 && tr.stats().countSeries[0] == 2, "series %zu", tr.stats().rateSeries.size());
    // saved with the session
    AdsbTracker tr2;
    CHECK(tr2.loadResults(tr.saveResults(), 40.0), "load");
    CHECK(tr2.stats().positions == 2 && std::fabs(tr2.stats().sectorMaxNm[0] - 60.0) < 1.0 && tr2.stats().maxAltFt == 41000 && tr2.stats().rateSeries.size() == 3, "stats survive a save");
    tr2.resetStats();
    CHECK(tr2.stats().positions == 0 && tr2.stats().sectorMaxNm[0] == 0 && !tr2.stats().hasMaxAlt && tr2.stats().rateSeries.empty(), "reset");
    // no reference: no ranges, but the altitude is kept
    AdsbTracker tr3;
    tr3.applyExternal(u, 1, 1.0);
    CHECK(tr3.stats().positions == 0 && tr3.stats().maxAltFt == 12000, "no reference");
}

// a loopback server sends an SBS line to the first client and a Beast frame to the second; the feed in reads both, the one outside the LAN is refused
static void testFeedIn() {
    netInit();
    auto listener = [](int& port) {
        sock_t l = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(l, (sockaddr*)&a, sizeof a); listen(l, 2);
        socklen_t n = sizeof a; getsockname(l, (sockaddr*)&a, &n); port = ntohs(a.sin_port);
        return l;
    };
    int p1 = 0, p2 = 0;
    sock_t l1 = listener(p1), l2 = listener(p2);
    const std::string sbs = "MSG,3,1,1,4840D6,1,x,x,x,x,,38000,,,52.1,4.1,,,0,0,0,0\r\nMSG,1,1,1,4840D6,1,x,x,x,x,KLM1023,,,,,,,,,,,\r\n";
    std::string beast = bytesOf("1A33" "000000000001" "20" "8D4840D6202CC371C32CE0576098");
    std::thread t1([&] { sock_t c = accept(l1, nullptr, nullptr); if (c != kBadSock) { sockSendAll(c, sbs.data(), sbs.size()); std::this_thread::sleep_for(std::chrono::milliseconds(800)); sockClose(c); } });
    std::thread t2([&] { sock_t c = accept(l2, nullptr, nullptr); if (c != kBadSock) { sockSendAll(c, beast.data(), beast.size()); std::this_thread::sleep_for(std::chrono::milliseconds(800)); sockClose(c); } });
    std::atomic<int> updates{0}, frames{0}, lastSrcU{0}, lastSrcF{0};
    AdsbFeedIn in;
    AdsbFeedIn::Sink sk;
    sk.update = [&](int src, const AdsbExtUpdate& u) { if (u.icao == 0x4840D6) { updates++; lastSrcU = src; } };
    sk.frame = [&](int src, const uint8_t*, int bits) { if (bits == 112) { frames++; lastSrcF = src; } };
    in.setSink(sk);
    in.setSources({"127.0.0.1:" + std::to_string(p1), "localhost:" + std::to_string(p2), "8.8.8.8:30005", "nonsense"}, false);
    for (int i = 0; i < 100 && (updates < 2 || frames < 1); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(updates == 2 && lastSrcU == 1, "SBS lines: %d from source %d", updates.load(), lastSrcU.load());
    CHECK(frames == 1 && lastSrcF == 2, "Beast frames: %d from source %d", frames.load(), lastSrcF.load());
    const auto info = in.info();
    CHECK(info.size() == 4 && info[0].format == "SBS" && info[0].msgs == 2 && info[1].format == "Beast" && info[1].msgs == 1, "info");
    CHECK(info.size() == 4 && !info[2].error.empty() && info[2].state == 0 && !info[3].error.empty(), "the outside host and the bad address are refused");
    in.stop();
    t1.join(); t2.join();
    sockClose(l1); sockClose(l2);
}

int main() {
    testCountries();
    testNNumbers();
    testCsv();
    testBeast();
    testSbs();
    testLan();
    testMerge();
    testStats();
    testFeedIn();
    printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
