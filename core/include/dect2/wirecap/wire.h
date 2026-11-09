// Wireshark output: one switch per mode. A decoder hands its packets, exactly as received, to Hub::publish; with the mode's switch on they go to a pcapng
// file, to a live capture in Wireshark (extcap, tools/onair_extcap.cpp) and, for GSMTAP channels, to UDP. All off by default. docs/WIRESHARK-3.8.md.
//
// Packets stay as they were on the air: the hooks sit before any decryption, so an encrypted payload stays encrypted here.
#pragma once
#include "dect2/wirecap/wire_chan.h"
#include "dect2/wirecap/wire_format.h"
#include <atomic>
#include <memory>
#include <string>

namespace dect2::wire {

struct Config {
    std::string dir;                        // the folder of the pcapng files ("" no files)
    bool files = true, live = true, gsmtap = true;
    std::string gsmtapHost = "127.0.0.1";   // only ever a host the user typed in
    int gsmtapPort = kGsmtapPort;
    int streamPort = kDefaultStreamPort;    // always bound to 127.0.0.1
    uint64_t maxFileBytes = 1ull << 30;     // a file stops growing here (the live capture goes on)
};

struct Stats {
    uint64_t packets = 0, bytes = 0, dropped = 0;
    std::string file;      // the file being written ("" none yet)
    std::string error;     // why the file stopped ("" fine)
    int clients = 0;       // live captures attached
};

class Hub {
public:
    static Hub& get();
    ~Hub();
    void configure(const Config& c);
    Config config() const;
    void setEnabled(Chan c, bool on);
    bool enabled(Chan c) const { return on_[c].load(std::memory_order_relaxed); }
    // from any thread; returns at once (the packet is queued; when the queue is full it is dropped and counted). unixUsec 0: now.
    void publish(Chan c, const uint8_t* p, size_t n, int64_t unixUsec = 0);
    void publish(Chan c, const Bytes& b) { publish(c, b.data(), b.size()); }
    Stats stats(Chan c) const;
    int boundPort() const;      // the local port while listening, 0 otherwise
    bool waitIdle(int timeoutMs);   // everything queued so far has been written (tests)
    void shutdown();            // every switch off, files closed, port closed
private:
    Hub();
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<bool> on_[kChanCount];
};

inline bool on(Chan c) { return Hub::get().enabled(c); }

// Copies the extcap program and onair.lua into the user's Wireshark folders. home: the home folder (Windows: the Roaming application data folder).
struct InstallResult { bool ok = false; std::string extcapPath, luaPath, error; };
InstallResult installIntoWireshark(const std::string& extcapProgram, const std::string& home);

} // namespace dect2::wire
