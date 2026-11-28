// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WSJT-X UDP message protocol (the NetworkMessage.hpp layout, schema 2, Qt QDataStream big-endian), the sending side: GridTracker, JTAlert,
// Log4OM and others listen on UDP port 2237 for Heartbeat, Status and Decode messages. The messages are written from the published
// protocol description; the encoders are pure (no socket) so that they can be tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::ham::wsjtx {

constexpr uint32_t kMagic = 0xADBCCBDAu;
constexpr uint32_t kSchema = 2;
constexpr int kDefaultPort = 2237;
enum MsgType : uint32_t { Heartbeat = 0, Status = 1, Decode = 2 };

// Heartbeat: id, max schema, software version, revision
std::vector<uint8_t> encodeHeartbeat(const std::string& id, const std::string& version, const std::string& revision);
// Decode: new, time (ms since UTC midnight), snr dB, dt s, audio Hz, mode char ("~" FT8, "+" FT4), message
struct DecodeMsg { bool isNew = true; uint32_t msSinceMidnight = 0; int snr = 0; double dt = 0; uint32_t df = 0; std::string mode = "~"; std::string text; bool lowConfidence = false; bool offAir = false; };
std::vector<uint8_t> encodeDecode(const std::string& id, const DecodeMsg& m);
// Status: the dial frequency and mode GridTracker uses to place the decodes on a band
struct StatusMsg { uint64_t dialHz = 0; std::string mode = "FT8"; std::string deCall, deGrid; bool decoding = false; uint32_t rxDf = 0, trPeriod = 15; };
std::vector<uint8_t> encodeStatus(const std::string& id, const StatusMsg& m);

// The mode character of a decode message: "~" FT8, "+" FT4, "" for modes the protocol does not carry (the others are not sent)
std::string modeChar(const std::string& modeName);

// A UDP sender to host:port (a loopback address by default). send() returns false when the socket cannot be opened or the send fails.
class Sender {
public:
    Sender();
    ~Sender();
    bool configure(const std::string& host, int port);   // reopens the socket when the target changed
    bool send(const std::vector<uint8_t>& pkt);
    const std::string& error() const;
private:
    struct Impl;
    Impl* p_;
};

}  // namespace dect2::ham::wsjtx
