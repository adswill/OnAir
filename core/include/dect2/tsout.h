// Transport-stream outputs: file recording and paced UDP/RTP streaming, optionally filtered to one service.
#pragma once
#include "ts.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

namespace dect2 {

struct OutputConfig {
    bool file = false;
    std::string path;
    bool udp = false;
    std::string host = "127.0.0.1";
    int port = 1234;
    int ttl = 8;
    bool rtp = false;
    int serviceId = -1;      // -1: whole multiplex
    bool dropNull = false;   // remove null packets (otherwise the multiplex is passed through unchanged)
};

struct OutputStats {
    bool fileOpen = false, udpOpen = false;
    uint64_t fileBytes = 0, filePackets = 0;
    uint64_t udpDatagrams = 0, udpBytes = 0, udpDropped = 0;
    uint64_t udpSendErrors = 0;   // datagrams the system refused to send
    uint64_t udpCatchUps = 0;     // times a backlog was sent sooner instead of being dropped
    double udpQueueMs = 0;
    std::string error;
};

class OutputManager {
public:
    OutputManager();
    ~OutputManager();
    void configure(const OutputConfig& c);
    OutputConfig config() const;
    // One transport-stream packet in stream order. `snap` supplies the PIDs of the selected service.
    void packet(const uint8_t* pkt188, const TsSnapshot* snap);
    // Many packets at once (one lock): `n` packets of 188 bytes back to back.
    void packets(const uint8_t* data, size_t n, const TsSnapshot* snap);
    // A burst of packets (one T2 frame) is complete; `frameSeconds` is the time it represents (used for UDP pacing).
    void burstDone(double frameSeconds);
    OutputStats stats() const;
    void close();

private:
    struct Impl;
    Impl* p_;
};

} // namespace dect2
