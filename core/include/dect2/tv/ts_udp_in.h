// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live MPEG-TS input over UDP for the digital TV transmitters: a program such as OBS, ffmpeg or VLC sends a transport stream to
// udp://@:1234 (or a multicast group, udp://239.1.1.1:1234, or RTP-wrapped) and the transmitter reads it as its "video file".
// The modulator pulls packets at its own fixed rate. A small buffer absorbs network jitter; when it runs empty a null packet (PID 0x1FFF) is
// sent instead (the rate is kept, the receiver's PCR clock is not disturbed), when it overflows the oldest packets are dropped. Both are counted.
// Datagrams of any size are accepted (7 x 188 is typical): the byte stream is re-aligned to 188-byte packets on its own sync bytes, a
// 12-byte RTP header (payload type 33, MP2T) is removed. Nothing leaves this computer; OnAir only listens.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct TsUdpStats {
    bool running = false;
    uint64_t datagrams = 0;
    uint64_t packets = 0;        // 188-byte packets received and kept in order
    uint64_t nullsInserted = 0;  // packets the modulator asked for while the buffer was empty (or still filling)
    uint64_t dropped = 0;        // packets thrown away (oldest first) because the buffer was full
    uint64_t resyncs = 0;        // times the stream lost its alignment and was found again
    uint64_t junkBytes = 0;      // bytes skipped while searching the sync byte
    uint64_t rtpDatagrams = 0;   // datagrams that carried an RTP header
    size_t queued = 0;           // packets waiting now
    size_t capacity = 0;
    std::string address;         // what it listens on, "0.0.0.0:1234" or "239.1.1.1:1234 (multicast)"
};

// 188-byte alignment and the bounded queue, without a socket (the test feeds it directly).
class TsUdpBuffer {
public:
    explicit TsUdpBuffer(size_t capacityPackets = 6000, size_t prefillPackets = 64);
    void feedDatagram(const uint8_t* d, size_t n);   // one datagram (RTP header detected and removed)
    void feedBytes(const uint8_t* d, size_t n);      // a piece of the byte stream
    void pull(uint8_t* pk);                          // exactly 188 bytes: the oldest packet, else a null packet
    TsUdpStats stats() const;
private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

// "udp://@:1234", "udp://0.0.0.0:1234", "udp://239.1.1.1:1234", "rtp://@:5004" (also "udp://host:port?..." options are ignored)
bool parseTsUdpSpec(const std::string& spec, std::string& host, int& port);
bool isTsUdpSpec(const std::string& spec);

// Starts listening and returns the modulator's packet source (keeps the listener alive while it exists). false with err when it cannot listen.
bool tsUdpOpen(const std::string& spec, std::function<void(uint8_t*)>& out, std::string& err);
// The counters of the most recently opened input (running = false when none is open); for the transmit panel.
TsUdpStats tsUdpLastStats();

} // namespace dect2
