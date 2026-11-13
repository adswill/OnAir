// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (history plot, top bar).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kEsgChatMax = 500;
constexpr size_t kEsgPacketMax = 200;

// One line of the chat: heard (dir 0) or sent by OnAir (dir 1)
struct EsgChatLine {
    int64_t wallTime = 0;
    double sec = 0;
    int dir = 0;
    // 0 plain text, 1 decrypted, 2 encrypted and not readable (no key or another key), 3 an encrypted repeat dropped by the replay guard,
    // 4 not text and too short to be encrypted (shown in hex)
    int kind = 0;
    std::string text;          // the line as received (displayText() before drawing)
    std::string hex;           // the message octets
    int packets = 0;
    float levelDb = -200, snrDb = 0;
    double cfoHz = 0;
    uint64_t runId = 0;
    uint32_t counter = 0;
};

// One packet with a sync word (CRC good or not)
struct EsgPacket {
    int64_t wallTime = 0;
    double sec = 0;
    int len = 0;
    bool crcOk = false;
    bool inverted = false;     // found with the frequencies swapped (a receiver set up with I and Q exchanged)
    float levelDb = -200, snrDb = 0;
    double cfoHz = 0;
    std::string hex;           // the payload
};

struct EsubghzTelemetry {
    uint64_t seq = 0;
    int state = 0;             // 0 nothing heard, 1 bursts, 2 packets in the last seconds
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // packets with a good / a bad CRC
    double timeSec = 0, inputRate = 0;
    double signalOffsetHz = 0;
    uint64_t bursts = 0, messages = 0, decrypted = 0, unreadable = 0, replays = 0, sent = 0;
    bool keyOn = false;
    std::vector<EsgChatLine> chat;     // oldest first
    std::vector<EsgPacket> packets;    // newest first
};

inline std::string esubghzSummary(const EsubghzTelemetry& t) {
    char b[200];
    if (t.blocksOk == 0 && t.sent == 0) { snprintf(b, sizeof b, "listening for Flipper chat%s", t.keyOn ? " (key set)" : ""); return b; }
    snprintf(b, sizeof b, "%llu messages heard, %llu sent, %llu packets", (unsigned long long)t.messages, (unsigned long long)t.sent, (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
