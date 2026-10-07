// ADS-B / Mode S message encoders: the test signal generator and the tests build messages with them.
// Every function returns a complete frame with its parity. The layouts are those decoded in adsb_msg.cpp.
#pragma once
#include "adsb_msg.h"
#include <string>

namespace dect2::adsb {

struct Frame {
    uint8_t b[14] = {};
    int bits = 112;
};

Frame encodeAllCall(uint32_t icao, int ca, int interrogatorCode = 0);                                            // DF11
Frame encodeIdentification(uint32_t icao, int ca, int tc, int catCode, const std::string& callsign);           // DF17, TC 1-4 (4: set A)
Frame encodeAirbornePosition(uint32_t icao, int ca, int tc, int altFt, bool gillham, bool odd, double lat, double lon);   // DF17, TC 9-18
Frame encodeGnssPosition(uint32_t icao, int ca, int tc, int heightFt, bool odd, double lat, double lon);       // DF17, TC 20-22
Frame encodeSurfacePosition(uint32_t icao, int ca, int tc, double speedKt, double trackDeg, bool odd, double lat, double lon);   // DF17, TC 5-8
Frame encodeVelocity(uint32_t icao, int ca, double speedKt, double trackDeg, int vrateFpm, bool hasDiff, int gnssDiffFt);   // TC 19 subtype 1
Frame encodeAirspeed(uint32_t icao, int ca, double speedKt, bool trueAirspeed, double headingDeg, int vrateFpm);            // TC 19 subtype 3
Frame encodeAircraftStatus(uint32_t icao, int ca, int emergency, int squawk);                                    // TC 28 subtype 1
Frame encodeTargetState(uint32_t icao, int ca, int selAltFt, double baroMb, double selHdgDeg, int nacp);         // TC 29 subtype 1
Frame encodeOperationalStatus(uint32_t icao, int ca, int version, int nacp, int sil);                            // TC 31 subtype 0
Frame encodeTisb(const Frame& es, int cf);                                                                       // the same ME in a DF18
Frame encodeAltitudeReply(uint32_t icao, int fs, int altFt, bool gillham);                                       // DF4
Frame encodeIdentityReply(uint32_t icao, int fs, int squawk);                                                    // DF5
Frame encodeAirAir(uint32_t icao, bool ground, int altFt, bool longForm);                                        // DF0 / DF16
Frame encodeCommB(uint32_t icao, bool identity, int fs, int altFtOrSquawk, const uint8_t mb[7]);                 // DF20 / DF21
// 56 bit MB fields
void mbCallsign(uint8_t mb[7], const std::string& callsign);                                                     // BDS 2,0
void mbSelectedVertical(uint8_t mb[7], int mcpFt, int fmsFt, double baroMb);                                     // BDS 4,0
void mbTrackAndTurn(uint8_t mb[7], double rollDeg, double trackDeg, double gsKt, double rateDps, double tasKt);   // BDS 5,0
void mbHeadingAndSpeed(uint8_t mb[7], double headingDeg, double iasKt, double mach, int baroRateFpm, int inertialRateFpm);   // BDS 6,0

// the 13 bit AC field for an altitude (Q = 1, 25 ft steps, or Gillham when gillham is set; 0 when it cannot be coded)
unsigned encodeAc13(int altFt, bool gillham);
unsigned encodeAlt12(int altFt, bool gillham);

} // namespace dect2::adsb
