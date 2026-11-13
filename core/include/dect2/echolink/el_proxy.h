// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EchoLink proxy (relay) protocol: for a station whose UDP ports 5198 / 5199 cannot be reached (behind NAT, a firewall or a carrier that
// blocks them), every EchoLink packet and the directory connection go through one TCP connection to an EchoLink proxy (default port 8100),
// which sends and receives the UDP packets for it. The layout follows SvxLink's echolib EchoLinkProxy.cpp (GPL-2.0-or-later, Tobias
// Blomberg SM0SVX; see THIRD_PARTY.md). The code is OnAir's own and is checked against a mock proxy only, never a real one.
//
//   connect:  the proxy sends an 8-byte nonce. The client answers CALLSIGN "\n" MD5(PASSWORD + nonce) (16 raw bytes). PASSWORD is the
//             proxy's password in upper case; an empty one means a public proxy, whose password by convention is "PUBLIC".
//   then:     message blocks both ways: type (1 byte), the remote IPv4 address (4 bytes, in network order), the data length (4 bytes, little
//             endian), the data. Types: 1 TCP_OPEN (client: open a TCP connection to the address, port 5200, the directory), 2 TCP_DATA,
//             3 TCP_CLOSE (no data), 4 TCP_STATUS (proxy: 4-byte little-endian status, 0 = the TCP connection is open), 5 UDP_DATA (the
//             audio port, 5198), 6 UDP_CONTROL (the control port, 5199), 7 SYSTEM (proxy: one byte, 1 bad password, 2 access denied).
//             The proxy always uses the standard ports at the far end, so no port is carried.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace echolink {

constexpr int kProxyPort = 8100;
constexpr int kProxyNonceBytes = 8;
constexpr int kProxyHeaderBytes = 9;
constexpr uint32_t kProxyMaxBlock = 16384;   // SvxLink's receive buffer: a longer block is a broken stream

enum class ProxyType : uint8_t { TcpOpen = 1, TcpData = 2, TcpClose = 3, TcpStatus = 4, UdpData = 5, UdpControl = 6, System = 7 };
constexpr uint8_t kProxyBadPassword = 1;
constexpr uint8_t kProxyAccessDenied = 2;

// CALL "\n" MD5(upper(password or "PUBLIC") + nonce)
std::vector<uint8_t> proxyAuthReply(const std::string& call, const std::string& password, const uint8_t nonce[kProxyNonceBytes]);
// ip: dotted IPv4, "" for none (0.0.0.0)
std::vector<uint8_t> makeProxyBlock(ProxyType t, const std::string& ip, const uint8_t* data = nullptr, size_t n = 0);
bool ipv4Bytes(const std::string& ip, uint8_t out[4]);
std::string ipv4Text(const uint8_t b[4]);

struct ProxyBlock {
    ProxyType type = ProxyType::System;
    std::string ip;
    std::vector<uint8_t> data;
};

// Reads the proxy's stream in any chunk sizes: first the nonce (nonceReady()), then blocks. failed() on an unknown type or an oversized block.
class ProxyParser {
public:
    void reset();
    void feed(const uint8_t* p, size_t n);
    bool nonceReady() const { return haveNonce_; }
    const uint8_t* nonce() const { return nonce_; }
    bool next(ProxyBlock& out);              // the next whole block, false when none is complete
    bool failed() const { return failed_; }
private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    bool haveNonce_ = false, failed_ = false;
    uint8_t nonce_[kProxyNonceBytes] = {};
};

const char* proxySystemText(uint8_t code);   // "bad proxy password", "access to the proxy denied", "unknown proxy message"

} // namespace echolink
} // namespace dect2
