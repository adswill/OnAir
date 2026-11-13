// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The two small network outputs of the remote API besides HTTP: a rigctl (Hamlib rigctld subset) TCP server and the UDP broadcast of decoded messages.
// Both are off until configured. The rigctl port listens on 127.0.0.1 unless allowLan is set (the protocol has no login, so allowLan is an explicit
// choice for a trusted network); the UDP output sends one JSON line per datagram (see docs/REMOTE-API.md) to the address given, never anywhere else.
#pragma once
#include "dect2/net/api_policy.h"
#include <memory>
#include <string>

namespace dect2 {

// token: commands that change the radio need "\auth <token>" first. Needed (and refused when empty) whenever allowLan is on; on the loopback
// the HTTP guard is the protection and no token is asked for. Reads stay open, as in rigctld.
struct RigctlConfig { bool on = false; int port = 4532; bool allowLan = false; std::string token; };

class RigctlServer {
public:
    explicit RigctlServer(apipol::RigctlRadio& radio);
    ~RigctlServer();
    bool configure(const RigctlConfig& c);   // false: cannot listen (error())
    int port() const;                        // the port in use (0 asks for any free port)
    std::string error() const;
    int clients() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct UdpOutConfig { bool on = false; std::string host = "255.255.255.255"; int port = 0; };   // host: a dotted IPv4 address

class UdpOut {
public:
    UdpOut();
    ~UdpOut();
    bool configure(const UdpOutConfig& c);   // false: bad address or port, or no socket (error())
    bool send(const std::string& line);      // one datagram, at most 1400 bytes (longer: false); a newline is added
    std::string error() const;
    uint64_t sent() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

bool validUdpHost(const std::string& host);   // dotted IPv4, nothing else

} // namespace dect2
