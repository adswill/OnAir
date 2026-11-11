// Drone Remote ID telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kRidRssiHist = 24;    // the last levels of a drone, oldest first
constexpr size_t kRidTrack = 120;      // positions kept per drone (the trail on the map)
constexpr size_t kRidDroneMax = 200;   // drones kept (the longest unheard go first)
constexpr size_t kRidLogMax = 400;     // messages in the log

struct RidPoint { double lat = 0, lon = 0; float alt = -1000; };

// One drone: everything heard from one broadcast address (a BLE advertiser or a Wi-Fi transmitter)
struct RidDrone {
    std::string mac;                   // the BLE advertising address or the Wi-Fi source address
    bool viaBle = false, viaWifi = false;
    // Basic ID (a drone may send two: a serial number and a session ID)
    std::string serial, sessionId, caaId, utmId;
    int uaType = -1;                   // -1: no Basic ID heard yet
    // Location / vector
    bool hasPos = false;
    double lat = 0, lon = 0;
    float altGeo = -1000, altBaro = -1000, height = -1000;   // m; <= -1000 unknown
    int heightType = 0;                // 0 above take-off, 1 above ground
    int status = 0;
    float speed = -1, vspeed = 0, direction = -1;   // m/s, m/s up, degrees true; < 0 unknown (vspeedValid for vspeed)
    bool vspeedValid = false;
    int horizAcc = 0, vertAcc = 0, speedAcc = 0;
    int timestamp = -1;                // tenths of a second after the hour
    std::vector<RidPoint> track;
    // System
    bool hasOperator = false;
    double opLat = 0, opLon = 0;
    float opAlt = -1000;
    int opLocType = 0, classType = 0, category = 0, uaClass = 0;
    int areaCount = 0, areaRadius = 0;
    uint32_t sysTime = 0;
    // Operator ID, Self-ID, authentication
    std::string operatorId, selfId;
    int selfIdType = 0;
    int authType = -1, authPages = 0;
    // reception
    float rssi = -200;
    std::vector<float> rssiHist;
    double firstSec = 0, lastSec = 0;  // signal time
    double autoEmitSec = -1e9;         // the automations: signal time of the last report of this drone
    int64_t firstWall = 0, lastWall = 0;
    uint64_t messages = 0;
    uint32_t typeMask = 0;             // 1 << message type heard
    int counter = -1;                  // the last message counter
};

struct RidMsgLog {
    int64_t wallTime = 0;
    double sec = 0;
    int transport = 0;                 // 0 Bluetooth LE, 1 Wi-Fi beacon
    int channel = 0;                   // BLE 37 / 38 / 39, Wi-Fi 1 - 13 (0: not known)
    std::string mac;
    int type = 0;                      // rid::MsgType
    float rssi = -200;
    std::string info;
};

struct RidTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 Remote ID heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last good packet
    float snrDb = 0;
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // Remote ID messages decoded
    uint64_t blocksBad = 0;            // BLE CRC / Wi-Fi FCS failures of the demodulators
    // details
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;              // the radio's centre (0: a file)
    int bleChannel = 0, wifiChannel = 0;   // what each demodulator listens to (0: BLE off; Wi-Fi 0 with a file)
    double bleOffsetHz = 0, wifiOffsetHz = 0;
    uint64_t blePackets = 0, wifiFrames = 0;   // all good packets / frames heard
    uint64_t bleRid = 0, wifiRid = 0;  // of them, with Remote ID
    uint64_t perType[16] = {};
    float wifiFed = 1;                 // share of the samples the burst gate handed to the Wi-Fi demodulator over the last report (1: all)
    std::vector<RidDrone> drones;      // most recently heard first
    std::vector<RidMsgLog> log;        // newest first
};

inline std::string ridSummary(const RidTelemetry& t) {
    char b[160];
    if (t.drones.empty()) return "listening for Remote ID";
    snprintf(b, sizeof b, "%zu drone%s, %llu messages", t.drones.size(), t.drones.size() == 1 ? "" : "s", (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
