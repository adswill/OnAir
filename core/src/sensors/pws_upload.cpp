// PWS upload: see pws_upload.h.
#include "dect2/sensors/pws_upload.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {

namespace {
const SensorField* fld(const SensorDevice& d, const char* k) { for (const auto& f : d.latest) if (f.numeric && f.key == k) return &f; return nullptr; }
const SensorSeries* ser(const SensorDevice& d, const char* k) { for (const auto& s : d.history) if (s.key == k) return &s; return nullptr; }
// the counter rise since `from` (resets of the counter, a new battery, are skipped)
bool rise(const SensorSeries& s, int64_t from, double& out) {
    double sum = 0, prev = NAN;
    bool any = false;
    for (const auto& p : s.pts) {
        if (p.utc < from) { prev = p.v; continue; }
        if (!std::isnan(prev) && p.v >= prev) sum += p.v - prev;
        prev = p.v;
        any = true;
    }
    if (!any) return false;
    out = sum;
    return true;
}
std::string num(double v, int dec) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dec, v); return b; }
std::string urlEnc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) { if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c; else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; } }
    return o;
}
}  // namespace

PwsReading pwsFromSensor(const SensorDevice& d, int64_t now, int tz) {
    PwsReading r;
    if (auto* f = fld(d, "temperature_C")) { r.hasTemp = true; r.tempC = f->num; }
    else if (auto* f2 = fld(d, "temperature_F")) { r.hasTemp = true; r.tempC = (f2->num - 32) / 1.8; }
    if (auto* f = fld(d, "humidity")) if (f->num >= 0 && f->num <= 100) { r.hasHum = true; r.hum = f->num; }
    if (auto* f = fld(d, "pressure_hPa")) { r.hasPress = true; r.pressHpa = f->num; }
    if (auto* f = fld(d, "wind_avg_km_h")) { r.hasWind = true; r.windKmh = f->num; }
    else if (auto* f2 = fld(d, "wind_avg_m_s")) { r.hasWind = true; r.windKmh = f2->num * 3.6; }
    else if (auto* f3 = fld(d, "wind_avg_mi_h")) { r.hasWind = true; r.windKmh = f3->num * 1.609344; }
    if (auto* f = fld(d, "wind_max_km_h")) { r.hasGust = true; r.gustKmh = f->num; }
    else if (auto* f2 = fld(d, "wind_max_m_s")) { r.hasGust = true; r.gustKmh = f2->num * 3.6; }
    else if (auto* f3 = fld(d, "wind_max_mi_h")) { r.hasGust = true; r.gustKmh = f3->num * 1.609344; }
    if (auto* f = fld(d, "wind_dir_deg")) { r.hasDir = true; r.dirDeg = std::fmod(std::fmod(f->num, 360) + 360, 360); }
    const SensorSeries* rs = ser(d, "rain_mm");
    double scale = 1;
    if (!rs) { rs = ser(d, "rain_in"); scale = 25.4; }
    if (rs && !rs->pts.empty()) {
        double v;
        if (rise(*rs, now - 3600, v)) { r.hasRain1h = true; r.rain1hMm = v * scale; }
        if (rise(*rs, now - 86400, v)) { r.hasRain24h = true; r.rain24hMm = v * scale; }
        const int64_t midnight = (now + tz) / 86400 * 86400 - tz;
        if (rise(*rs, midnight, v)) { r.hasRainDay = true; r.rainDayMm = v * scale; }
    }
    return r;
}

PwsReading pwsMerge(const PwsReading& a, const PwsReading& b) {
    PwsReading r = a;
    if (!r.hasTemp && b.hasTemp) { r.hasTemp = true; r.tempC = b.tempC; }
    if (!r.hasHum && b.hasHum) { r.hasHum = true; r.hum = b.hum; }
    if (!r.hasPress && b.hasPress) { r.hasPress = true; r.pressHpa = b.pressHpa; }
    if (!r.hasWind && b.hasWind) { r.hasWind = true; r.windKmh = b.windKmh; }
    if (!r.hasGust && b.hasGust) { r.hasGust = true; r.gustKmh = b.gustKmh; }
    if (!r.hasDir && b.hasDir) { r.hasDir = true; r.dirDeg = b.dirDeg; }
    if (!r.hasRain1h && b.hasRain1h) { r.hasRain1h = true; r.rain1hMm = b.rain1hMm; }
    if (!r.hasRain24h && b.hasRain24h) { r.hasRain24h = true; r.rain24hMm = b.rain24hMm; }
    if (!r.hasRainDay && b.hasRainDay) { r.hasRainDay = true; r.rainDayMm = b.rainDayMm; }
    return r;
}

std::string pwsWundergroundUrl(const std::string& id, const std::string& key, const PwsReading& r, int64_t utc) {
    const time_t t = (time_t)utc;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &g);
    std::string u = "https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php?ID=" + urlEnc(id) + "&PASSWORD=" + urlEnc(key) + "&dateutc=" + urlEnc(ts);
    if (r.hasTemp) u += "&tempf=" + num(r.tempC * 1.8 + 32, 1);
    if (r.hasHum) u += "&humidity=" + num(r.hum, 0);
    if (r.hasPress) u += "&baromin=" + num(r.pressHpa * 0.0295299830714, 3);
    if (r.hasWind) u += "&windspeedmph=" + num(r.windKmh / 1.609344, 1);
    if (r.hasGust) u += "&windgustmph=" + num(r.gustKmh / 1.609344, 1);
    if (r.hasDir) u += "&winddir=" + num(r.dirDeg, 0);
    if (r.hasRain1h) u += "&rainin=" + num(r.rain1hMm / 25.4, 2);
    if (r.hasRainDay) u += "&dailyrainin=" + num(r.rainDayMm / 25.4, 2);
    u += "&softwaretype=OnAir&action=updateraw";
    return u;
}

std::string pwsCwopPacket(const std::string& call, double lat, double lon, const PwsReading& r, int64_t utc) {
    const time_t t = (time_t)utc;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char ts[16];
    std::snprintf(ts, sizeof ts, "%02d%02d%02dz", g.tm_mday, g.tm_hour, g.tm_min);
    // degrees and minutes to 1/100 minute, rounded once (59.996' becomes the next degree)
    const long long lh = std::llround(std::fabs(lat) * 6000), oh = std::llround(std::fabs(lon) * 6000);
    char pos[40];
    std::snprintf(pos, sizeof pos, "%02lld%02lld.%02lld%c/%03lld%02lld.%02lld%c", lh / 6000, lh % 6000 / 100, lh % 100, lat >= 0 ? 'N' : 'S', oh / 6000, oh % 6000 / 100, oh % 100, lon >= 0 ? 'E' : 'W');
    auto f3 = [](bool has, double v) { char b[8]; if (!has) return std::string("..."); std::snprintf(b, sizeof b, "%03d", (int)std::lround(std::max(0.0, std::min(999.0, v)))); return std::string(b); };
    std::string w = "_" + f3(r.hasDir, r.dirDeg == 0 && r.hasDir ? 360 : r.dirDeg) + "/" + f3(r.hasWind, r.windKmh / 1.609344) + "g" + f3(r.hasGust, r.gustKmh / 1.609344);
    if (r.hasTemp) {
        const int tf = (int)std::lround(r.tempC * 1.8 + 32);
        char b[8];
        if (tf < 0) std::snprintf(b, sizeof b, "-%02d", std::min(99, -tf)); else std::snprintf(b, sizeof b, "%03d", std::min(999, tf));
        w += std::string("t") + b;
    } else w += "t...";
    char b[16];
    if (r.hasRain1h) { std::snprintf(b, sizeof b, "r%03d", (int)std::lround(std::min(999.0, r.rain1hMm / 0.254))); w += b; }
    if (r.hasRain24h) { std::snprintf(b, sizeof b, "p%03d", (int)std::lround(std::min(999.0, r.rain24hMm / 0.254))); w += b; }
    if (r.hasRainDay) { std::snprintf(b, sizeof b, "P%03d", (int)std::lround(std::min(999.0, r.rainDayMm / 0.254))); w += b; }
    if (r.hasHum) { const int h = (int)std::lround(r.hum); std::snprintf(b, sizeof b, "h%02d", h >= 100 ? 0 : std::max(1, h)); w += b; }
    if (r.hasPress) { std::snprintf(b, sizeof b, "b%05d", (int)std::lround(std::max(0.0, std::min(99999.0, r.pressHpa * 10)))); w += b; }
    return call + ">APRS,TCPIP*:@" + ts + pos + w + "OnAir";
}

bool pwsCwopSend(const std::string& call, const std::string& pass, const std::string& packet, std::string& err, const std::string& host, int port) {
    netInit();
    sock_t s = tcpConnect(host, port, 8000, err);
    if (s == kBadSock) return false;
    sockRecvTimeout(s, 5000);
    char buf[512];
    sockRecv(s, buf, sizeof buf);   // the server's banner
    const std::string login = "user " + call + " pass " + (pass.empty() ? "-1" : pass) + " vers OnAir 3.9\r\n";
    bool ok = sockSendAll(s, login.data(), login.size());
    if (ok) { sockRecv(s, buf, sizeof buf); const std::string line = packet + "\r\n"; ok = sockSendAll(s, line.data(), line.size()); }
    if (!ok) err = "could not send to " + host;
    sockClose(s);
    return ok;
}

}  // namespace dect2
