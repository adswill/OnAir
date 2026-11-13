// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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

// ---- ERMES (ETS 300 133-4)
uint32_t ermesEncode(uint32_t info18);                           // the 30 bit codeword (for the tests)
bool ermesDecode(uint32_t word30, uint32_t& info18, int* fixed); // repairs up to two errors
constexpr uint32_t kErmesPreamble = 0b001000100010001000100010001000u, kErmesSync = 0b100010100010000010100000101010u, kErmesApt = 0b100100111000011000100011100100u, kErmesDelim = 0b110101011110011111101110111011u;
struct ErmesSystem { int country = 0, oper = 0, area = 0, eti = 0, bai = 0, fsi = 0, cycle = 0, subseq = 0, batch = 0, ssit = 0, ssif = 0; };
ErmesSystem ermesSystem(uint32_t si0, uint32_t si1, uint32_t ssi);
std::string ermesSystemText(const ErmesSystem& s);
std::string ermesMessageText(int category, const std::vector<uint32_t>& data18);   // category: 0 tone, 1 numeric, 2 alphanumeric, 3 transparent

// ---- 802.11p / ITS-G5 safety messages
struct Bsm { int msgCnt = 0; uint32_t id = 0; int lat = 0, lon = 0, elev = 0, speed = 0, heading = 0; };
bool bsmParse(const uint8_t* p, size_t n, Bsm& out);             // p at the start of the MessageFrame (messageId 20, UPER)
struct Cam { uint32_t station = 0; int protocol = 0, stationType = 0, lat = 0, lon = 0; };
bool camParse(const uint8_t* p, size_t n, Cam& out);             // p at the ItsPduHeader
std::string itsFrameText(const uint8_t* mpdu, size_t n, std::string& kind, std::string& id, double* latLon, bool& hasPos);  // an 802.11 data frame; "" when no safety message was found

// ---- DSRC tolling: HDLC framing only
struct DsrcFrame { size_t bytes = 0; int lidBytes = 0; bool broadcast = false; int control = 0; };
bool dsrcFrame(const uint8_t* p, size_t n, DsrcFrame& out);      // p: a frame between flags including its FCS; false when the FCS fails
uint16_t dsrcFcs(const uint8_t* p, size_t n);                    // X.25

} // namespace dect2::avroad
