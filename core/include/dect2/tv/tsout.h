// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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
    // An HTTP server for the same stream: GET http://host:httpPort/stream.ts answers video/mp2t and then the packets as they are paced (OBS, VLC, ffmpeg).
    bool http = false;
    int httpPort = 8080;     // 0: any free port (httpPortInUse() in the stats says which)
    bool httpLan = false;    // listen on every address (else 127.0.0.1 only)
    std::string httpKey;     // non-empty: the request must carry ?key=<this>
    std::string iface;       // multicast: the local address of the adapter the datagrams leave by ("" = the system's choice)
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
    bool httpOpen = false;
    int httpPortInUse = 0, httpClients = 0;
    uint64_t httpBytes = 0, httpDropped = 0;   // bytes sent to viewers; chunks a slow viewer missed
    std::string error;
};

// "udp://127.0.0.1:1234", "udp://239.1.1.1:5000", "rtp://host:port", "http://:8080" or "http://0.0.0.0:8080" (the HTTP server; any address but the loopback means the network).
// Fills the network fields of `c` (udp / rtp / host / port, or http / httpPort / httpLan) and clears the other kind; nothing else is touched.
// false: err says what is wrong (the address is not one of these, or the port is not 1..65535).
bool parseStreamUrl(const std::string& url, OutputConfig& c, std::string& err);

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
