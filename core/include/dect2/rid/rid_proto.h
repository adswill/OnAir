// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID (ASTM F3411-22a, ASD-STAN EN 4709-002 "Open Drone ID"): the 25-byte messages a drone broadcasts openly about itself
// and its operator, and the two broadcast carriers this mode hears: Bluetooth LE legacy advertising (service data, UUID 0xFFFA, application
// code 0x0D, one message per packet) and Wi-Fi beacons (vendor specific element, OUI FA:0B:BC type 0x0D, a message pack). Receive side
// only: the messages are public by law (EU 2019/945, FAA 14 CFR part 89) and nothing here transmits or answers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::rid {

constexpr int kMsgSize = 25;             // every message, the pack's entries included
constexpr int kMaxPack = 9;              // messages in a pack (a Wi-Fi element holds at most 9 after its header)
constexpr uint16_t kBleUuid = 0xFFFA;    // ASTM International: the 16-bit service UUID of the service data
constexpr uint8_t kAppCode = 0x0D;       // Open Drone ID application code (after the UUID), also the vendor element's OUI type
constexpr uint8_t kOui[3] = {0xFA, 0x0B, 0xBC};   // ASD-STAN, the Wi-Fi vendor specific element

enum MsgType { BasicId = 0, Location = 1, Auth = 2, SelfId = 3, System = 4, OperatorId = 5, Pack = 0xF };

// One decoded message. The fields of the other types stay at their defaults. Unknown values are kept as the spec marks them:
// speed < 0, vspeed NaN, direction < 0, an altitude <= -1000, lat / lon 0 (posValid false), timestamp < 0.
struct Message {
    int type = -1, version = 0;
    // Basic ID
    int idType = 0;                      // 0 none, 1 serial number (ANSI/CTA-2063-A), 2 CAA registration, 3 UTM UUID, 4 specific session ID
    int uaType = 0;                      // 0 none, 1 aeroplane, 2 helicopter / multirotor ... 15 other
    std::string id;                      // up to 20 characters (types 1, 2, 4); type 3: the 16 UUID bytes as hex
    // Location / vector
    int status = 0;                      // 0 undeclared, 1 ground, 2 airborne, 3 emergency, 4 remote ID system failure
    int heightType = 0;                  // 0 above take-off, 1 above ground
    bool posValid = false;
    double lat = 0, lon = 0;
    double altBaro = -1000, altGeo = -1000, height = -1000;   // m
    double speed = -1;                   // m/s over ground
    double vspeed = 0; bool vspeedValid = false;   // m/s, up positive
    double direction = -1;               // degrees from true north
    int horizAcc = 0, vertAcc = 0, baroAcc = 0, speedAcc = 0;   // the spec's enums (0 unknown)
    int timestamp = -1;                  // tenths of a second since the full hour (UTC)
    int tsAcc = 0;                       // tenths of a second, 0 unknown
    // Authentication
    int authType = 0, page = 0, lastPage = 0, authLength = 0;
    uint32_t authTime = 0;               // seconds since 2019-01-01 00:00 UTC (page 0)
    std::vector<uint8_t> authData;       // this page's share
    // Self-ID
    int descType = 0;                    // 0 text, 1 emergency, 2 extended status
    std::string text;                    // Self-ID description, or the operator ID
    // System
    int opLocType = 0;                   // 0 take-off position, 1 live GNSS, 2 fixed
    int classType = 0;                   // 0 undeclared, 1 EU
    bool opValid = false;
    double opLat = 0, opLon = 0, opAlt = -1000;
    int areaCount = 1, areaRadius = 0;   // m
    double areaCeiling = -1000, areaFloor = -1000;
    int category = 0, uaClass = 0;       // EU: 0 undefined, 1 open, 2 specific, 3 certified; class 0 undefined, 1 .. 7 = class C0 .. C6
    uint32_t sysTime = 0;                // seconds since 2019-01-01 00:00 UTC
    // Operator ID
    int opIdType = 0;
};

bool decodeMessage(const uint8_t* m, Message& out);                       // 25 bytes; false for an unknown type
// A message or a message pack (header 0xF?): the messages appended to out; returns how many, -1 malformed
int decodeBlock(const uint8_t* p, size_t n, std::vector<Message>& out);
void encodeMessage(const Message& m, uint8_t out[kMsgSize]);
std::vector<uint8_t> encodePack(const std::vector<Message>& msgs);

// The carriers. Both return false when the data holds no Open Drone ID; counter: the message counter byte.
bool fromBleAdvData(const uint8_t* ad, size_t n, int& counter, std::vector<Message>& out);   // the AD structures of an advertising packet
bool fromBeacon(const uint8_t* mpdu, size_t n, int& counter, std::vector<Message>& out);      // a beacon or probe response (n without FCS)
std::vector<uint8_t> bleServiceData(int counter, const Message& m);      // the AD structure (31 bytes)
std::vector<uint8_t> beaconElement(int counter, const std::vector<Message>& msgs);   // the vendor specific element (0xDD ...)

const char* msgTypeName(int t);
const char* uaTypeName(int t);
const char* idTypeName(int t);
const char* statusName(int s);
const char* opLocTypeName(int t);
const char* authTypeName(int t);
const char* categoryName(int c);
std::string className(int c);                // "C2", "-"
std::string horizAccText(int a);             // "< 10 m" ...
std::string vertAccText(int a);
std::string speedAccText(int a);
std::string summary(const Message& m);       // one line for the message log

} // namespace dect2::rid
