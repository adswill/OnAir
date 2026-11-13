// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B feeds in from other receivers (dump1090, readsb, another OnAir): Beast binary and SBS / BaseStation text over TCP.
// Positions from other receivers are shown, not computed: there is no MLAT (that needs synchronised time stamps across sites).
// The parsers are plain functions on bytes and lines (tests); AdsbFeedIn owns the connections.
#pragma once
#include "adsb_track.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct BeastFrame {
    int bits = 0;                // 56 or 112 (Mode A/C frames, type '1', are dropped)
    uint8_t bytes[14] = {};
    uint8_t signal = 0;          // the RSSI byte
    uint64_t ts12 = 0;           // 12 MHz counter of the sender
};

// Beast binary: 0x1A, a type ('1' Mode A/C, '2' short, '3' long), 6 byte time stamp, a signal byte, the frame; a 0x1A inside is doubled.
// push() takes any chunk of the stream and appends the frames that completed; it finds its way back after garbage.
class BeastParser {
public:
    void push(const uint8_t* d, size_t n, std::vector<BeastFrame>& out);
    uint64_t resyncs() const { return resyncs_; }
private:
    std::vector<uint8_t> buf_;      // the unescaped payload of the frame being read
    int state_ = 0;                 // 0 outside a frame, 1 after 0x1A, 2 in the payload, 3 in the payload after 0x1A
    int need_ = 0;                  // payload bytes to go (7 + frame length)
    uint64_t resyncs_ = 0;
};

// One SBS "MSG,..." line (CRLF already removed). False for other lines (STA, AIR, ID, SEL, CLK) and lines that cannot be read.
bool sbsParseLine(const std::string& line, AdsbExtUpdate& u);

// A host that is on this network: loopback, private IPv4, link-local, IPv6 ULA / link-local, and the names localhost, *.local, *.lan, *.home, *.internal
bool adsbHostIsLan(const std::string& host);
// "host:port" (port 30005 Beast / 30003 SBS style); false with err for a bad address
bool adsbSplitHostPort(const std::string& s, std::string& host, int& port, std::string& err);

class AdsbFeedIn {
public:
    // Called from the connection threads. frame: a Beast frame (112 or 56 bits); update: an SBS line. src is 1 for the first source.
    struct Sink {
        std::function<void(int src, const uint8_t* bytes, int bits)> frame;
        std::function<void(int src, const AdsbExtUpdate& u)> update;
    };
    AdsbFeedIn();
    ~AdsbFeedIn();
    void setSink(Sink s);
    // Replace the list of sources (host:port each). Addresses outside the local network are left out unless allowNonLan; the reason for each
    // refusal is in info()[i].error.
    void setSources(const std::vector<std::string>& hostPorts, bool allowNonLan);
    std::vector<AdsbSourceInfo> info() const;    // one per source, the first is source 1 (not "this receiver")
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
