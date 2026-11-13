// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The spot feeds of the Ham screen (4.0): the network side of ham_spots.h. Every feed is off by default and runs only while the user has it
// switched on:
//  - a DX cluster and the Reverse Beacon Network over telnet: OnAir logs in with the user's callsign (what these servers ask for) and reads spots;
//  - POTA and SOTA activator spots from their public APIs (HTTPS GET, nothing about the user is sent);
//  - PSK Reporter "who hears me": HTTPS GET with the user's callsign in the query (the service's own interface), at most once in 5 minutes.
// Nothing else leaves the computer. One worker thread; the UI takes the parsed spots with take().
#pragma once
#include "dect2/ham/ham_spots.h"
#include "dect2/ham/ham_upload.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2::ham {

// ---- telnet text: lines out of a byte stream (telnet IAC sequences dropped, CR removed) ----
class TelnetLines {
public:
    std::vector<std::string> feed(const char* data, size_t n);
    const std::string& partial() const { return cur_; }   // the text after the last newline (a login prompt has no newline)
    void reset() { cur_.clear(); iac_ = 0; }
private:
    std::string cur_;
    int iac_ = 0;   // bytes of a telnet command still to skip
};
bool telnetAsksCall(const std::string& text);   // "login:", "Please enter your call:", "callsign:" ...

struct SpotFeedConfig {
    std::string myCall;                 // the login of the telnet feeds and the PSK Reporter query; feeds that need it stay off without it
    bool cluster = false;
    std::string clusterHost;            // "dxc.example.org"
    int clusterPort = 7300;
    bool rbn = false;                   // telnet.reversebeacon.net 7000 (CW / RTTY)
    bool rbnDigital = false;            // telnet.reversebeacon.net 7001 (FT8 / FT4)
    bool pota = false, sota = false, pskr = false;
    double potaEverySec = 60, sotaEverySec = 120, pskrEverySec = 300;   // PSK Reporter asks for 5 minutes or more
};

using HttpGet = std::function<UpResponse(const std::string& url)>;
HttpGet curlGet();                      // https only, certificates verified, 2 MB at most, 20 s

std::string potaUrl();                  // https://api.pota.app/spot/activator
std::string sotaUrl();                  // https://api2.sota.org.uk/api/spots/50/all
std::string pskrUrl(const std::string& myCall);   // https://retrieve.pskreporter.info/query?senderCallsign=..&flowStartSeconds=-900&rronly=1

class SpotFeeds {
public:
    explicit SpotFeeds(HttpGet get = curlGet()) : get_(std::move(get)) {}
    ~SpotFeeds() { stop(); }
    // starts the worker when any feed is on (and worker is true), stops it when none is; tests pass worker = false and call pollHttp themselves
    void setConfig(const SpotFeedConfig& c, bool worker = true);
    SpotFeedConfig config() const;
    void stop();
    std::vector<ExtSpot> take();               // the spots that came in since the last call
    std::string status(SpotSrc s) const;       // "connected", "3 spots 12:04", "error: ..." ("" when off)
    bool running() const { return th_.joinable(); }
    // one pass of the HTTP polls (the worker calls it each second; tests call it directly with their own clock)
    void pollHttp(double nowUtc);
private:
    void run();
    void telnetLoop(int which);                 // 0 cluster, 1 RBN, 2 RBN digital
    void setStatus(SpotSrc s, const std::string& t);
    void push(const std::vector<ExtSpot>& v);
    mutable std::mutex mu_;
    SpotFeedConfig cfg_;
    HttpGet get_;
    std::vector<ExtSpot> in_;
    std::string status_[kSpotSrcs];
    double lastPota_ = 0, lastSota_ = 0, lastPskr_ = 0;
    std::atomic<bool> quit_{false};
    std::atomic<int> gen_{0};                  // bumped by setConfig: the telnet loops reconnect with the new settings
    std::thread th_;
};

}  // namespace dect2::ham
