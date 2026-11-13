// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Chase GPS (sonde_chase.h): NMEA from a serial GPS or a file, or gpsd over TCP.
#include "dect2/sonde/sonde_chase.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/timecompat.h"
#include "dect2/mcp/mcp.h"
#include "dect2/rfi/rfi.h"
#include "../net/tcp_client.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

namespace dect2 {
namespace chase {

bool nmeaLine(const std::string& line, ChaseFix& fix, double now) {
    rfi::Fix f;
    if (!rfi::parseNmea(line, f)) return false;
    if (!f.valid) return true;
    fix.valid = true; fix.lat = f.lat; fix.lon = f.lon; fix.steadyT = now;
    const size_t g = line.find("GGA,");
    if (g != std::string::npos && g >= 3 && g <= 6) {   // $GPGGA / $GNGGA: field 7 satellites, field 9 altitude above mean sea level
        std::vector<std::string> fl;
        size_t p = line.find(',');
        while (p != std::string::npos) { const size_t q = line.find_first_of(",*", p + 1); fl.push_back(line.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1)); if (q == std::string::npos || line[q] == '*') break; p = q; }
        if (fl.size() >= 9) {
            fix.sats = std::atoi(fl[6].c_str());
            if (!fl[8].empty()) { fix.altM = std::atof(fl[8].c_str()); fix.hasAlt = true; }
        }
    }
    return true;
}

bool gpsdLine(const std::string& line, ChaseFix& fix, double now) {
    mcp::Json j; std::string err;
    if (!mcp::parseJson(line, j, err) || j.strOr("class") != "TPV") return false;
    const int mode = (int)j.numOr("mode", 0);
    if (mode < 2 || !j.has("lat") || !j.has("lon")) return true;
    const double la = j.numOr("lat", 999), lo = j.numOr("lon", 999);
    if (!(la >= -90 && la <= 90) || !(lo >= -180 && lo <= 180)) return true;
    fix.valid = true; fix.lat = la; fix.lon = lo; fix.steadyT = now;
    if (mode >= 3 && (j.has("altMSL") || j.has("alt"))) { fix.altM = j.has("altMSL") ? j.numOr("altMSL", 0) : j.numOr("alt", 0); fix.hasAlt = true; }
    return true;
}

}  // namespace chase

void ChaseGps::start(Source s, const std::string& where, int port) {
    stop();
    { std::lock_guard<std::mutex> l(mu_); fix_ = ChaseFix(); src_ = s; status_ = s == Off ? "off" : "starting"; }
    if (s == Off || where.empty()) { if (s != Off) { std::lock_guard<std::mutex> l(mu_); status_ = "no device, file or host given"; } return; }
    quit_ = false;
    if (s == Nmea) th_ = namedThread("onair-chasegps", [this, where] { runNmea(where); });
    else th_ = namedThread("onair-chasegps", [this, where, port] { runGpsd(where, port); });
}

void ChaseGps::stop() {
    quit_ = true;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> l(mu_);
    src_ = Off; status_ = "off";
}

bool ChaseGps::fix(ChaseFix& f, double maxAgeS) const {
    std::lock_guard<std::mutex> l(mu_);
    if (!fix_.valid || steadyNowSec() - fix_.steadyT > maxAgeS) return false;
    f = fix_;
    return true;
}

std::string ChaseGps::status() const {
    std::lock_guard<std::mutex> l(mu_);
    if (src_ != Off && fix_.valid && steadyNowSec() - fix_.steadyT > 10) return "no fix for " + std::to_string((int)(steadyNowSec() - fix_.steadyT)) + " s";
    return status_;
}

void ChaseGps::runNmea(std::string path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { std::lock_guard<std::mutex> l(mu_); status_ = "cannot open " + path; return; }
#ifndef _WIN32
    if (isatty(fileno(f))) {   // a serial GPS: 9600 baud 8N1 raw; a read gives up after 0.2 s so the thread can be stopped
        termios t;
        if (tcgetattr(fileno(f), &t) == 0) { cfmakeraw(&t); cfsetspeed(&t, B9600); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 2; tcsetattr(fileno(f), TCSANOW, &t); }
    }
#endif
    { std::lock_guard<std::mutex> l(mu_); status_ = "waiting for a fix (NMEA)"; }
    char line[512];
    while (!quit_.load()) {
        if (!std::fgets(line, sizeof line, f)) { std::clearerr(f); std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        std::lock_guard<std::mutex> l(mu_);
        ChaseFix x = fix_;
        if (chase::nmeaLine(line, x, steadyNowSec()) && x.valid && x.steadyT != fix_.steadyT) { fix_ = x; status_ = "GPS fix (NMEA)"; }
    }
    std::fclose(f);
}

void ChaseGps::runGpsd(std::string host, int port) {
    netInit();
    while (!quit_.load()) {
        std::string err;
        sock_t s = tcpConnect(host, port, 3000, err);
        if (s == kBadSock) {
            { std::lock_guard<std::mutex> l(mu_); status_ = "gpsd: " + (err.empty() ? std::string("cannot connect") : err) + ", retrying"; }
            for (int i = 0; i < 50 && !quit_.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const char* w = "?WATCH={\"enable\":true,\"json\":true}\n";
        sockSendAll(s, w, std::strlen(w));
        sockRecvTimeout(s, 300);
        { std::lock_guard<std::mutex> l(mu_); status_ = "gpsd connected, waiting for a fix"; }
        std::string buf;
        char b[2048];
        while (!quit_.load()) {
            const long long k = sockRecv(s, b, sizeof b);
            if (k == 0) break;
            if (k < 0) {
#ifdef _WIN32
                const int e = WSAGetLastError(); if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
                break;
            }
            buf.append(b, (size_t)k);
            size_t p;
            while ((p = buf.find('\n')) != std::string::npos) {
                const std::string ln = buf.substr(0, p); buf.erase(0, p + 1);
                std::lock_guard<std::mutex> l(mu_);
                ChaseFix x = fix_;
                if (chase::gpsdLine(ln, x, steadyNowSec()) && x.valid && x.steadyT != fix_.steadyT) { fix_ = x; status_ = "GPS fix (gpsd)"; }
            }
            if (buf.size() > 65536) buf.clear();
        }
        sockClose(s);
        { std::lock_guard<std::mutex> l(mu_); if (!quit_.load()) status_ = "gpsd closed the connection, retrying"; }
    }
}

}  // namespace dect2
