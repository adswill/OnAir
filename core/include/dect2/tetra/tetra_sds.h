// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA SDS interpreter: the clear D-SDS-DATA CMCE PDU (EN 300 392-2 clause 14.8), its SDS-TL protocol identifier and the two payloads that matter on
// the air: simple text messages and the Location Information Protocol short location report (ETSI TS 100 392-18-1) as a position for the map and the
// call log. Unencrypted PDUs only: the caller never passes an air interface encrypted block. Bit layouts are written from the standards and checked
// by round trip with the generator here; they still need a real capture (docs/SMART-voice-3.7.md). Pure functions, no state.
#pragma once
#include "tetra_pdu.h"
#include <string>

namespace dect2::tetra {

struct SdsMsg {
    bool valid = false;
    int callingIssi = -1;           // -1 not given
    int dataType = -1;              // short data type identifier 0..3
    int pid = -1;                   // SDS-TL protocol identifier, -1 none (type 1 to 3 without a user data 4)
    std::string kind;               // "location", "text", "status / data"
    std::string text;               // the text of a text message, or a one-line summary of the payload
    bool hasPos = false;
    double lat = 0, lon = 0;        // degrees
    int posErrorM = -1;             // upper bound of the position error in metres, -1 unknown
    double speedKmh = -1;           // -1 unknown
    int headingDeg = -1;            // -1 unknown
    int timeElapsed = -1;           // LIP: 0 now, 1 < 5 s, 2 < 5 min, 3 older / unknown
    int reason = -1;                // reason for sending (LIP)
    // Location Information Protocol, long report (TS 100 392-18-1 clause 6.2.2) and more of the short one
    int lipPdu = -1;                // 0 short location report, 1 long location report, -1 not a LIP position
    int shape = -1;                 // long report: location shape 0..10 (table 6.79)
    bool hasAlt = false;
    double altM = 0;                // metres (WGS84 ellipsoid, or the user defined reference when altRef is 1)
    int altRef = 0;
    double horizUncM = -1;          // horizontal uncertainty in metres (circle radius, ellipse half major axis, arc outer radius), -1 unknown
    int confidencePct = -1;         // confidence level of an ellipse or arc, percent, -1 not known
    bool hasVert = false;
    double vertKmh = 0;             // + up, - down
    bool hasTimePos = false;        // long report with a time of position: day of month, hour, minute, second (UTC)
    int tpDay = 0, tpHour = 0, tpMin = 0, tpSec = 0;
    int terminalIssi = -1;          // long report: type 5 element "terminal or location identification" as an SSI
    int msgRef = -1, resultCode = -1, statusValue = -1;   // long report type 5 elements, -1 not present
    int ackRequested = 0;
    std::string source;             // one line: which PDU, shape and reason for sending, for the map card
};

// bits: the CMCE PDU from its first bit (PDU type); false when it is not a D-SDS-DATA or it is cut short
bool parseSdsData(const Bits& bits, SdsMsg& out);
// D-STATUS (table 14.14): calling party and the 16-bit pre-coded status. False when not a D-STATUS or cut short.
bool parseStatus(const Bits& bits, SdsMsg& out);
// the LIP short location report alone (76 bits from the PDU type)
bool parseLipShort(const Bits& bits, size_t pos, SdsMsg& out);
// the LIP long location report (PDU type 1, extension 3), from its first bit up to `end` (exclusive); false when it is another PDU, cut short, or has a
// location shape / velocity type that is reserved. A report without a location (shape 0) gives true and hasPos false (status, result code).
bool parseLipLong(const Bits& bits, size_t pos, size_t end, SdsMsg& out);
// either of the two, chosen by the PDU type bits
bool parseLip(const Bits& bits, size_t pos, size_t end, SdsMsg& out);
const char* lipReasonText(int reason);                  // table 6.94, "" for a reserved value
double lipHorizUncM(int code6);                         // table 6.49: the "less than" bound in metres, -1 best effort
double lipAltitudeM(int value11);                       // table 6.62 (11-bit value, 0 reserved gives -1e9)

// generator (tests, the demo)
Bits buildLipShort(double lat, double lon, int errCode, int speedCode, int dir16, int timeElapsed, int reason);
// a long report: every field is the raw coded value of the table named; type5 is a list of (identifier, data bits) elements appended after the fixed part
struct LipType5 { int id; Bits data; };
struct LipLongSpec {
    int timeType = 1;                       // 0 none, 1 elapsed, 2 time of position
    int timeElapsed = 0;                    // 2 bits
    int day = 1, hour = 0, minute = 0, second = 0;
    int shape = 1;                          // table 6.79
    double lat = 0, lon = 0;
    int hpu = 0;                            // 6 bits: circle radius / ellipse half major axis
    int minorAxis = 0, angle = 0, confidence = 0;
    int altRef = 0, altValue = 0;           // 1 + 11 bits
    int altUnc = 0;                         // 3 bits (shape 7, 8)
    int posErr = 0;                         // shape 10, 3 bits
    int inner = 0, outer = 0, startAngle = 0, stopAngle = 0;   // arc
    int velType = 0;                        // table 6.123
    int hv = 0, hvUnc = 0, vvSign = 0, vv = 0, vvUnc = 0, dirExt = 0, dirUnc = 0;
    int ack = 0;
    int addType = 0;                        // 0 reason for sending, 1 user data
    int addData = 0;                        // 8 bits
    std::vector<LipType5> type5;
};
Bits buildLipLong(const LipLongSpec& s);
Bits buildSdsData(int callingIssi, const Bits& userData4, int pid);          // short data type 3 with SDS-TL protocol identifier
Bits buildSdsText(int callingIssi, const std::string& text);                 // PID 0x02 (table 29.21), ISO 8859-1

} // namespace dect2::tetra
