// PWS upload (round 4 #19): readings from sensor values in metric and imperial, rain from the counter history (last hour, 24 h, since midnight,
// a counter reset), merging two sensors, the Weather Underground address, and the CWOP packet against the APRS101 layout (wind, temperature
// below 0, humidity 100 %, rain, pressure, position rounding, missing values), and the upload to a local fake APRS-IS server.
#include "dect2/sensors/pws_upload.h"
#include "../../core/src/net/tcp_client.h"
#include "../common/test_util.h"
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
using namespace dect2;

static SensorField nf(const char* k, double v) { SensorField f; f.key = k; f.num = v; f.numeric = true; f.text = std::to_string(v); return f; }

int main() {
    const int64_t now = 1'800'000'000;   // 2027-01-15 08:00 UTC
    SensorDevice d;
    d.latest = {nf("temperature_C", -3.5), nf("humidity", 100), nf("wind_avg_m_s", 2.0), nf("wind_max_m_s", 5.0), nf("wind_dir_deg", 0)};
    SensorSeries rain; rain.key = "rain_mm";
    // 2.0 yesterday, 10.0 at 00:30 UTC, 11.0 at 07:10, a reset to 0.2 at 07:20 (new battery), 0.8 at 07:50: 1 h = 1.0 + 0.6 (the rise from the last value before the hour counts)
    rain.pts = {{now - 86400 - 600, 2.0}, {now - 7 * 3600 - 1800, 10.0}, {now - 3000, 11.0}, {now - 2400, 0.2}, {now - 600, 0.8}};
    d.history = {rain};
    const PwsReading r = pwsFromSensor(d, now, 0);
    CHECK(r.hasTemp && r.tempC == -3.5 && r.hasHum && r.hum == 100 && r.hasWind && std::fabs(r.windKmh - 7.2) < 1e-9 && std::fabs(r.gustKmh - 18) < 1e-9 && r.hasDir && r.dirDeg == 0 && !r.hasPress, "values");
    CHECK(r.hasRain1h && std::fabs(r.rain1hMm - 1.6) < 1e-9, "rain last hour across the reset: %.2f", r.rain1hMm);
    CHECK(r.hasRainDay && std::fabs(r.rainDayMm - 9.6) < 1e-9 && r.hasRain24h && std::fabs(r.rain24hMm - 9.6) < 1e-9, "since midnight %.2f, 24 h %.2f", r.rainDayMm, r.rain24hMm);
    SensorDevice baro; baro.latest = {nf("pressure_hPa", 1013.2), nf("temperature_C", 21)};
    const PwsReading m = pwsMerge(r, pwsFromSensor(baro, now, 0));
    CHECK(m.hasPress && m.pressHpa == 1013.2 && m.tempC == -3.5, "merge fills only what is missing");
    SensorDevice imp; imp.latest = {nf("temperature_F", 50), nf("wind_avg_mi_h", 10)};
    const PwsReading ri = pwsFromSensor(imp, now, 0);
    CHECK(std::fabs(ri.tempC - 10) < 1e-9 && std::fabs(ri.windKmh - 16.09344) < 1e-9, "imperial sensors");

    const std::string u = pwsWundergroundUrl("IKAUNA12", "a b&c", m, now);
    CHECK(u.rfind("https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php?ID=IKAUNA12&PASSWORD=a%20b%26c&dateutc=2027-01-15%2008%3A00%3A00", 0) == 0 &&
          u.find("&tempf=25.7") != std::string::npos && u.find("&humidity=100") != std::string::npos && u.find("&baromin=29.920") != std::string::npos &&
          u.find("&windspeedmph=4.5") != std::string::npos && u.find("&windgustmph=11.2") != std::string::npos && u.find("&winddir=0") != std::string::npos &&
          u.find("&rainin=0.06") != std::string::npos && u.find("&dailyrainin=0.38") != std::string::npos && u.find("&action=updateraw") != std::string::npos, "WU address: %s", u.c_str());

    const std::string p = pwsCwopPacket("CW1234", 54.69, -25.2799999, m, now);
    CHECK(p == "CW1234>APRS,TCPIP*:@150800z5441.40N/02516.80W_360/004g011t026r006p038P038h00b10132OnAir", "CWOP packet: %s", p.c_str());
    PwsReading cold; cold.hasTemp = true; cold.tempC = -30;
    const std::string pc = pwsCwopPacket("CW1", 59.99999, 10.0, cold, now);
    CHECK(pc == "CW1>APRS,TCPIP*:@150800z6000.00N/01000.00E_.../...g...t-22OnAir", "missing values and rounding: %s", pc.c_str());

    // upload to a fake APRS-IS server on the loopback
    netInit();
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    CHECK(bind(ls, (sockaddr*)&a, sizeof a) == 0 && listen(ls, 1) == 0, "listen");
    socklen_t l = sizeof a; getsockname(ls, (sockaddr*)&a, &l);
    std::string got;
    std::thread th([&] {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c == kBadSock) return;
        const char* banner = "# aprsc fake\r\n";
        sockSendAll(c, banner, strlen(banner));
        sockRecvTimeout(c, 3000);
        char b[1024];
        for (int i = 0; i < 20 && got.find("OnAir\r\n", got.find("vers") + 1) == std::string::npos; i++) {
            const long long n = sockRecv(c, b, sizeof b);
            if (n <= 0) break;
            got.append(b, (size_t)n);
            if (got.find("vers OnAir") != std::string::npos && got.find("\r\n") != std::string::npos && got.find("logresp") == std::string::npos) { const char* ok = "# logresp CW1234 unverified\r\n"; sockSendAll(c, ok, strlen(ok)); }
        }
        sockClose(c);
    });
    std::string err;
    const bool sent = pwsCwopSend("CW1234", "", p, err, "127.0.0.1", ntohs(a.sin_port));
    th.join();
    sockClose(ls);
    CHECK(sent && got.find("user CW1234 pass -1 vers OnAir") == 0 && got.find(p + "\r\n") != std::string::npos, "sent: %s", err.c_str());
    return testSummary("sensors_pws");
}
