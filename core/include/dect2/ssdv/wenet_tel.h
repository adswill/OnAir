// Wenet (Project Horus) non-picture packets: GPS and system telemetry, orientation, and text messages on the same link as the SSDV picture. Field layouts as in the
// Wenet receiver (WenetPackets.py), which is licensed GPLv3 or later; these parsers are written from that layout.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct WenetGps {
    bool valid = false;
    int week = 0; double iTow = 0; int leap = 0;
    double lat = 0, lon = 0, alt = 0, speed = 0, heading = 0, ascent = 0;
    int numSv = 0, fix = 0, dynModel = 0;
    bool haveSys = false; double radioTemp = 0, cpuTemp = 0; int cpuSpeed = 0;
    bool havePower = false; double battV = 0; int battI = 0; double auxTemp = 0;
    double utc() const { return week * 604800.0 + iTow - leap + 315964800.0; }
};
struct WenetOrient {
    bool valid = false;
    int sysStatus = 0, sysError = 0, sysCal = 0, gyroCal = 0, accelCal = 0, magCal = 0, temp = 0;
    double heading = 0, roll = 0, pitch = 0;
    double qx = 0, qy = 0, qz = 0, qw = 0;
};
struct WenetText { uint16_t id = 0; std::string text; };

// each takes the raw packet (256 bytes for Wenet, padded with 0x55) and fills its struct; false: too short or not that type
bool wenetParseGps(const uint8_t* pkt, int n, WenetGps& out);
bool wenetParseOrient(const uint8_t* pkt, int n, WenetOrient& out);
bool wenetParseText(const uint8_t* pkt, int n, WenetText& out);

}  // namespace dect2
