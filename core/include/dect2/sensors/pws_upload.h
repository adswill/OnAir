// Personal weather station upload from a Sub-GHz weather sensor (WeeWX, Cumulus): Weather Underground (the PWS upload protocol) and CWOP
// (an APRS complete weather report, APRS101 chapter 12, over APRS-IS). Off by default; the user's station id and key. docs/PWS-UPLOAD-3.9.md.
// Windy and AWEKAS are not included: their upload interfaces could not be checked here.
#pragma once
#include "dect2/sensors/sensors_tel.h"
#include <cstdint>
#include <string>

namespace dect2 {

struct PwsReading {
    bool hasTemp = false, hasHum = false, hasPress = false, hasWind = false, hasGust = false, hasDir = false;
    bool hasRain1h = false, hasRain24h = false, hasRainDay = false;
    double tempC = 0, hum = 0, pressHpa = 0, windKmh = 0, gustKmh = 0, dirDeg = 0, rain1hMm = 0, rain24hMm = 0, rainDayMm = 0;
    bool any() const { return hasTemp || hasHum || hasPress || hasWind || hasRain1h; }
};

// from a sensor's latest values and its rain counter history (rain_mm / rain_in): last hour, last 24 h and since local midnight (tzOffsetSec)
PwsReading pwsFromSensor(const SensorDevice& d, int64_t nowUtc, int tzOffsetSec);
// several sensors make one station (an outdoor thermometer and a rain gauge): a value of b fills a missing one of a
PwsReading pwsMerge(const PwsReading& a, const PwsReading& b);

// https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php?ID=..&PASSWORD=..&dateutc=..&tempf=.. (imperial units)
std::string pwsWundergroundUrl(const std::string& stationId, const std::string& key, const PwsReading& r, int64_t utc);
// "CW1234>APRS,TCPIP*:@101523z5441.40N/02517.00E_220/004g005t050r000p000P000h50b10132OnAir"
std::string pwsCwopPacket(const std::string& call, double lat, double lon, const PwsReading& r, int64_t utc);
// sends one packet to CWOP over APRS-IS (cwop.aprs.net:14580, passcode -1 for CW stations, else the user's); false + err
bool pwsCwopSend(const std::string& call, const std::string& passcode, const std::string& packet, std::string& err, const std::string& host = "cwop.aprs.net", int port = 14580);

}  // namespace dect2
