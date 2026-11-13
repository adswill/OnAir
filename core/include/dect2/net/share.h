// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Share my radio": an rtl_tcp-compatible server inside OnAir. Whatever the engine reads from its radio goes out as unsigned 8-bit IQ to every
// client that connects (a second OnAir through its rtl_tcp network radio, SDR#, SDR++, GQRX, ...). Off until the user starts it. By default only
// this computer can connect; "LAN" opens it to the network. The clients' frequency / gain commands are only handed on when allowControl is set.
// The server streams the samples at the rate the radio runs at; a client's sample-rate command is accepted and ignored.
#pragma once
#include "dect2/common/ring.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct ShareConfig {
    bool on = false;
    int port = 1234;             // 0: any free port (tests); config() then tells which
    bool lan = false;            // other computers may connect (else 127.0.0.1 only)
    bool allowControl = false;   // frequency / gain / bias-tee commands of the clients are carried out
};

struct ShareCommand { uint8_t cmd = 0; uint32_t value = 0; };   // an rtl_tcp command: 0x01 frequency (Hz), 0x04 gain (tenths of dB), 0x0e bias tee, ...

struct ShareStatus {
    bool listening = false;
    std::vector<std::string> clients;   // "ip:port" of every connected client
    uint64_t sentBytes = 0;
    uint64_t droppedBytes = 0;          // samples thrown away because a client was too slow
    std::string lastError;
};

class ShareServer {
public:
    static ShareServer& get();
    ~ShareServer();
    void configure(const ShareConfig& c);   // starts, restarts or stops the server as the config says
    ShareConfig config() const;
    // The engine thread hands over the samples it read from the radio (a no-op while sharing is off and while nobody is connected).
    void offer(const cf32* x, size_t n);
    // The commands the clients sent since the last call (only filled while allowControl is on)
    std::vector<ShareCommand> takeCommands();
    ShareStatus status() const;
    bool active() const { return active_.load(std::memory_order_relaxed); }
private:
    ShareServer();
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<bool> active_{false};
};

// the rtl_tcp samples: IQ pair -> two unsigned bytes (what an RTL-SDR gives; the rtl_tcp client reads them back as (b - 127.4) / 128)
void shareToU8(const cf32* x, size_t n, std::vector<unsigned char>& out);

} // namespace dect2
