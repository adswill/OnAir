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
};

// bits: the CMCE PDU from its first bit (PDU type); false when it is not a D-SDS-DATA or it is cut short
bool parseSdsData(const Bits& bits, SdsMsg& out);
// the LIP short location report alone (76 bits from the PDU type)
bool parseLipShort(const Bits& bits, size_t pos, SdsMsg& out);

// generator (tests, the demo)
Bits buildLipShort(double lat, double lon, int errCode, int speedCode, int dir16, int timeElapsed, int reason);
Bits buildSdsData(int callingIssi, const Bits& userData4, int pid);          // short data type 3 with SDS-TL protocol identifier
Bits buildSdsText(int callingIssi, const std::string& text);                 // PID 0x01, ISO 8859-1

} // namespace dect2::tetra
