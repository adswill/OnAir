// Network tuner: serves the live channel over HTTP so that phones, TVs, VLC, Plex and Jellyfin can watch it.
//   /               a small status page
//   /lineup.m3u     channel list (IPTV playlist)
//   /guide.xml      programme guide (XMLTV)
//   /stream/<id>    one service as an MPEG transport stream
//   /discover.json /lineup.json /lineup_status.json   the HTTP part of the HDHomeRun protocol (Plex and Jellyfin can add it by address)
#pragma once
#include "engine.h"
#include <string>

namespace dect2 {

struct NetTunerConfig {
    int port = 8089;
    bool localOnly = true;      // listen on 127.0.0.1 only
    std::string key;            // if not empty, every request needs ?key=<key>
    std::string name = "OnAir";
};

struct NetTunerStats {
    bool running = false;
    int clients = 0;
    uint64_t bytesSent = 0;
    int port = 0;
    std::string error;
};

class NetTuner {
public:
    explicit NetTuner(Engine& e);
    ~NetTuner();
    bool start(const NetTunerConfig& c);   // false: the port could not be opened (see stats().error)
    void stop();
    NetTunerStats stats() const;
    std::vector<std::string> addresses() const;   // URLs the playlist can be reached at (this computer's addresses)

private:
    struct Impl;
    Impl* p_;
};

} // namespace dect2
