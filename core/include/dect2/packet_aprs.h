// APRS (Automatic Packet Reporting System) information field parser, after the APRS 1.0.1 specification (APRS101.pdf) and the 1.2 addenda:
// uncompressed and compressed positions, Mic-E, objects, items, status, messages, weather, telemetry.
#pragma once
#include <cmath>
#include <string>

namespace dect2 {
namespace aprs {

struct Weather {
    double windDirDeg = NAN, windMph = NAN, gustMph = NAN, tempF = NAN;
    double rain1hIn = NAN, rain24hIn = NAN, rainMidnightIn = NAN;     // inches
    double humidityPct = NAN, pressureMbar = NAN;
    bool any() const { return !std::isnan(windDirDeg) || !std::isnan(windMph) || !std::isnan(gustMph) || !std::isnan(tempF) || !std::isnan(rain1hIn) ||
                              !std::isnan(rain24hIn) || !std::isnan(rainMidnightIn) || !std::isnan(humidityPct) || !std::isnan(pressureMbar); }
};

struct Info {
    std::string type;            // Position, Mic-E, Object, Item, Status, Message, Weather, Telemetry, Raw GPS, Capabilities, Query, Third-party, Other
    bool hasPos = false;
    double lat = 0, lon = 0;     // degrees, north and east positive
    char symTable = 0, symCode = 0;
    bool hasAlt = false;  double altM = 0;
    bool hasCourse = false; int courseDeg = 0;
    bool hasSpeed = false;  double speedKnots = 0;
    std::string name;            // object or item name; the addressee of a message
    bool live = true;            // objects and items: false when killed
    std::string comment;         // what follows the position
    std::string text;            // message or status text
    std::string msgNo;           // message number after the '{'
    bool isAck = false, isRej = false;
    std::string micEStatus;      // Mic-E message code: "En Route", "Emergency", ...
    Weather wx;
    bool hasWx = false;
    std::string summary;         // one line for the table
};

// destCall: the destination address of the frame (Mic-E carries the latitude in it); info: the information field.
// False when the field is empty or the type byte is not understood at all.
bool parse(const std::string& destCall, const std::string& info, Info& out);

// Latitude / longitude as "25.2000N" / "55.3000E"
std::string fmtLat(double lat);
std::string fmtLon(double lon);

} // namespace aprs
} // namespace dect2
