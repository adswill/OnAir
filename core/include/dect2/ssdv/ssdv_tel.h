// SSDV telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (history plot, top bar).
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kSsdvPacketsMax = 400;
constexpr size_t kSsdvImagesMax = 8;

struct SsdvPacketLog {
    int64_t utc = 0;
    double timeSec = 0;
    std::string call;
    int imageId = 0, packetId = 0;
    int rsErrors = 0;          // bytes corrected by the Reed-Solomon code
    int baud = 0;
    bool async = false;        // 8N2 framed bytes (RTTY style) instead of a plain bit stream
    bool wenet = false;        // came over a Wenet link (LDPC frames, 115.2 / 96 kbaud)
    bool inverted = false;     // found with the two frequencies swapped
    bool noFec = false;
    bool eoi = false;
    float snrDb = 0;
};

struct SsdvImage {
    std::string call;
    int imageId = 0;
    int width = 0, height = 0;
    int quality = 0;
    int packets = 0;           // distinct packets held
    int lastPacket = -1;       // number of the last packet (-1: not seen yet)
    int missing = 0;
    bool complete = false;
    int64_t firstUtc = 0, lastUtc = 0;
    uint64_t rev = 0;          // grows with every packet added
    std::shared_ptr<const std::vector<uint8_t>> jpeg;   // the picture so far (gaps are grey); empty before the first packet
    std::string savedPath;     // where the receiver wrote it (empty: not saved)
    std::vector<std::pair<int, int>> gaps;   // MCU ranges lost packets may have left grey (Assembler::gapMcus)
};

struct SsdvTelemetry {
    uint64_t seq = 0;
    int state = 0;             // 0 nothing, 1 carrier, 2 packets in the last 30 s
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // packets that passed / failed the CRC (bad: a sync word was followed by a packet that failed)
    double timeSec = 0, inputRate = 0;
    int baud = 0;              // the speed the last good packet came in at
    bool async = false;
    bool wenet = false;        // the last good packet came over Wenet
    uint64_t wenetOther = 0, wenetIdle = 0;   // Wenet frames that were not SSDV: telemetry / text, and idle fill (both count in blocksOk)
    int fixedBaud = 0;         // the speed chosen by hand (0: automatic)
    uint64_t corrected = 0;    // packets that needed the Reed-Solomon code
    uint64_t imagesDone = 0;
    std::vector<SsdvPacketLog> packets;   // newest first
    std::vector<SsdvImage> images;        // newest first
};

inline std::string ssdvSummary(const SsdvTelemetry& t) {
    char b[160];
    if (t.blocksOk) {
        std::snprintf(b, sizeof b, "SSDV: %llu packets, %llu pictures done, %s%d baud", (unsigned long long)t.blocksOk, (unsigned long long)t.imagesDone, t.wenet ? "Wenet " : "", t.baud);
        return b;
    }
    if (t.state >= 1) return "SSDV: a carrier, no packet yet";
    return "SSDV: no signal";
}

} // namespace dect2
