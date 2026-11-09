// Pure protocol helpers of the aviation, rail and road modes: parsers and checks, tested on their own.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::avroad {

// ---- AAR end of train
struct EotFrame { int chaining = 0, batteryCond = 0, messageType = 0; uint32_t unitAddress = 0; int pressurePsi = 0, batteryPercent = 0; int valve = 0, confirm = 0, turbine = 0, motion = 0, markerBattery = 0, markerLight = 0; };
bool eotCheck(uint8_t* body63, int* fixed);       // d0 .. d44, c0 .. c17 as received; repairs up to two bits; false if it is not a codeword
EotFrame eotParse(const uint8_t* d45);
std::string eotText(const EotFrame& f, bool hot);

// ---- FANET (Skytraxx, published protocol.txt)
struct FanetFrame {
    int type = 0; bool forward = false; int manufacturer = 0; uint32_t device = 0;
    bool unicast = false, hasSignature = false; int destManufacturer = 0; uint32_t destDevice = 0;
    bool hasPos = false; double lat = 0, lon = 0;
    int aircraftType = 0; int altM = 0; float speedKmh = 0, climbMs = 0, headingDeg = 0; bool online = false;
    std::string text;      // a name, a message, or a description of the rest
    bool valid = false;
};
FanetFrame fanetParse(const uint8_t* p, size_t n);

// ---- OGN tracker radio packet (OGN-Tracker, as carried in SoftRF)
struct OgnPacket {
    uint32_t address = 0; int addrType = 0; bool other = false, encrypted = false, emergency = false; int relay = 0;
    bool addrParityOk = false;
    double lat = 0, lon = 0; int altM = 0; float speedMs = 0, headingDeg = 0, climbMs = 0, turnDegS = 0; int acftType = 0, fixQuality = 0, second = 0; bool stealth = false;
};
int ognParityErrors(const uint8_t* packet26);                    // failed checks of the (208,160) LDPC code
void ognParity(uint8_t* packet26);                               // writes bytes 20 .. 25 from bytes 0 .. 19 (for the tests)
bool ognRepair(uint8_t* packet26, const float* soft208, int iterations = 30);   // soft: > 0 means a one; bits numbered byte by byte, most significant first
OgnPacket ognParse(const uint8_t* packet26);                     // dewhitens a copy; an encrypted packet's position fields are left at zero
void ognWhiten(uint8_t* packet26);                               // the transmit-side scrambling (for the tests)

} // namespace dect2::avroad
