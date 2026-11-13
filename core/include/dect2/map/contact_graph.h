// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Who calls whom: the contact lines of the maps for QSO-style modes (FT8, FT4, FT2, JT65, JT9, Q65, FST4, MSK144, JS8Call, WSPR).
// parseContactMessage() reads one decoded message in the standard formats ("CQ DX K1ABC FN42", "K1ABC W9XYZ -12", "W9XYZ K1ABC RR73");
// ContactGraph keeps the stations (position from the last locator they sent, else the middle of their callsign prefix, marked estimated)
// and the contacts between them (a line per pair of stations, with times, and whether the QSO was completed), and drops contacts that
// have gone quiet. Pure code, no state outside the object; times are UTC seconds supplied by the caller.
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct ContactMsg {
    bool valid = false;        // the message names a station
    bool cq = false;          // CQ / QRZ: a station calling anybody
    std::string cqTag;        // "DX", "POTA", "NA" ... ("" none)
    std::string from, to;     // the sender and the station called ("" when hashed "<...>" or a CQ)
    std::string grid;         // the sender's 4-character locator when the message carries one
    std::string report;       // "-12", "R-05" ("" none)
    bool complete = false;    // RRR, RR73 or 73
    bool final73 = false;     // 73 or RR73 (the last word of a contact)
};
// A decoded message (WSJT-X text). valid=false for free text, telemetry and messages without a callsign.
ContactMsg parseContactMessage(const std::string& msg);

// The middle of the country a callsign belongs to (longest matching prefix of a built-in table of about a hundred prefixes); false: unknown.
bool callsignPrefixPosition(const std::string& call, double& lat, double& lon);

// The great circle from one point to another as a polyline, both ends included. A point about every `stepDeg` degrees of arc (at least 2
// points, at most 400). Longitudes are continuous (they may pass 180 or -180) so the line can be drawn without a jump.
std::vector<std::pair<double, double>> greatCircleArc(double lat1, double lon1, double lat2, double lon2, double stepDeg = 3.0);

struct ContactStation {
    std::string call;
    double lat = 0, lon = 0;
    bool known = false;       // lat / lon valid
    bool estimated = false;   // from the callsign prefix, not from a locator
    std::string grid;         // the last locator it sent
    double lastSeen = 0;
};

struct ContactEdge {
    std::string from, to;     // the newest message: from called to
    double first = 0, last = 0;
    int count = 0;            // messages between the two
    bool complete = false;    // a 73 / RR73 / RRR was seen
    int mode = 0;             // of the newest message
    double dialHz = 0;        // of the newest message (colour by band); 0 not known
    std::string lastMsg;
};

class ContactGraph {
public:
    explicit ContactGraph(double expirySec = 900) : expiry_(expirySec) {}
    void setExpiry(double sec) { expiry_ = sec; }
    double expiry() const { return expiry_; }
    // One decoded message heard at `t`. Returns false when it names no station. `senderGrid` (or empty) is a locator the decoder already
    // found for the sender; the message's own locator wins.
    bool add(double t, const std::string& msg, int mode = 0, double dialHz = 0, const std::string& senderGrid = std::string());
    // Drop contacts whose last message is older than the expiry before `now`, and stations nobody talks to any more and not heard for as long.
    void expire(double now);
    void clear() { stations_.clear(); edges_.clear(); }
    std::vector<ContactStation> stations() const;
    std::vector<ContactEdge> edges() const;   // oldest first
    bool station(const std::string& call, ContactStation& out) const;
    size_t edgeCount() const { return edges_.size(); }

private:
    ContactStation& touch(const std::string& call, double t);
    double expiry_;
    std::map<std::string, ContactStation> stations_;
    std::map<std::pair<std::string, std::string>, ContactEdge> edges_;   // key: the two calls in order
};

// The callsign without a portable suffix or a country prefix of a compound call ("PJ4/K1ABC/P" -> "K1ABC"): the call to show a contact under.
std::string baseCallsign(const std::string& call);

}
