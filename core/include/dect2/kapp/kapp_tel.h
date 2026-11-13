// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP receiver telemetry. RxTelemetry::kapp, engine standard 126, active standard 125 (family 124).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// One message that arrived whole (every block of it, by selective repeat).
struct KappPacket {
    std::string callsign;
    int profile = 0;               // profile of the burst that completed it
    uint32_t bytes = 0;
    uint64_t contentHash = 0;      // FNV-1a 64 of the message bytes (for a check against the original)
    double atSec = 0;              // stream time when it completed
    float snrDb = 0;               // Es/N0 of that burst
    bool binary = false;           // the bytes are not mostly text
    std::string preview;           // up to 160 bytes, control characters shown as '.'
};

struct KappTelemetry {
    uint64_t seq = 0;
    // 0 no input, 1 listening (no burst yet or none for 5 s), 2 receiving (a burst header passed its CRC within the last 5 s). A packet mode has no
    // continuous lock; "receiving" means bursts are being found.
    int state = 0;
    double inputRate = 0, timeSec = 0;
    bool dataValid = false;        // blocks have passed their CRC (the engine's "data valid" flag)
    // the last burst heard
    std::string callsign;
    int profile = 0;               // profile id 0 .. 8 of that burst (-1 none yet)
    float snrDb = 0;               // Es/N0 of that burst
    float cfoHz = 0;
    double lastBurstAgeSec = 0;
    // counts since the start (or the last clear)
    uint64_t detections = 0;       // preamble candidates
    uint64_t headerOk = 0, headerFail = 0;
    uint64_t bursts = 0;           // data bursts
    uint64_t acks = 0;             // ACK bursts heard (this receiver sends none: it listens only)
    uint64_t blocksOk = 0, blocksBad = 0;
    uint64_t messages = 0;         // distinct complete messages
    uint64_t repeats = 0;          // complete messages that were a repeat of one already delivered
    uint64_t bytesDelivered = 0;   // bytes of the distinct complete messages
    double throughputBps = 0;      // payload bits per second of the blocks that passed, averaged over the last 10 s
    uint32_t partialBytes = 0;     // bytes held of messages still incomplete
    uint32_t partialMessages = 0;
    uint64_t profileBursts[9] = {};   // bursts per profile
    int constellationBits = 0;     // bits per symbol of the last burst's modulation (1 BPSK, 2 QPSK, 4, 6, 8, 10)
    std::vector<std::pair<float, float>> constellation;   // up to 400 equalised data symbols of the last burst (I, Q)
    std::vector<KappPacket> packets;   // newest first, up to 32
};

inline std::string kappSummary(const KappTelemetry& t) {
    char b[200];
    if (t.state == 0) return "KAPP: waiting for samples";
    if (t.state == 1) return "KAPP: listening";
    snprintf(b, sizeof b, "KAPP: receiving, profile %d, SNR %.0f dB", t.profile, (double)t.snrDb);
    return b;
}

} // namespace dect2
