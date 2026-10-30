// nRF24L01 telemetry, shared by the receiver and the UI.
#pragma once
#include "dect2/nrf24/nrf24_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kNrf24FramesMax = 300;
constexpr size_t kNrf24DevicesMax = 100;
constexpr size_t kNrf24BurstsMax = 60;

struct Nrf24Frame {
    int64_t utc = 0;
    double timeSec = 0;
    int rateKbps = 0;            // 250, 1000 or 2000
    std::string address;         // hex, first byte first
    int addrLen = 0, payloadLen = 0, pid = 0, crcBits = 16;
    bool noAck = false;
    std::string payloadHex, text;
    float snrDb = 0;
    int repeats = 1;             // the same frame again in a row (ESB retransmits until it is acknowledged)
    bool confirmed = true;       // a CRC-8 frame is shown once its content was seen three times
};

struct Nrf24Device {
    std::string address;
    int addrLen = 0, rateKbps = 0, crcBits = 16;
    uint64_t count = 0;
    int64_t firstSeen = 0, lastSeen = 0;
    int lastPayloadLen = 0;
};

// A burst that gave no frame (another system on the channel, a damaged frame, a rate or format ESB does not describe)
struct Nrf24Burst {
    int64_t utc = 0;
    double timeSec = 0, durationUs = 0;
    float snrDb = 0;
};

struct Nrf24Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 bursts seen, 2 frames decoded lately
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // frames / bursts without a frame
    double timeSec = 0, inputRate = 0;
    uint64_t bursts = 0, crc16Frames = 0, crc8Frames = 0, frames250 = 0, frames1M = 0, frames2M = 0;
    std::vector<Nrf24Frame> frames;       // newest first
    std::vector<Nrf24Device> devices;     // newest first
    std::vector<Nrf24Burst> burstLog;     // newest first
};

inline std::string nrf24Summary(const Nrf24Telemetry& t) {
    char b[160];
    if (t.blocksOk) { std::snprintf(b, sizeof b, "nRF24: %llu frames from %zu address(es)", (unsigned long long)t.blocksOk, t.devices.size()); return b; }
    if (t.bursts) { std::snprintf(b, sizeof b, "2.4 GHz channel: %llu bursts, no ESB frame decoded", (unsigned long long)t.bursts); return b; }
    return "nRF24: no signal";
}

} // namespace dect2
