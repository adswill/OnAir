// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The NMEA text of the selected sonde (dect2/sonde/sonde_nmea.h): checksum, degrees and minutes, knots, the GGA and RMC sentences, what
// is left out; and the sender (sonde_nmea_out.h) writing them to a file standing in for a serial port, and over UDP and TCP on localhost.
#include "dect2/sonde/sonde_nmea.h"
#include "dect2/sonde/sonde_nmea_out.h"
#include "../../core/src/net/tcp_client.h"
#include "../common/test_util.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

using namespace dect2;

static SondeInfo sonde() {
    SondeInfo s; s.kind = SondeRs41; s.type = "RS41"; s.serial = "T1234567";
    s.hasPos = true; s.lat = 52.6278; s.lon = 8.778; s.altM = 12345.6;
    s.hasVel = true; s.hSpeed = 10.0; s.vSpeed = 5; s.headingDeg = 275.5; s.sats = 9;
    s.hasTime = true; s.unixTime = 1700000000.0;   // 2023-11-14 22:13:20 UTC
    s.lastHeardS = 1;
    return s;
}

static bool crcOk(const std::string& line) {
    const size_t st = line.find('$'), en = line.find('*');
    return st == 0 && en != std::string::npos && line.compare(en + 1, 2, sondenmea::checksum(line.substr(1, en - 1))) == 0 && line.substr(en + 3) == "\r\n";
}

int main() {
    // the textbook checksum example
    const std::string ex = "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";   // the well-known example sentence, checksum 47
    CHECK(sondenmea::checksum(ex) == "47", "checksum of the GGA example: %s", sondenmea::checksum(ex).c_str());
    CHECK(sondenmea::sentence(ex) == "$" + ex + "*47\r\n", "sentence framing");

    CHECK(sondenmea::degMin(52.6278, true) == "5237.6680,N", "lat %s", sondenmea::degMin(52.6278, true).c_str());
    CHECK(sondenmea::degMin(8.778, false) == "00846.6800,E", "lon %s", sondenmea::degMin(8.778, false).c_str());
    CHECK(sondenmea::degMin(-33.5, true) == "3330.0000,S" && sondenmea::degMin(-122.25, false) == "12215.0000,W", "S and W");
    CHECK(sondenmea::degMin(10.999999999, true) == "1100.0000,N", "minutes round up into the degree: %s", sondenmea::degMin(10.999999999, true).c_str());
    CHECK(std::fabs(sondenmea::knots(10) - 19.43844) < 1e-4 && sondenmea::knots(0) == 0, "knots");
    CHECK(sondenmea::timeField(1700000000.5) == "221320.50" && sondenmea::dateField(1700000000) == "141123", "time %s date %s", sondenmea::timeField(1700000000.5).c_str(), sondenmea::dateField(1700000000).c_str());

    SondeInfo s = sonde();
    const std::string g = sondenmea::gga(s, 1);
    CHECK(g == "$GPGGA,221320.00,5237.6680,N,00846.6800,E,1,9,1.0,12345.6,M,,M,,*" + sondenmea::checksum("GPGGA,221320.00,5237.6680,N,00846.6800,E,1,9,1.0,12345.6,M,,M,,") + "\r\n", "GGA: %s", g.c_str());
    const std::string r = sondenmea::rmc(s, 1);
    CHECK(r.compare(0, 63, "$GPRMC,221320.00,A,5237.6680,N,00846.6800,E,19.4,275.5,141123,,") == 0 && crcOk(r), "RMC: %s", r.c_str());
    CHECK(crcOk(g), "GGA checksum");
    const std::string both = sondenmea::sentences(s, 1);
    CHECK(both == g + r, "sentences = GGA + RMC");

    // sats unknown: the field stays empty; no sonde clock: the computer's; no velocity: speed and course 0
    SondeInfo u = sonde(); u.sats = -1; u.hasTime = false; u.hasVel = false;
    const std::string gu = sondenmea::gga(u, 1700000000.0);
    CHECK(gu.find(",1,,1.0,") != std::string::npos && gu.find("221320.00") != std::string::npos && crcOk(gu), "GGA without satellites: %s", gu.c_str());
    CHECK(sondenmea::rmc(u, 1700000000.0).find(",A,5237.6680,N,00846.6800,E,0.0,0.0,141123,") != std::string::npos, "RMC without velocity");
    SondeInfo w = sonde(); w.headingDeg = -90;
    CHECK(sondenmea::rmc(w, 1).find(",270.0,") != std::string::npos, "a negative heading wraps");

    // nothing to send
    SondeInfo n1 = sonde(); n1.hasPos = false;
    SondeInfo n2 = sonde(); n2.lastHeardS = 60;
    SondeInfo n3 = sonde(); n3.sats = 2;
    SondeInfo n4 = sonde(); n4.lat = 0; n4.lon = 0;
    CHECK(sondenmea::sentences(n1, 1).empty() && sondenmea::sentences(n2, 1).empty() && sondenmea::sentences(n3, 1).empty() && sondenmea::sentences(n4, 1).empty(), "no position, old frame, 2 satellites, 0/0: nothing is sent");

    // the sender: a file stands in for the serial port (a pty or a plain file takes the same writes)
    const std::string path = "sonde_nmea_test_out.txt";
    std::remove(path.c_str());
    SondeNmeaConfig c; c.enabled = true; c.kind = 0; c.path = path; c.baud = 4800;
    SondeNmeaOut& o = SondeNmeaOut::get();
    o.configure(c);
    o.send(both);
    for (int i = 0; i < 60 && o.sentCount() < 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    o.stop();
    std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf();
    CHECK(ss.str() == both, "the port got the sentences unchanged (%zu bytes)", ss.str().size());
    CHECK(o.status() == "off", "status off after stop");
    std::remove(path.c_str());

    // UDP on localhost
    netInit();
    sock_t us = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    CHECK(us != kBadSock && bind(us, (sockaddr*)&a, sizeof a) == 0, "udp bind");
    socklen_t al = sizeof a; getsockname(us, (sockaddr*)&a, &al);
    sockRecvTimeout(us, 3000);
    SondeNmeaConfig cu; cu.enabled = true; cu.kind = 2; cu.host = "127.0.0.1"; cu.port = ntohs(a.sin_port);
    o.configure(cu); o.send(both);
    char buf[512]; const long long k = sockRecv(us, buf, sizeof buf);
    o.stop();
    CHECK(k == (long long)both.size() && std::string(buf, (size_t)k) == both, "UDP datagram holds both sentences (%lld)", k);
    sockClose(us);
    return testSummary("sonde_nmea");
}
