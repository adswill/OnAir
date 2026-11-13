// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live RDS encoder control: the station name (PS), radio text (RT), PI, PTY, TP / TA / music flag and the clock group (CT) of the FM transmitter
// can change while it transmits. A change is seen at the next group boundary of the encoder (FmGenerator), no restart. Four ways in, all ending at
// RdsLive::apply(): the panel in the transmit window, the REST endpoint (/api/v1/x/rds), the MCP tool rds_set and a plain text command port
// (UDP and TCP, one "KEY=value" per line, the style of the common RDS encoders: PS=NAME, RT=text, PI=4A21, PTY=10, TP=1, TA=0, MS=M, CT=1).
// A text file can also be watched: its first line is the radio text (for a now-playing script). Transmit policy: warnings only, nothing is refused.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace dect2 {

struct RdsState {
    std::string ps = "        ";   // exactly 8 characters
    std::string rt;                // up to 64 characters
    int pi = 0x4A21;
    int pty = 10;
    bool tp = true, ta = false, music = true;
    bool ct = false;               // send the clock-time group (4A) with the computer's UTC time
    int ctOffsetHalfHours = 0;     // the local time offset sent with it, in half hours (-24..24)
    int abFlag = 0;                // the radio text A/B flag: flips whenever the text changes, so receivers clear the old text
};

class RdsLive {
public:
    RdsLive();
    ~RdsLive();
    static RdsLive& global();                 // the one the transmitter, the panel and the APIs share

    RdsState get() const;
    uint64_t version() const;                 // rises with every change
    void seed(const RdsState& s);             // the transmit request's values (a start of the transmitter); abFlag is kept
    // One change. key (case blind): PS, RT / TEXT, PI (hex), PTY, TP, TA, MS / MUSIC (M, S, 1, 0), CT (0/1), CTOFF (half hours) .
    // true: done; false: err says why (the state is not changed).
    bool apply(const std::string& key, const std::string& value, std::string& err);
    // One text line "KEY=value" (or "KEY value"); "?" or "GET" answers the state. Returns the reply (no newline): "OK ..." or "ERR ...".
    std::string command(const std::string& line);
    std::string json() const;                 // the state as one JSON object

    // ---- sources of changes (each can be off)
    // The command port: UDP and TCP on this number (0 = off). Listens on 127.0.0.1 unless lan; with lan a token is needed (an empty token is refused).
    // A token set: each UDP datagram starts with "<token>;" and a TCP session sends "AUTH=<token>" first. Returns false with err when it cannot listen.
    bool listen(int port, bool lan, const std::string& token, std::string& err);
    int listeningPort() const;                // 0 = off
    // Watch a text file: its first line is the radio text whenever it changes (polled twice a second). Empty path = off.
    void watchFile(const std::string& path);
    std::string watchedFile() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The 104 bits worth of group B, C, D words for the clock group 4A from a UTC time (seconds since 1970); used by the encoder and the test.
void rdsClockWords(int64_t utc, int offsetHalfHours, bool tp, int pty, uint16_t& b, uint16_t& c, uint16_t& d);

} // namespace dect2
