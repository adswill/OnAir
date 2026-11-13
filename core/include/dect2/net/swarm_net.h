// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LAN swarm discovery over UDP multicast (see swarm.h for the rules). Opt-in: nothing is opened until start(). Announces go to 239.255.77.31:47831
// with TTL 1 (a router never forwards them), sent only on private IPv4 interfaces, and received packets are accepted only from a private address on
// the same subnet as one of this computer's interfaces. Carries the Announce text only: no IQ, no audio. POSIX only for now (Windows returns an error).
#pragma once
#include "dect2/net/swarm.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::swarm {

constexpr const char* kGroup = "239.255.77.31";
constexpr int kPort = 47831;

struct IfaceAddr { uint32_t ip = 0, mask = 0; };   // host order
std::vector<IfaceAddr> privateInterfaces();       // up, multicast-capable, non-loopback IPv4 interfaces with a private address (never a public one)

struct NetStats {
    bool running = false;
    uint64_t sent = 0, received = 0, rejected = 0;
    std::string lastError;
    int interfaces = 0;   // private IPv4 interfaces in use
};

class SwarmNet {
public:
    using AnnounceFn = std::function<Announce()>;   // called about every 5 s from the worker thread; must be thread-safe
    SwarmNet();
    ~SwarmNet();
    // Starts announcing and listening. False with err set when no private interface exists or the sockets fail. Calling it again restarts.
    bool start(const std::string& ownId, AnnounceFn announce, std::string& err);
    void stop();
    std::vector<Node> nodes();   // the swarm view; entries expire 30 s after their last announce
    NetStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}   // namespace dect2::swarm
