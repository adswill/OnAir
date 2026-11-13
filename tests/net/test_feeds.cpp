// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Data feeds out: the formats (SBS, Beast, AIVDM, KISS, APRS-IS, ACARS / VDL2 JSON) and the feeds' sockets on localhost.
#include "dect2/net/feeds.h"
#include "dect2/ais/ais_proto.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> fromHex(const char* h) {
    std::vector<uint8_t> o;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) { unsigned v; sscanf(h + i, "%2x", &v); o.push_back((uint8_t)v); }
    return o;
}
static std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> o; size_t p = 0;
    for (;;) { size_t e = s.find(c, p); o.push_back(s.substr(p, e == std::string::npos ? std::string::npos : e - p)); if (e == std::string::npos) break; p = e + 1; }
    return o;
}
static FeedAdsbFrame frameOf(const char* hex) {
    FeedAdsbFrame f;
    const auto b = fromHex(hex);
    memcpy(f.bytes, b.data(), b.size());
    f.bits = (int)b.size() * 8;
    adsb::decodeMsg(f.bytes, f.bits, f.msg);
    f.icao = f.msg.icao;
    f.timeSec = 1.5;
    f.levelDbfs = -20;
    return f;
}

static void testSbs() {
    FeedAdsbFrame id = frameOf("8D4840D6202CC371C32CE0576098");   // identification KLM1023
    std::string l = feedfmt::sbsLine(id, 1760000000123LL);
    CHECK(l.size() > 2 && l.substr(l.size() - 2) == "\r\n", "SBS line ends in CRLF");
    auto f = split(l.substr(0, l.size() - 2), ',');
    CHECK(f.size() == 22, "SBS: 22 fields, got %zu (%s)", f.size(), l.c_str());
    if (f.size() == 22) {
        CHECK(f[0] == "MSG" && f[1] == "1" && f[4] == "4840D6", "SBS ident header: %s", l.c_str());
        CHECK(f[10] == "KLM1023", "SBS callsign '%s'", f[10].c_str());
        CHECK(f[6].size() == 10 && f[6][4] == '/' && f[7].size() == 12 && f[7][8] == '.' && f[7].substr(9) == "123", "SBS date/time '%s' '%s'", f[6].c_str(), f[7].c_str());
    }
    FeedAdsbFrame pos = frameOf("8D40621D58C382D690C8AC2863A7");   // airborne position, 38000 ft
    pos.hasPos = true; pos.lat = 52.25720; pos.lon = 3.91937;
    l = feedfmt::sbsLine(pos, 1760000000000LL);
    f = split(l.substr(0, l.size() - 2), ',');
    CHECK(f.size() == 22 && f[1] == "3" && f[4] == "40621D" && f[11] == "38000" && f[14] == "52.25720" && f[15] == "3.91937" && f[21] == "0", "SBS position: %s", l.c_str());
    FeedAdsbFrame vel = frameOf("8D485020994409940838175B284F");   // velocity: 159 kt, track 182.9, -832 fpm
    l = feedfmt::sbsLine(vel, 1760000000000LL);
    f = split(l.substr(0, l.size() - 2), ',');
    CHECK(f.size() == 22 && f[1] == "4" && f[12] == "159" && f[13] == "183" && f[16] == "-832", "SBS velocity: %s", l.c_str());
    FeedAdsbFrame anon = pos; anon.nonIcao = true;
    CHECK(feedfmt::sbsLine(anon, 0).find(",~40621D,") != std::string::npos, "SBS non-ICAO address marked ~");
}

static void testBeast() {
    uint8_t b[14] = {0x8D, 0x1A, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x1A};
    const std::string o = feedfmt::beastFrame(b, 112, 0x00001A000102ull, 0x1A);
    // 2 + 6 ts (one 0x1A doubled) + 1 signal (doubled) + 14 data (two doubled)
    CHECK(o.size() == 2 + 7 + 2 + 16, "Beast length %zu", o.size());
    CHECK((uint8_t)o[0] == 0x1A && o[1] == '3', "Beast header");
    const uint8_t want[] = {0x1A, '3', 0x00, 0x00, 0x1A, 0x1A, 0x00, 0x01, 0x02, 0x1A, 0x1A, 0x8D, 0x1A, 0x1A, 0x40};
    CHECK(memcmp(o.data(), want, sizeof want) == 0, "Beast escapes 0x1A in timestamp, signal and data");
    CHECK((uint8_t)o[o.size() - 1] == 0x1A && (uint8_t)o[o.size() - 2] == 0x1A, "Beast last data byte escaped");
    const std::string s = feedfmt::beastFrame(b, 56, 0, 0x80);
    CHECK(s[1] == '2' && s.size() == 2 + 6 + 1 + 7 + 1, "Beast short frame (one 0x1A in data): %zu", s.size());
    CHECK(feedfmt::beastSignal(0) == 255 && feedfmt::beastSignal(-6.0206f) == 128 && feedfmt::beastSignal(-200) == 0, "Beast signal level");
}

static void testAis() {
    // gpsd's example sentence: rebuilding it from its own payload gives the same sentence, checksum included
    const std::string known = "!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C";
    std::string chars; int fill = 0, cnt = 0, no = 0; char ch = 0;
    CHECK(ais::parseNmea(known, chars, fill, cnt, no, &ch), "parse the known sentence");
    const auto out = ais::toNmea(ais::unarmour(chars, fill), ch, 0);
    CHECK(out.size() == 1 && out[0] == known, "AIVDM rebuilt: %s", out.empty() ? "" : out[0].c_str());
    // a type 5 message (424 bits = 71 characters, 2 fill bits) splits into two sentences with a sequential id
    ais::Bits b(424, 0);
    for (size_t i = 0; i < b.size(); i++) b[i] = (i * 7 + 3) % 5 == 0;
    b[0] = 0; b[1] = 0; b[2] = 0; b[3] = 1; b[4] = 0; b[5] = 1;
    const auto two = ais::toNmea(b, 'A', 3);
    CHECK(two.size() == 2, "type 5 splits in two: %zu", two.size());
    if (two.size() == 2) {
        CHECK(two[0].rfind("!AIVDM,2,1,3,A,", 0) == 0 && two[1].rfind("!AIVDM,2,2,3,A,", 0) == 0, "fragment headers: %s / %s", two[0].c_str(), two[1].c_str());
        CHECK(two[0].substr(two[0].size() - 5, 3) == ",0*" && two[1].substr(two[1].size() - 5, 3) == ",2*", "fill bits only on the last fragment");
        for (const auto& s : two) {
            const size_t star = s.find('*');
            unsigned want = 0; sscanf(s.c_str() + star + 1, "%2x", &want);
            CHECK(ais::nmeaChecksum(s.substr(1, star - 1)) == want, "checksum of %s", s.c_str());
        }
    }
}

static void testAprs() {
    CHECK(feedfmt::aprsIsLine("N0CALL-9", "APRS", "WIDE1-1*,WIDE2-1", "!4903.50N/07201.75W-Test\r", "ly1abc", true)
          == "N0CALL-9>APRS,WIDE1-1*,WIDE2-1,qAR,LY1ABC:!4903.50N/07201.75W-Test\r\n", "APRS-IS line");
    CHECK(feedfmt::aprsIsLine("N0CALL", "APRS", "", ">status", "LY1ABC", true) == "N0CALL>APRS,qAR,LY1ABC:>status\r\n", "APRS-IS line without a path");
    CHECK(feedfmt::aprsIsLine("N0CALL", "APRS", "NOGATE", ">x", "LY1ABC", true).empty(), "NOGATE not gated");
    CHECK(feedfmt::aprsIsLine("N0CALL", "APRS", "WIDE2-1,RFONLY", ">x", "LY1ABC", true).empty(), "RFONLY not gated");
    CHECK(feedfmt::aprsIsLine("N0CALL", "APRS", "TCPIP*", ">x", "LY1ABC", true).empty(), "TCPIP not gated");
    CHECK(feedfmt::aprsIsLine("N0CALL", "APRS", "", "}W1AW>APRS,TCPIP:>x", "LY1ABC", true).empty(), "third party not gated");
    CHECK(feedfmt::aprsIsLine("N0CALL", "APRS", "", ">x", "LY1ABC", false).empty(), "not a UI / F0 frame");
    CHECK(feedfmt::aprsIsLine("N0CALL", "APRS", "", ">x", "", true).empty(), "no callsign set");
    CHECK(feedfmt::aprsIsLogin("ly1abc-10", "12345").rfind("user LY1ABC-10 pass 12345 vers OnAir ", 0) == 0, "APRS-IS login");
    const uint8_t d[] = {0x01, 0xC0, 0x02, 0xDB, 0x03};
    const std::string k = feedfmt::kissFrame(d, sizeof d);
    const uint8_t want[] = {0xC0, 0x00, 0x01, 0xDB, 0xDC, 0x02, 0xDB, 0xDD, 0x03, 0xC0};
    CHECK(k.size() == sizeof want && memcmp(k.data(), want, sizeof want) == 0, "KISS escapes");
}

static int depth(const std::string& j) {   // brace balance outside strings; -1 when it goes negative
    int d = 0; bool str = false;
    for (size_t i = 0; i < j.size(); i++) {
        if (str) { if (j[i] == '\\') i++; else if (j[i] == '"') str = false; continue; }
        if (j[i] == '"') str = true; else if (j[i] == '{') d++; else if (j[i] == '}') { if (--d < 0) return -1; }
    }
    return str ? 99 : d;
}

static void testJson() {
    AcarsMessage m;
    m.wallTime = 1760000000; m.freqHz = 131550000; m.levelDb = -23.4f; m.mode = '2'; m.reg = "N12345"; m.ack = '!';
    m.label = "H1"; m.blockId = '4'; m.downlink = true; m.msgNum = "M01"; m.msgSeq = 'A'; m.flightId = "AB1234"; m.text = "a\"b\nc";
    const std::string j = feedfmt::acarsJson(m, "test");
    for (const char* k : {"\"timestamp\":1760000000.000", "\"station_id\":\"test\"", "\"freq\":131.550", "\"level\":-23.4", "\"mode\":\"2\"", "\"label\":\"H1\"",
                          "\"block_id\":\"4\"", "\"ack\":false", "\"tail\":\"N12345\"", "\"flight\":\"AB1234\"", "\"msgno\":\"M01A\"", "\"text\":\"a\\\"b\\nc\"", "\"end\":true"})
        CHECK(j.find(k) != std::string::npos, "ACARS JSON has %s: %s", k, j.c_str());
    CHECK(depth(j) == 0 && j.front() == '{' && j.back() == '}', "ACARS JSON balanced: %s", j.c_str());

    Vdl2Message v;
    v.wallTime = 1760000000; v.freqHz = 136975000; v.levelDb = -30; v.snrDb = 20; v.src = 0x4CA7B5; v.srcType = 1; v.dst = 0x10916; v.dstType = 4;
    v.frameKind = 'I'; v.frameName = "I"; v.ns = 2; v.nr = 5; v.acars = true; v.acarsCrcOk = true; v.reg = "EI-FMV"; v.label = "H1"; v.blockId = '5';
    v.flight = "RYR1AB"; v.msgNum = "D01A"; v.acarsText = "HELLO";
    const std::string w = feedfmt::vdl2Json(v, "test");
    for (const char* k : {"{\"vdl2\":{", "\"freq\":136975000", "\"t\":{\"sec\":1760000000", "\"src\":{\"addr\":\"4CA7B5\",\"type\":\"Aircraft\",\"status\":\"Airborne\"}",
                          "\"dst\":{\"addr\":\"010916\",\"type\":\"Ground station\"}", "\"frame_type\":\"I\"", "\"acars\":{\"err\":false,\"crc_ok\":true", "\"reg\":\"EI-FMV\"",
                          "\"msg_text\":\"HELLO\"", "\"noise_level\":-50.0"})
        CHECK(w.find(k) != std::string::npos, "VDL2 JSON has %s: %s", k, w.c_str());
    CHECK(depth(w) == 0, "VDL2 JSON balanced: %s", w.c_str());

    GnssFix fix;
    fix.valid = true; fix.latDeg = 54.6872; fix.lonDeg = 25.2797; fix.heightM = 112; fix.nSats = 8; fix.hdop = 1.2f;
    fix.timeValid = true; fix.year = 2026; fix.month = 10; fix.day = 14; fix.hour = 12; fix.minute = 34; fix.second = 56.0;
    const std::string rmc = feedfmt::gnssRmc(fix), gga = feedfmt::gnssGga(fix);
    CHECK(rmc.rfind("$GPRMC,", 0) == 0 && rmc.find("*") != std::string::npos && rmc.size() >= 4 && rmc.substr(rmc.size() - 2) == "\r\n", "RMC: %s", rmc.c_str());
    CHECK(gga.rfind("$GPGGA,", 0) == 0 && gga.find("54") != std::string::npos, "GGA: %s", gga.c_str());
    const std::string ble = feedfmt::bleJson("C0:FF:EE:12:34:56", "Lock", -42.5f);
    CHECK(ble.find("\"type\":\"ble\"") != std::string::npos && ble.find("\"mac\":\"C0:FF:EE:12:34:56\"") != std::string::npos && ble.find("\"rssi\":-42.5") != std::string::npos, "BLE JSON: %s", ble.c_str());
    CHECK(depth(ble) == 0, "BLE JSON balanced");
    const std::string lr = feedfmt::loraJson("meshtastic", "01 02 AA", 9.5f, 869525000, 11, "\"from\":\"!1a2b3c4d\"");
    CHECK(lr.find("\"type\":\"meshtastic\"") != std::string::npos && lr.find("\"hex\":\"01 02 AA\"") != std::string::npos && lr.find("\"sf\":11") != std::string::npos && lr.find("\"from\":\"!1a2b3c4d\"") != std::string::npos, "LoRa JSON: %s", lr.c_str());
    CHECK(depth(lr) == 0, "LoRa JSON balanced");
}

// ---------------------------------------------------------------- sockets on localhost

static int freePort(int type) {
    sock_t s = socket(AF_INET, type, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(s, (sockaddr*)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(s, (sockaddr*)&a, &al);
    const int p = ntohs(a.sin_port);
    sockClose(s);
    return p;
}
static sock_t udpListen(int& port) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(s, (sockaddr*)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(s, (sockaddr*)&a, &al);
    port = ntohs(a.sin_port);
    sockRecvTimeout(s, 2000);
    return s;
}
static bool waitFor(const std::function<bool()>& ok, int ms = 3000) {
    for (int i = 0; i < ms / 10; i++) { if (ok()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return ok();
}
static std::string recvUntil(sock_t s, size_t n) {
    std::string o; char b[4096];
    while (o.size() < n) { const long long k = sockRecv(s, b, sizeof b); if (k <= 0) break; o.append(b, (size_t)k); }
    return o;
}

static void testLoopback() {
    netInit();
    FeedConfig c;
    c.on[kFeedSbs] = c.on[kFeedBeast] = c.on[kFeedKiss] = c.on[kFeedAis] = c.on[kFeedAcars] = true;
    c.sbsPort = freePort(SOCK_STREAM); c.beastPort = freePort(SOCK_STREAM); c.kissPort = freePort(SOCK_STREAM);
    sock_t aisRx = udpListen(c.aisPort), acarsRx = udpListen(c.acarsPort);
    Feeds& F = Feeds::get();
    F.configure(c);
    CHECK(waitFor([&] { return F.status(kFeedSbs).active && F.status(kFeedBeast).active && F.status(kFeedKiss).active && F.status(kFeedAis).active; }), "feeds came up: %s / %s",
          F.status(kFeedSbs).state.c_str(), F.status(kFeedSbs).lastError.c_str());
    std::string err;
    sock_t sbs = tcpConnect("127.0.0.1", c.sbsPort, 2000, err), beast = tcpConnect("127.0.0.1", c.beastPort, 2000, err), kiss = tcpConnect("127.0.0.1", c.kissPort, 2000, err);
    CHECK(sbs != kBadSock && beast != kBadSock && kiss != kBadSock, "connect: %s", err.c_str());
    sock_t sbs2 = tcpConnect("127.0.0.1", c.sbsPort, 2000, err);
    CHECK(waitFor([&] { return F.status(kFeedSbs).clients == 2 && F.status(kFeedBeast).clients == 1 && F.status(kFeedKiss).clients == 1; }), "clients counted: %d", F.status(kFeedSbs).clients);
    for (sock_t s : {sbs, sbs2, beast, kiss}) sockRecvTimeout(s, 2000);

    CHECK(F.wantAdsb() && F.wantAis() && F.wantAprs() && F.wantAcars() && !F.wantVdl2(), "wanted feeds");
    FeedAdsbFrame id = frameOf("8D4840D6202CC371C32CE0576098");
    id.levelDbfs = -10;   // signal byte 81 (-20 dBFS would be 0x1A, escaped)
    F.adsb(id);
    const std::string line = recvUntil(sbs, 10);
    CHECK(line.rfind("MSG,1,1,1,4840D6,", 0) == 0 && line.find("KLM1023") != std::string::npos, "SBS over TCP: %s", line.c_str());
    CHECK(recvUntil(sbs2, 10).rfind("MSG,1", 0) == 0, "second SBS client gets it too");
    const std::string be = recvUntil(beast, 2 + 6 + 1 + 14);
    CHECK(be.size() >= 23 && (uint8_t)be[0] == 0x1A && be[1] == '3' && (uint8_t)be[9] == 0x8D, "Beast over TCP (%zu bytes)", be.size());
    const uint64_t ts = ((uint64_t)(uint8_t)be[2] << 40) | ((uint64_t)(uint8_t)be[3] << 32) | ((uint64_t)(uint8_t)be[4] << 24) | ((uint64_t)(uint8_t)be[5] << 16) | ((uint64_t)(uint8_t)be[6] << 8) | (uint8_t)be[7];
    CHECK(ts == 18000000, "Beast timestamp 1.5 s at 12 MHz: %llu", (unsigned long long)ts);

    F.aprs({0x82, 0xA0, 0xA4, 0xA6, 0x40, 0x40, 0x60, 0x9C, 0x60, 0x86, 0x82, 0x98, 0x98, 0x61, 0x03, 0xF0, '>', 'h', 'i'}, "N0CALL", "APRS", "", ">hi", true);
    const std::string kf = recvUntil(kiss, 22);
    CHECK(kf.size() == 22 && (uint8_t)kf[0] == 0xC0 && kf[1] == 0 && (uint8_t)kf.back() == 0xC0, "KISS over TCP (%zu bytes)", kf.size());

    F.ais({"!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C"});
    char buf[2048];
    long long k = sockRecv(aisRx, buf, sizeof buf);
    CHECK(k > 0 && std::string(buf, (size_t)k) == "!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C\r\n", "AIVDM over UDP");

    AcarsMessage m; m.label = "H1"; m.reg = "N1"; m.text = "X";
    F.acars(m);
    k = sockRecv(acarsRx, buf, sizeof buf);
    CHECK(k > 0 && std::string(buf, (size_t)k).find("\"label\":\"H1\"") != std::string::npos, "ACARS JSON over UDP");

    c.on[kFeedGnss] = c.on[kFeedBle] = c.on[kFeedLora] = true;
    sock_t gnssRx = udpListen(c.gnssPort), bleRx = udpListen(c.blePort), loraRx = udpListen(c.loraPort);
    F.configure(c);
    CHECK(waitFor([&] { return F.status(kFeedGnss).active && F.status(kFeedBle).active && F.status(kFeedLora).active; }), "new UDP feeds came up");
    GnssFix fx; fx.valid = true; fx.latDeg = 1; fx.lonDeg = 2; fx.nSats = 4;
    F.gnss(fx);
    k = sockRecv(gnssRx, buf, sizeof buf);
    CHECK(k > 0 && std::string(buf, (size_t)k).rfind("$GPRMC,", 0) == 0, "GNSS NMEA over UDP");
    F.ble("AA:BB:CC:DD:EE:FF", "Phone", -50.f);
    k = sockRecv(bleRx, buf, sizeof buf);
    CHECK(k > 0 && std::string(buf, (size_t)k).find("\"mac\":\"AA:BB:CC:DD:EE:FF\"") != std::string::npos, "BLE JSON over UDP");
    F.lora("lora", "AB CD", 3.f, 868100000, 7);
    k = sockRecv(loraRx, buf, sizeof buf);
    CHECK(k > 0 && std::string(buf, (size_t)k).find("\"type\":\"lora\"") != std::string::npos, "LoRa JSON over UDP");

    // a client that goes away is dropped from the count
    sockClose(sbs2);
    F.adsb(id);
    CHECK(waitFor([&] { F.adsb(id); return F.status(kFeedSbs).clients == 1; }), "closed client dropped: %d", F.status(kFeedSbs).clients);
    CHECK(F.status(kFeedSbs).sent >= 2, "sent counted: %llu", (unsigned long long)F.status(kFeedSbs).sent);

    // turning a feed off closes its port
    c.on[kFeedSbs] = false;
    F.configure(c);
    CHECK(waitFor([&] { return !F.status(kFeedSbs).active; }), "SBS off");
    CHECK(!F.wantVdl2() && F.status(kFeedSbs).state == "off", "state off");
    // APRS-IS without a callsign stays idle
    c.on[kFeedAprsIs] = true;
    F.configure(c);
    CHECK(waitFor([&] { return F.status(kFeedAprsIs).state == "needs a callsign"; }), "APRS-IS needs a callsign: %s", F.status(kFeedAprsIs).state.c_str());
    F.stop();
    CHECK(!F.wantAdsb() && !F.wantAis(), "stopped");
    for (sock_t s : {sbs, beast, kiss, aisRx, acarsRx, gnssRx, bleRx, loraRx}) sockClose(s);
}

int main() {
    testSbs();
    testBeast();
    testAis();
    testAprs();
    testJson();
    testLoopback();
    if (fails) { printf("feeds: %d checks FAILED\n", fails); return 1; }
    printf("feeds: all checks passed\n");
    return 0;
}
