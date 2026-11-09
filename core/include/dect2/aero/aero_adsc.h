// ADS-C (FANS-1/A, ARINC 745) downlinks inside ACARS text, as the Aero SU layer delivers it, and builders for the test signal.
// Layers (ARINC 622 ATS application in the ACARS text, as libacars reads it, arinc.c):
//   [/]GGGGGGG.ADS.RRRRRRRHEX...CCCC   ground address (7 or 4 characters), IMI "ADS" (or "DIS"), the aircraft address as 7 characters
//                                     (dots in front), the binary part as hex, then the CRC as 4 hex digits
// CRC: CRC-16 polynomial 0x1021, init 0xFFFF, not reflected, over IMI + address + binary part; the CRC sent is the inverted register,
// high byte first (so the register over everything including the CRC ends at 0x1D0F, the check libacars makes).
// The binary part is a list of groups, each a tag octet then a fixed number of octets (bit layouts and scales: libacars adsc.c,
// checked against the decoded examples in its PROG_GUIDE.md). Lat / lon: 21 bits two's complement, LSB 90/2^19 deg; altitude 16 bits
// two's complement x 4 ft; time stamp 15 bits x 0.125 s past the hour.
#pragma once
#include <utility>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct AeroAdscPoint { double lat = 0, lon = 0; int altFt = 0; int etaSec = -1; };

// One uplink group of a ground system (ARINC 745 layouts as libacars documents them in adsc.c)
struct AeroAdscRequest {
    int kind = 0;                    // 1 cancel all and disconnect, 2 cancel a contract, 6 cancel emergency mode, 7 periodic, 8 event, 9 emergency periodic
    int contract = -1;               // contract number; -1 for kind 1
    int intervalSec = -1;            // periodic: the reporting interval
    std::vector<int> groups;         // groups asked for: 12 flight id, 13 predicted route, 14 earth, 15 air, 16 meteo, 17 airframe id, 21 intent
    double latDevNm = -1;            // event: report when the lateral deviation exceeds this
    bool hasVs = false; int vsFpm = 0;
    bool hasAltRange = false; int ceilingFt = 0, floorFt = 0;
    bool waypoint = false;
};

struct AeroAdscMessage {
    bool found = false;              // the text carries an ARINC 622 ADS-C application
    bool disconnect = false;         // IMI "DIS" (a reason code only)
    bool uplink = false;             // uplink groups: requests below
    bool crcOk = false;
    bool complete = false;           // every group was read to the end of the binary part
    std::string groundAddr, airReg;  // airReg without the leading dots
    std::vector<int> tags;           // groups in the order sent
    // basic report, or an event report with the same layout (tags 7, 9, 10, 18, 19, 20)
    bool hasPos = false;
    int posTag = 0;
    double lat = 0, lon = 0, timeSec = 0;   // timeSec: seconds past the hour
    int altFt = 0;
    int accuracy = 0;                // figure of merit, position accuracy code 0..7 (0 = navigation lost, 7 = < 0.05 nm)
    bool redundancyOk = false, tcasOk = false;
    std::string flightId;            // tag 12
    uint32_t icao = 0;               // tag 17, airframe id
    bool hasRoute = false;           // tag 13
    AeroAdscPoint next, nextNext;
    bool hasEarth = false;           // tag 14
    double trackDeg = 0, groundKt = 0;
    bool trackValid = false;
    int vsFpm = 0;
    bool hasAir = false;             // tag 15
    double headingDeg = 0, mach = 0;
    bool headingValid = false;
    int airVsFpm = 0;
    bool hasMeteo = false;           // tag 16
    double windKt = 0, windDeg = 0, tempC = 0;
    bool windValid = false;
    int reason = -1;                 // DIS reason code
    std::vector<AeroAdscRequest> requests;              // uplink: the contracts asked for or cancelled
    std::vector<int> acks;                              // downlink: contract numbers acknowledged
    std::vector<std::pair<int, int>> nacks;             // downlink: contract number, reason code
};

uint16_t aeroArincCrc(const uint8_t* p, size_t n, uint16_t init = 0xFFFF);   // register after n bytes (CRC-16/CCITT-FALSE without the inversion)
const char* aeroAdscTagName(int tag);                                       // downlink group name, "" when not known

// Reads the downlink groups of a binary part (without its CRC). False when a tag is unknown or a group is cut short;
// what was read before that stays in `out`.
bool aeroParseAdscGroups(const uint8_t* p, size_t n, AeroAdscMessage& out);

// Finds and decodes an ADS-C application in an ACARS message text (label B6, or H1 with the "#M1B/B6 " prefix of the
// sublabel and MFI, or A6 uplinks). False when there is none. `uplink` selects the group table.
bool aeroDecodeAdsc(const std::string& text, bool uplink, AeroAdscMessage& out);

// Several lines in plain words for the message detail pane.
std::string aeroDescribeAdsc(const AeroAdscMessage& m);
bool aeroParseAdscUplink(const uint8_t* p, size_t n, AeroAdscMessage& out);   // false: a group is cut short or not known
std::string aeroAdscNackReason(int code);
std::vector<uint8_t> aeroAdscPeriodicRequest(int contract, int intervalSec, const std::vector<int>& groups);   // for the tests
std::vector<uint8_t> aeroAdscEventRequest(int contract, double latDevNm, int ceilingFt, int floorFt, bool waypoint);
std::vector<uint8_t> aeroAdscAck(int contract);
std::vector<uint8_t> aeroAdscNack(int contract, int reason);

// The state of the ADS-C contracts between each aircraft and each ground system, from all the decoders that carry ADS-C (ACARS, VDL2, HFDL, Aero).
struct AdscContract {
    std::string reg, ground;
    int contract = 0, kind = 0, intervalSec = -1;
    int state = 0;                   // 0 requested, 1 acknowledged, 2 refused, 3 cancelled
    int nackReason = -1;
    std::vector<int> groups;
    double latDevNm = -1; bool hasVs = false; int vsFpm = 0; bool hasAltRange = false; int ceilingFt = 0, floorFt = 0; bool waypoint = false;
    int64_t requested = 0, updated = 0;   // wall clock seconds
};
struct AdscLink { std::string reg, ground; uint32_t reports = 0; int64_t lastReport = 0; };

class AdscContractBook {
public:
    static AdscContractBook& get();
    void note(const AeroAdscMessage& m, int64_t wall);   // CRC-checked messages only
    std::vector<AdscContract> contractsOf(const std::string& reg) const;   // reg with or without its leading dots; dashes are ignored
    std::vector<AdscLink> linksOf(const std::string& reg) const;
    void clear();
    static std::string describe(const AdscContract& c);
private:
    mutable std::mutex mu_;
    std::vector<AdscContract> c_;
    std::vector<AdscLink> l_;
};

// ---- builders (test signal and tests) ----
std::vector<uint8_t> aeroAdscBasicGroup(double lat, double lon, int altFt, double secPastHour, int accuracy = 6, int tag = 7);
std::vector<uint8_t> aeroAdscFlightIdGroup(const std::string& id);          // up to 8 characters A-Z, 0-9, space
std::vector<uint8_t> aeroAdscEarthRefGroup(double trackDeg, double groundKt, int vsFpm);
std::vector<uint8_t> aeroAdscPredictedRouteGroup(const AeroAdscPoint& next, const AeroAdscPoint& nextNext);
std::vector<uint8_t> aeroAdscMeteoGroup(double windKt, double windDeg, double tempC);
// "/GGGGGGG.ADS.RRRRRRR" + hex of the groups + CRC
std::string aeroBuildAdscText(const std::string& groundAddr, const std::string& registration, const std::vector<uint8_t>& groups);

} // namespace dect2
