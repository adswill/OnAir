// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Opt-in Discord Rich Presence over the local Discord IPC (no SDK). A background thread owns the connection; the UI only posts the wanted state.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace discord {

struct Presence {
    bool enabled = false;       // false: clear the activity and close the connection
    bool running = false;       // receiver running; otherwise "Idle"
    std::string mode;           // display name of the mode
    std::string freq;           // "93.900 MHz", "File playback", ... (empty: not shown)
    long long startUnix = 0;    // when this mode / frequency session started
    bool operator==(const Presence& o) const { return enabled == o.enabled && running == o.running && mode == o.mode && freq == o.freq && startUnix == o.startUnix; }
};

const char* appId();                                          // kDiscordAppId or $ONAIR_DISCORD_APP_ID; empty when not set
std::string jsonEscape(const std::string& s);                 // the content of a JSON string literal (quotes not included)
std::string encodeFrame(uint32_t opcode, const std::string& json);   // uint32 LE opcode, uint32 LE length, payload
std::string handshakeJson(const std::string& clientId);
std::string activityJson(const Presence& p, int pid, const std::string& nonce);   // the SET_ACTIVITY command (activity null when !enabled)

class Rpc {
public:
    Rpc();
    ~Rpc();                                   // clears the activity, closes, joins
    void set(const Presence& p);              // cheap; never blocks on the network
    void setIntervals(double minSendSec, double retrySec);   // defaults 15 s and 20 s (tests use less)
    void setAppId(const std::string& id);     // default appId()
private:
    void loop();
    std::mutex mu_;
    std::condition_variable cv_;
    Presence want_;
    double minSend_ = 15, retry_ = 20;
    std::string id_;
    bool quit_ = false;
    std::thread th_;
};

}  // namespace discord
