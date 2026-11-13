// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radiosonde upload (sonde_upload.h): golden APRS lines for every sonde type OnAir decodes, the login and the beacon, units and rounding,
// the 9 character object name, and the sockets against a MOCK APRS-IS server on localhost (never a real server): login, beacon, objects,
// two targets at once, the packet counter, the per-sonde rate limit, reconnect, an unverified login, and "nothing while off".
#include "dect2/net/sonde_upload.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static sock_t listenOn(int& port) {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return kBadSock;
    socklen_t l = sizeof a; getsockname(ls, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
    sockSetNonBlocking(ls, true);
    return ls;
}
static sock_t acceptWait(sock_t ls, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c != kBadSock) { sockSetNonBlocking(c, true); return c; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return kBadSock;
}
// reads until n complete lines (or the time is up); the lines without the line end
static std::vector<std::string> readLines(sock_t c, size_t n, int ms) {
    std::string o; char b[1024]; std::vector<std::string> v;
    for (int i = 0; i < ms / 10 && v.size() < n; i++) {
        const long long k = recv(c, b, sizeof b, 0);
        if (k > 0) o.append(b, (size_t)k); else if (k == 0) break; else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        size_t p;
        while ((p = o.find("\r\n")) != std::string::npos) { v.push_back(o.substr(0, p)); o.erase(0, p + 2); }
    }
    return v;
}
template <class F> static bool waitFor(F f, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return false;
}
static void say(sock_t c, const char* s) { send(c, s, (int)strlen(s), 0); }

static SondeInfo mk(int kind, const char* type, const char* sub, const char* ser, double lat, double lon, double alt, bool vel, double hd, double hs, double vs, double freqMhz) {
    SondeInfo s; s.kind = kind; s.type = type; s.subtype = sub; s.serial = ser; s.hasPos = true; s.lat = lat; s.lon = lon; s.altM = alt;
    s.hasTime = true; s.unixTime = 1700000000;   // 2023-11-14 22:13:20 UTC
    if (vel) { s.hasVel = true; s.headingDeg = hd; s.hSpeed = hs; s.vSpeed = vs; }
    s.freqHz = freqMhz * 1e6;
    return s;
}

int main() {
    netInit();
    const std::string C = "DG9BFC-10";
    // ---- formats
    CHECK(sondeup::loginLine("dg9bfc-10", "12345").find("user DG9BFC-10 pass 12345 vers OnAir ") == 0, "login line");
    CHECK(sondeup::loginLine(C, "") .find(" pass -1 vers OnAir ") != std::string::npos, "empty passcode is -1");
    CHECK(sondeup::loginLine(C, "1").size() > 2 && sondeup::loginLine(C, "1").substr(sondeup::loginLine(C, "1").size() - 2) == "\r\n", "login ends with CRLF");
    CHECK(sondeup::beaconLine(C, 52.6812, 8.7954) == "DG9BFC-10>APRS,TCPIP*:!5240.87N/00847.72E&OnAir Radiosonde RX\r\n", "beacon: %s", sondeup::beaconLine(C, 52.6812, 8.7954).c_str());
    CHECK(sondeup::beaconLine(C, 91, 0).empty(), "beacon with a bad latitude is refused");
    CHECK(sondeup::latText(10.999999) == "1100.00N" && sondeup::lonText(-179.999999) == "18000.00W", "minutes carry into degrees: %s %s", sondeup::latText(10.999999).c_str(), sondeup::lonText(-179.999999).c_str());
    CHECK(sondeup::latText(0) == "0000.00N" && sondeup::latText(-0.5) == "0030.00S" && sondeup::lonText(-0.5) == "00030.00W", "zero and signs");

    struct G { SondeInfo s; const char* want; };
    SondeInfo rs41 = mk(SondeRs41, "RS41", "RS41-SG", "R1234567", 52.6812, 8.7954, 20000, true, 90, 6, 5.1, 403.5);
    rs41.hasTemp = true; rs41.tempC = -52.3; rs41.hasHumidity = true; rs41.humidity = 3.0; rs41.hasPressure = true; rs41.pressureHpa = 11.2;
    SondeInfo rs92 = mk(SondeRs92, "RS92", "RS92-SGP", "N1234567", 48.1, 11.5, 1234.5, true, 0, 0.2, -12.34, 404.1);
    SondeInfo dfm = mk(SondeDfm, "DFM", "DFM", "19012345", -33.9, 151.2, 100, true, 359.6, 10.5, 2.0, 403.0);
    dfm.hasTemp = true; dfm.tempC = 12.0;
    SondeInfo lms = mk(SondeLms6, "LMS6", "LMS6-1680", "123456", 52.0, 0.0, 500, true, 180, 3, 1, 403.5);
    SondeInfo mei = mk(SondeMeisei, "iMS-100", "iMS-100", "1234567", -0.5, -0.5, 0.4, true, 270, 20, -3, 402.7);
    mei.hasTemp = true; mei.tempC = 15.26; mei.hasHumidity = true; mei.humidity = 55.6; mei.hasPressure = true; mei.pressureHpa = 900.04;
    SondeInfo mrz = mk(SondeMrz, "MRZ", "MRZ-N1", "MRZ-5667-39155", 55.5, 37.5, 30000, false, 0, 0, 0, 400.0);
    SondeInfo im54 = mk(SondeImet54, "iMet-54", "iMet-54", "iMet54-12345678", 40.0, -100.0, 9999, true, 45, 1, 1, 403.0);
    im54.hasTemp = true; im54.tempC = -5.0;
    const G gold[] = {
        {rs41, "DG9BFC-10>APRS,TCPIP*:;R1234567 *221320h5240.87N/00847.72EO090/012/A=065617 Clb=5.1m/s t=-52.3C h=3.0% p=11.2hPa 403.500MHz Type=RS41-SG\r\n"},
        {rs92, "DG9BFC-10>APRS,TCPIP*:;N1234567 *221320h4806.00N/01130.00EO360/000/A=004050 Clb=-12.3m/s 404.100MHz Type=RS92-SGP\r\n"},
        {dfm, "DG9BFC-10>APRS,TCPIP*:;D19012345*221320h3354.00S/15112.00EO360/020/A=000328 Clb=2.0m/s t=12.0C 403.000MHz Type=DFM\r\n"},
        {lms, "DG9BFC-10>APRS,TCPIP*:;LMS61E240*221320h5200.00N/00000.00EO180/006/A=001640 Clb=1.0m/s 403.500MHz Type=LMS6\r\n"},
        {mei, "DG9BFC-10>APRS,TCPIP*:;IMS12D687*221320h0030.00S/00030.00WO270/039/A=000001 Clb=-3.0m/s t=15.3C h=55.6% p=900.0hPa 402.700MHz Type=IMS100\r\n"},
        {mrz, "DG9BFC-10>APRS,TCPIP*:;MRZC7C0D3*221320h5530.00N/03730.00EO/A=098425 400.000MHz Type=MRZ\r\n"},
        {im54, "DG9BFC-10>APRS,TCPIP*:;IMET45678*221320h4000.00N/10000.00WO045/002/A=032805 Clb=1.0m/s t=-5.0C 403.000MHz Type=iMet-54\r\n"},
    };
    for (const G& g : gold) {
        const std::string got = sondeup::objectLine(C, g.s, 0);
        CHECK(got == g.want, "golden %s:\n  got  %s  want %s", g.s.type.c_str(), got.c_str(), g.want);
        CHECK(sondeup::objectName(g.s).size() == 9, "object name of %s is 9 characters", g.s.type.c_str());
    }
    CHECK(sondeup::objectName(rs41) == "R1234567 ", "name padded with a space");
    // nothing invented, nothing sent without what is needed
    SondeInfo m10 = mk(SondeM10, "M10", "M10", "123-4-12345", 50, 10, 1000, true, 1, 1, 1, 403);
    SondeInfo m20 = mk(SondeM20, "M20", "M20", "123-4-12345", 50, 10, 1000, true, 1, 1, 1, 403);
    SondeInfo im1 = mk(SondeImet, "iMet", "iMet-1/4", "iMet-403500", 50, 10, 1000, true, 1, 1, 1, 403);
    std::string why;
    CHECK(sondeup::objectLine(C, m10, 0, &why).empty() && !why.empty(), "M10 has no object name");
    CHECK(sondeup::objectLine(C, m20, 0).empty() && sondeup::objectLine(C, im1, 0).empty(), "M20 and iMet-1/4 are not sent");
    SondeInfo np = rs41; np.hasPos = false;
    CHECK(sondeup::objectLine(C, np, 0, &why).empty() && why == "no valid position", "no position, no line");
    np = rs41; np.lat = 0; np.lon = 0;
    CHECK(sondeup::objectLine(C, np, 0).empty(), "0,0 is not a position");
    SondeInfo notime = rs41; notime.hasTime = false;
    CHECK(sondeup::objectLine(C, notime, 1700000000.9).find("*221320h") != std::string::npos, "wall time used when the sonde sent none");
    SondeInfo nof = dfm; nof.freqHz = 0;
    CHECK(sondeup::objectLine(C, nof, 0).find("MHz") == std::string::npos, "no frequency field without a frequency");
    SondeInfo neg = rs41; neg.altM = -100;
    CHECK(sondeup::objectLine(C, neg, 0).find("/A=-00328") != std::string::npos, "negative altitude: %s", sondeup::objectLine(C, neg, 0).c_str());
    SondeInfo lowser = rs41; lowser.serial = "R12?4567";
    CHECK(sondeup::objectLine(C, lowser, 0).empty(), "an unreadable serial is not sent");

    // ---- sockets against a mock server
    int p1 = 0, p2 = 0;
    sock_t l1 = listenOn(p1), l2 = listenOn(p2);
    CHECK(l1 != kBadSock && l2 != kBadSock, "loopback listeners");
    if (l1 == kBadSock || l2 == kBadSock) return 1;
    SondeUploadTiming tm; tm.reconnectMinS = 0.3; tm.objectGapS = 1.0; tm.beaconEveryS = 1.5;
    SondeUpload& U = SondeUpload::get();
    U.setTiming(tm);
    U.setStation(52.6812, 8.7954, true);
    SondeUploadTarget a; a.name = "A"; a.host = "127.0.0.1"; a.port = p1; a.call = C; a.pass = "12345";
    SondeUploadTarget b = a; b.name = "B"; b.port = p2; b.call = "DG9BFC-11"; b.pass.clear();
    U.configure({a, b});   // both off
    CHECK(!U.wanted(), "nothing wanted while every target is off");
    U.frame(rs41, 1);
    CHECK(acceptWait(l1, 300) == kBadSock, "no connection while disabled");
    CHECK(U.status(0).state == "off", "status off");

    a.enabled = b.enabled = true;
    U.configure({a, b});
    CHECK(U.wanted(), "wanted once enabled");
    sock_t c1 = acceptWait(l1, 3000), c2 = acceptWait(l2, 3000);
    CHECK(c1 != kBadSock && c2 != kBadSock, "both targets connect");
    if (c1 == kBadSock || c2 == kBadSock) { printf("sonde_upload: %d failures\n", ++fails); return 1; }
    std::vector<std::string> h1 = readLines(c1, 2, 2000), h2 = readLines(c2, 2, 2000);
    CHECK(h1.size() == 2 && h1[0].rfind("user DG9BFC-10 pass 12345 vers OnAir ", 0) == 0, "target A login: %s", h1.empty() ? "(none)" : h1[0].c_str());
    CHECK(h1.size() == 2 && h1[1] == "DG9BFC-10>APRS,TCPIP*:!5240.87N/00847.72E&OnAir Radiosonde RX", "target A beacon on connect");
    CHECK(h2.size() == 2 && h2[0].rfind("user DG9BFC-11 pass -1 vers OnAir ", 0) == 0, "target B login with -1");
    say(c1, "# logresp DG9BFC-10 verified, server T2TEST\r\n");
    say(c2, "# logresp DG9BFC-11 verified, server T2TEST\r\n");
    CHECK(waitFor([&] { return U.status(0).verified && U.status(1).verified; }, 2000), "verified state read from the server");

    U.frame(rs41, 1700000000);
    U.frame(rs41, 1700000000);   // the same sonde again at once: rate-limited
    U.frame(dfm, 1700000000);    // another sonde: goes
    U.frame(m10, 1700000000);    // M10: never
    const std::string wantRs = "DG9BFC-10>APRS,TCPIP*:;R1234567 *221320h5240.87N/00847.72EO090/012/A=065617 Clb=5.1m/s t=-52.3C h=3.0% p=11.2hPa 403.500MHz Type=RS41-SG";
    std::vector<std::string> o1 = readLines(c1, 2, 2000), o2 = readLines(c2, 2, 2000);
    CHECK(o1.size() == 2 && o1[0] == wantRs, "target A object: %s", o1.empty() ? "(none)" : o1[0].c_str());
    CHECK(o1.size() == 2 && o1[1].find(";D19012345*") != std::string::npos, "target A second sonde");
    CHECK(o2.size() == 2 && o2[0] == "DG9BFC-11" + wantRs.substr(9), "target B object uses its own callsign: %s", o2.empty() ? "(none)" : o2[0].c_str());
    CHECK(waitFor([&] { return U.status(0).sent == 2 && U.status(1).sent == 2; }, 2000), "counters: A %llu, B %llu", (unsigned long long)U.status(0).sent, (unsigned long long)U.status(1).sent);
    CHECK(U.status(0).lastLine.find(";D19012345*") != std::string::npos, "last line shown");
    CHECK(U.status(0).lastLine.find("12345") == std::string::npos || U.status(0).lastLine.find("pass") == std::string::npos, "the passcode is not shown");
    CHECK(waitFor([&] { return U.status(0).beacons >= 2; }, 4000), "beacon repeats (%llu)", (unsigned long long)U.status(0).beacons);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    U.frame(rs41, 1700000010);   // after the gap: goes again
    CHECK(waitFor([&] { return U.status(0).sent == 3; }, 3000), "the same sonde again after the gap");

    // the server goes away: reconnect with a wait, login and beacon again
    sockClose(c1);
    CHECK(waitFor([&] { return U.status(0).reconnects >= 1; }, 5000), "reconnect counted");
    sock_t c1b = acceptWait(l1, 8000);
    CHECK(c1b != kBadSock, "target A reconnects");
    if (c1b != kBadSock) {
        std::vector<std::string> r = readLines(c1b, 2, 2000);
        CHECK(r.size() >= 1 && r[0].rfind("user DG9BFC-10 ", 0) == 0, "login again after the reconnect");
        // an unverified answer: sonde lines are no longer sent and are counted as dropped
        say(c1b, "# logresp DG9BFC-10 unverified, server T2TEST\r\n");
        CHECK(waitFor([&] { return !U.status(0).lastError.empty(); }, 2000), "unverified login reported");
        const uint64_t before = U.status(0).sent;
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        U.frame(mei, 1700000000);
        CHECK(waitFor([&] { return U.status(0).dropped >= 1; }, 2000), "dropped while unverified");
        CHECK(U.status(0).sent == before, "nothing sent while unverified");
        sockClose(c1b);
    }

    // disabling one stops its data
    b.enabled = false; a.enabled = true;
    U.configure({a, b});
    CHECK(waitFor([&] { return U.status(1).state == "off"; }, 1000), "disabled target shows off");

    U.stop();
    CHECK(!U.wanted(), "stopped");
    if (c2 != kBadSock) sockClose(c2);
    sockClose(l1); sockClose(l2);
    printf(fails ? "sonde_upload: %d failures\n" : "sonde_upload: OK\n", fails);
    return fails ? 1 : 0;
}
