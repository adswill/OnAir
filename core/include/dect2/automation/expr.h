// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The condition language of the automations: a small expression evaluator written for this purpose (no code from outside is ever run).
//   operators  or and not  == != < <= > >=  contains startswith endswith matches in  + - * / %  ( )
//   literals   12  3.5  20km  118.1MHz  'text' "text"  true false null  [1, 2, 3]
//   units      Hz kHz MHz GHz (to Hz), ms s min h (to seconds), km nm mi (to km), ft kt pass through
//   fields     any name of the event (type, mode, freq, freq_mhz, time and the event's own fields); a missing field is null and compares false
//   functions  distance_km(lat, lon) / distance_km(lat1, lon1, lat2, lon2), bearing_deg(lat, lon), within(lat, lon, clat, clon, radius_km),
//              inside(lat, lon, "lat,lon; lat,lon; ..."), hour() minute() weekday(), lower upper len abs min max round, str num, has("field")
// Limits: 2000 characters, nesting 40, 500 nodes, 5000 evaluation steps, text results 4096 characters. A fault makes the condition false.
#pragma once
#include "dect2/automation/event.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2::automation {

struct Env {
    const Event* ev = nullptr;
    double now = 0;              // Unix seconds (hour(), minute() and weekday() use it)
    int tzOffsetSec = 0;         // local time minus UTC
    bool stationSet = false;     // the receiver position (distance_km with two arguments, bearing_deg)
    double stationLat = 0, stationLon = 0;
};

struct ExprError {
    int pos = 0;                 // 0-based character position in the expression text
    std::string msg;
    std::string text() const { return msg + " at character " + std::to_string(pos + 1); }
};

class Expr {
public:
    // null on a syntax error (err filled). Empty text is a valid expression that is always true.
    static std::shared_ptr<const Expr> parse(const std::string& text, ExprError& err);
    // The value; on a run-time fault (division by zero, wrong argument types, too many steps) a Null value and err filled.
    Value eval(const Env& env, std::string* err = nullptr) const;
    // eval() as a yes/no; a fault is no
    bool test(const Env& env, std::string* err = nullptr) const;
    const std::string& source() const { return src_; }
    struct Node;
private:
    std::string src_;
    std::shared_ptr<const Node> root_;
};

// ---- geometry, shared with the rules (geofence) and the scripts ----
namespace geo {
double distanceKm(double lat1, double lon1, double lat2, double lon2);   // great circle, mean Earth radius 6371.0088 km
double bearingDeg(double lat1, double lon1, double lat2, double lon2);   // initial bearing 0 - 360
// polygon text "lat,lon; lat,lon; ..." (3 to 64 points); false on a bad text
bool parsePolygon(const std::string& text, std::vector<std::pair<double, double>>& pts);
bool pointInPolygon(double lat, double lon, const std::vector<std::pair<double, double>>& pts);   // ray casting; points on the edge count as inside only by chance
}  // namespace geo

// glob match: * any run, ? one character, case-insensitive, linear-ish (no backtracking blow-up)
bool globMatch(const std::string& pattern, const std::string& text);

}  // namespace dect2::automation
