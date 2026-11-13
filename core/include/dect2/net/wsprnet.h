// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WSPRnet (wsprnet.org) spot upload: HTTP POST application/x-www-form-urlencoded to wsprnet.org/post/ (port 80).
// Off until the user opts in; spots are public and need a valid amateur callsign and locator.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace wsprnet {

// One spot row for the upload form (pure; tested without the network).
struct SpotFields {
    std::string rcall, rgrid;   // the receiving station
    double rqrgMhz = 0;         // dial frequency, MHz
    std::string tcall, tgrid;   // the transmitting station
    double tqrgMhz = 0;         // spotted RF, MHz
    int sigDb = 0;              // SNR, dB (wsprnet "sig")
    double dt = 0;              // s, timing offset
    int drift = 0;              // Hz drift (0 when unknown)
    int dbm = 0;                // reported TX power, dBm
    int64_t slotUtc = 0;        // UTC seconds at the start of the 2-minute WSPR slot
};

std::string urlEncodeForm(const std::string& s);
std::string encodeSpotBody(const SpotFields& s, const std::string& version);   // POST body, no leading ?

// Same sender in the same 2-minute slot and on the same audio tone (10 Hz) is kept once.
class Dedupe {
public:
    explicit Dedupe(int windowSec = 130) : window_(windowSec) {}
    bool accept(const std::string& tcall, int64_t slotUtc, int freqHz10, int64_t now);
    void clear() { seen_.clear(); }
private:
    struct Seen { std::string key; int64_t t; };
    int window_;
    std::vector<Seen> seen_;
};

} // namespace wsprnet

struct WsprNetConfig {
    bool on = false;
    std::string call, locator;
    std::string host = "wsprnet.org";
    int port = 80;
};

struct WsprNetStatus {
    bool active = false;
    uint64_t spotsSent = 0, duplicates = 0, failures = 0;
    int64_t lastSend = 0;
    std::string state, lastError;
};

class WsprNet {
public:
    static WsprNet& get();
    ~WsprNet();
    void configure(const WsprNetConfig& c);
    WsprNetConfig config() const;
    WsprNetStatus status() const;
    void stop();
    void setDialHz(double hz) { dial_.store(hz, std::memory_order_relaxed); }
    bool want() const { return want_.load(std::memory_order_relaxed); }
    // WSPR decode path only: audioHz offset from dial, slotUtc start of the 2-minute slot.
    void spot(double audioHz, float snrDb, float dt, const std::string& call, const std::string& grid, int dbm, double slotUtc, double dialHz = 0);   // dialHz > 0: the dial of the band it was heard on (multi-band), else the tuned one
    struct Impl;
private:
    WsprNet();
    std::atomic<bool> want_{false};
    std::atomic<double> dial_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace dect2
