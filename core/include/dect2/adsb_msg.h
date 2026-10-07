// Mode S / ADS-B message layer: CRC-24, field extraction, CPR position, altitude and identity codes, message decoding.
// No signal processing here, so every function can be checked against published examples.
// Sources: ICAO Annex 10 Vol. IV (message formats, CRC), RTCA DO-260B (extended squitter formats) and "The 1090 Megahertz Riddle"
// (Junzi Sun, TU Delft OPEN Publishing, https://mode-s.org/1090mhz) for the worked examples. Each table says where it comes from.
// Bit numbers in comments are the standard's: bit 1 is the first (most significant) bit of the message, the ME field is bits 33-88.
#pragma once
#include <cstdint>
#include <string>

namespace dect2::adsb {

constexpr uint32_t kCrcGenerator = 0xFFF409;   // G(x) = x^24 + x^23 + ... + x^10 + x^3 + 1 without the x^24 term (Annex 10 Vol. IV 3.1.2.3.3.1)

// Remainder of the whole message (all nbits, parity included; nbits is a multiple of 8).
// DF11 / DF17 / DF18: 0 for a good message (DF11: the interrogator code, 0 to 127). Address / parity formats: the aircraft address.
uint32_t crc24(const uint8_t* msg, int nbits);
// Message length of a downlink format: 56, 112, or 0 when the format is not one the receiver searches for
int dfLength(int df);
// The 24 parity bits of a message whose first nbits - 24 bits are filled in (the rest is ignored). XOR the address in for address / parity formats.
uint32_t crcParity(const uint8_t* msg, int nbits);

// Bits first .. first + count - 1 (1 based, as in the standards), count <= 32
uint32_t getBits(const uint8_t* msg, int first, int count);
void setBits(uint8_t* msg, int first, int count, uint32_t value);

// Single-bit error correction: the position (0 based from the first bit) whose flip explains this remainder, or -1
int singleBitPosition(int nbits, uint32_t remainder);
// The remainder produced by flipping bit pos of an nbits message
uint32_t singleBitSyndrome(int nbits, int pos);

// ---- altitude, identity and character codes
// 13 bit AC field of DF0/4/16/20 (C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4). False when it carries no altitude or a code that is not defined.
bool decodeAc13(unsigned ac13, int& altFt);
// 12 bit altitude of an airborne position message (the AC field without the M bit)
bool decodeAlt12(unsigned alt12, int& altFt);
// Gillham (Mode C) code, 11 bits C1 A1 C2 A2 C4 A4 B1 B2 D2 B4 D4 (D1 is 0) to altitude in feet; false for an undefined code
bool gillhamToAltitude(unsigned code11, int& altFt);
// ... and back (-1200 to 126700 ft in 100 ft steps); false when out of range
bool altitudeToGillham(int altFt, unsigned& code11);
// 13 bit identity field (C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4) to the four octal digits of the squawk, written as a decimal number (7700)
int decodeId13(unsigned id13);
unsigned encodeId13(int squawk);
// Aircraft identification character set, 6 bit code to character ('#' for a code that is not defined, ' ' is 32)
char charFrom6bit(unsigned c);
int charTo6bit(char c);   // -1 when the character has no code
// Category from the type code (1-4: sets D, C, B, A) and CA field: "A3", and a name such as "Large (75000 - 300000 lb)"
std::string categoryCode(int tc, int ca);
std::string categoryName(int tc, int ca);
const char* emergencyName(int state);   // aircraft status emergency state 0-7

// ---- compact position reporting (CPR), 17 bit, NZ = 15
int cprNl(double lat);                                  // number of longitude zones at this latitude
// Global decoding of an even and an odd airborne frame (17 bit values); latest says which of the two came last
bool cprGlobalAirborne(int latEven, int lonEven, int latOdd, int lonOdd, bool oddIsLatest, double& lat, double& lon);
// Local decoding: the position is the one nearest to the reference (valid within 180 NM for airborne, 45 NM for surface)
bool cprLocalAirborne(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon);
// Surface positions repeat every 90 degrees: the reference picks the solution
bool cprGlobalSurface(int latEven, int lonEven, int latOdd, int lonOdd, bool oddIsLatest, double refLat, double refLon, double& lat, double& lon);
bool cprLocalSurface(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon);
// Encoding (the generator and the tests use it)
void cprEncodeAirborne(double lat, double lon, bool odd, int& latCpr, int& lonCpr);
void cprEncodeSurface(double lat, double lon, bool odd, int& latCpr, int& lonCpr);

// Great circle distance (NM) and initial bearing (degrees from north) between two points
double distanceNm(double lat1, double lon1, double lat2, double lon2);
double bearingDeg(double lat1, double lon1, double lat2, double lon2);

// ---- Comm-B
// What a BDS code inference found in the 56 bit MB field of a DF20 / DF21 message
struct CommB {
    int bds = 0;                       // 0x20, 0x40, 0x50, 0x60, or 0 when the field is not one of them (or fits more than one)
    std::string callsign;              // 2,0
    bool hasSelAlt = false; int selAltMcpFt = 0; bool hasSelAltFms = false; int selAltFmsFt = 0; bool hasBaro = false; double baroMb = 0;   // 4,0
    bool hasRoll = false; double rollDeg = 0; bool hasTrack = false; double trackDeg = 0; bool hasGs = false; int gsKt = 0;                 // 5,0
    bool hasTrackRate = false; double trackRateDps = 0; bool hasTas = false; int tasKt = 0;
    bool hasHeading = false; double headingDeg = 0; bool hasIas = false; int iasKt = 0; bool hasMach = false; double mach = 0;           // 6,0
    bool hasBaroRate = false; int baroRateFpm = 0; bool hasInertialRate = false; int inertialRateFpm = 0;
};
// mb: the 7 bytes of the MB field (message bits 33-88). Follows the checks of the BDS inference chapter of the Riddle: reserved bits zero,
// status bits consistent with their values, values in a plausible range; a field that passes for two formats is not decoded.
CommB inferCommB(const uint8_t* mb);

// ---- the decoded message
struct Msg {
    int df = 0;
    int nbits = 0;
    uint32_t icao = 0;           // DF11 / 17 / 18 / 19: the AA field. Address / parity formats: set by the caller from the remainder
    int ca = 0;                  // capability (DF11, DF17) or control field (DF18)
    bool ground = false;         // on the ground (flight status, surface position, capability)
    bool alert = false, spi = false;
    int fs = -1;                 // flight status (DF4 / 5 / 20 / 21)
    int vs = -1;                 // vertical status (DF0 / 16)
    bool hasAlt = false; int altFt = 0; bool altGnss = false;   // barometric altitude (DF0 / 4 / 16 / 20, TC 9-18), or GNSS height (TC 20-22)
    bool hasSquawk = false; int squawk = 0;                     // DF5 / 21, TC 28
    // extended squitter (DF17 / 18 / 19)
    int tc = -1;                 // type code 1 - 31
    int st = -1;                 // subtype
    bool hasIdent = false; std::string callsign; char catSet = 0; int catCode = 0;   // TC 1-4: catSet is 'A' to 'D'
    bool hasCpr = false; bool cprOdd = false; bool surface = false; int cprLat = 0, cprLon = 0;   // TC 5-8, 9-18, 20-22
    bool hasMove = false; double moveKt = 0; bool hasTrack = false; double trackDeg = 0;          // surface movement (TC 5-8)
    bool hasVel = false; double speedKt = 0; int speedKind = 0;                                    // TC 19: 0 ground speed, 1 indicated airspeed, 2 true airspeed
    bool hasHeading = false; double headingDeg = 0; bool headingIsTrack = false;                   // track for ground speed, magnetic heading for airspeed
    bool hasVrate = false; int vrateFpm = 0; bool vrateBaro = false;
    bool hasGnssDiff = false; int gnssDiffFt = 0;        // GNSS height minus barometric altitude
    int emergency = -1;                                  // TC 28: emergency state, 0 none
    // TC 29 target state and status (subtype 1)
    bool hasSelAlt = false; int selAltFt = 0; bool selAltFms = false; bool hasBaro = false; double baroMb = 0; bool hasSelHdg = false; double selHdgDeg = 0;
    int tsNacp = -1; bool autopilot = false, vnav = false, altHold = false, approach = false, lnav = false, tsModeValid = false;
    // TC 31 operational status
    int adsbVersion = -1; int nacp = -1; int nicSuppA = -1; int sil = -1; int gva = -1; int nicBaro = -1;
    // DF20 / 21 Comm-B
    CommB commb;
};

// Field extraction for a message whose length and DF are right. The caller has done the CRC and the address checks.
bool decodeMsg(const uint8_t* bytes, int nbits, Msg& m);
std::string toHex(const uint8_t* bytes, int nbits);

} // namespace dect2::adsb
