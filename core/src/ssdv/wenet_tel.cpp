#include "dect2/ssdv/wenet_tel.h"
#include <cstring>

namespace dect2 {

namespace {
inline uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
inline uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline float f32(const uint8_t* p) { uint32_t v = u32(p); float f; memcpy(&f, &v, 4); return f; }
bool all55(const uint8_t* p, int n) { for (int i = 0; i < n; i++) if (p[i] != 0x55) return false; return true; }
}

bool wenetParseGps(const uint8_t* p, int n, WenetGps& o) {
    if (n < 81 || p[0] != 0x01) return false;
    o = WenetGps();
    o.week = u16(p + 1); o.iTow = u32(p + 3) / 1000.0; o.leap = p[7];
    o.lat = f32(p + 8); o.lon = f32(p + 12); o.alt = f32(p + 16); o.speed = f32(p + 20); o.heading = f32(p + 24); o.ascent = f32(p + 28);
    o.numSv = p[32]; o.fix = p[33]; o.dynModel = p[34];
    if (!all55(p + 35, 38)) { o.haveSys = true; o.radioTemp = f32(p + 35); o.cpuTemp = f32(p + 39); o.cpuSpeed = u16(p + 43); }
    if (!all55(p + 73, 8)) { o.havePower = true; o.battV = u16(p + 73) / 1000.0; o.battI = u16(p + 75); o.auxTemp = f32(p + 77); }
    o.valid = o.lat >= -90 && o.lat <= 90 && o.lon >= -180 && o.lon <= 180;
    return o.valid;
}

bool wenetParseOrient(const uint8_t* p, int n, WenetOrient& o) {
    if (n < 43 || p[0] != 0x02) return false;
    o = WenetOrient();
    o.sysStatus = p[8]; o.sysError = p[9]; o.sysCal = p[10]; o.gyroCal = p[11]; o.accelCal = p[12]; o.magCal = p[13]; o.temp = (int8_t)p[14];
    o.heading = f32(p + 15); o.roll = f32(p + 19); o.pitch = f32(p + 23); o.qx = f32(p + 27); o.qy = f32(p + 31); o.qz = f32(p + 35); o.qw = f32(p + 39);
    o.valid = true;
    return true;
}

bool wenetParseText(const uint8_t* p, int n, WenetText& o) {
    if (n < 4 || p[0] != 0x00) return false;
    const int len = p[1];
    if (4 + len > n) return false;
    o.id = u16(p + 2);
    o.text.assign((const char*)p + 4, (size_t)len);
    return true;
}

}  // namespace dect2
