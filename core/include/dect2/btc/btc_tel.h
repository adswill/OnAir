// Bluetooth Classic discovery telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning (as BleTelemetry).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kBtcDevMax = 400;    // devices kept (the longest unheard go first)
constexpr size_t kBtcLogMax = 300;    // packets in the log

// One device or piconet as heard: by its LAP (the lower 24 bits of the address; every packet of a piconet starts with its master's LAP)
struct BtcDevice {
    uint32_t lap = 0;
    std::string lapText;               // "A1B2C3"
    std::string kind;                  // "inquiry (GIAC)", "device or piconet master" ...
    bool hasFhs = false;               // an inquiry response was decoded: the full address and the class of device are known
    std::string addr;                  // "00:11:22:33:44:55" (NAP:UAP:LAP), from the FHS
    std::string cod, major;            // class of device as text, and its major class
    uint32_t codRaw = 0;
    uint32_t clk = 0;                  // CLK27-2 from the FHS, units of 1.25 ms
    float rssi = -200;                 // dBFS of the last packet (the radio's level, not calibrated dBm)
    std::vector<float> rssiHist;
    double firstSec = 0, lastSec = 0;  // signal time
    int64_t firstWall = 0, lastWall = 0;
    uint64_t packets = 0;
    double lastMhz = 0;                // the channel of the last packet
    uint64_t chanLo = 0, chanHi = 0;   // channel k (2402 + k MHz) heard: bit k of chanLo (0 to 63), bit k - 64 of chanHi
};

struct BtcPacket {
    int64_t wallTime = 0;
    double sec = 0;
    double mhz = 0;
    uint32_t lap = 0;
    float rssi = -200;
    int errors = 0;                    // bit errors in the sync word
    std::string info;                  // "ID packet, GIAC", "inquiry response: 00:11:22:33:44:55, Phone: smartphone" ...
};

struct BtcTelemetry {
    uint64_t seq = 0;
    int state = 0;                     // 0 nothing heard, 1 access codes heard in the last seconds
    double cfoHz = 0;                  // carrier offset of the last good packet
    float snrDb = 0;                   // SNR of the last good packets
    float levelDb = -200;              // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;             // access codes found
    uint64_t blocksBad = 0;            // bursts that looked like Bluetooth (right length) and had no access code
    // details
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;              // the radio's centre (0: not known, a file)
    int channelsCovered = 0;           // 1 MHz channels inside the sample rate
    uint64_t giacIds = 0;              // inquiry ID packets (something is scanning for devices)
    uint64_t fhsCount = 0;             // inquiry responses decoded
    uint64_t burstsSeen = 0, burstsWide = 0;   // narrow bursts analysed; wideband bursts (Wi-Fi and the like) skipped
    std::vector<BtcDevice> devices;    // most recently heard first
    std::vector<BtcPacket> log;        // newest first
};

inline std::string btcSummary(const BtcTelemetry& t) {
    char b[160];
    if (t.blocksOk == 0) {
        snprintf(b, sizeof b, "listening on %d channels", t.channelsCovered);
        return b;
    }
    snprintf(b, sizeof b, "%zu devices, %llu access codes", t.devices.size(), (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
