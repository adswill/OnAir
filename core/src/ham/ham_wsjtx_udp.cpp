// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
#include "dect2/ham/ham_wsjtx_udp.h"
#include "../net/netcompat.h"
#include <cstring>
#include <string>
#ifndef _WIN32
#include <netdb.h>
#endif

namespace dect2::ham::wsjtx {
namespace {
void u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void u32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }
void u64(std::vector<uint8_t>& o, uint64_t v) { for (int s = 56; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }
void f64(std::vector<uint8_t>& o, double v) { uint64_t b; std::memcpy(&b, &v, 8); u64(o, b); }
void qstr(std::vector<uint8_t>& o, const std::string& s) { u32(o, (uint32_t)s.size()); o.insert(o.end(), s.begin(), s.end()); }   // QString as utf8: length + bytes
void header(std::vector<uint8_t>& o, MsgType t, const std::string& id) { u32(o, kMagic); u32(o, kSchema); u32(o, t); qstr(o, id); }
}  // namespace

std::vector<uint8_t> encodeHeartbeat(const std::string& id, const std::string& version, const std::string& revision) {
    std::vector<uint8_t> o; header(o, Heartbeat, id); u32(o, kSchema); qstr(o, version); qstr(o, revision); return o;
}
std::vector<uint8_t> encodeDecode(const std::string& id, const DecodeMsg& m) {
    std::vector<uint8_t> o; header(o, Decode, id);
    u8(o, m.isNew); u32(o, m.msSinceMidnight); u32(o, (uint32_t)(int32_t)m.snr); f64(o, m.dt); u32(o, m.df);
    qstr(o, m.mode); qstr(o, m.text); u8(o, m.lowConfidence); u8(o, m.offAir);
    return o;
}
std::vector<uint8_t> encodeStatus(const std::string& id, const StatusMsg& m) {
    std::vector<uint8_t> o; header(o, Status, id);
    u64(o, m.dialHz); qstr(o, m.mode); qstr(o, ""); qstr(o, ""); qstr(o, m.mode);   // dial, mode, DX call, report, Tx mode
    u8(o, 0); u8(o, 0); u8(o, m.decoding); u32(o, m.rxDf); u32(o, m.rxDf);          // Tx enabled, transmitting, decoding, Rx DF, Tx DF
    qstr(o, m.deCall); qstr(o, m.deGrid); qstr(o, "");                               // DE call, DE grid, DX grid
    u8(o, 0); qstr(o, ""); u8(o, m.mode == "FT4"); u8(o, 0); u32(o, 20); u32(o, m.trPeriod); qstr(o, "OnAir"); qstr(o, "");   // watchdog, sub-mode, fast mode, special op, tolerance, T/R period, config name, Tx message
    return o;
}
std::string modeChar(const std::string& n) { return n == "FT8" ? "~" : n == "FT4" ? "+" : ""; }

struct Sender::Impl {
    sock_t s = kBadSock; sockaddr_in to{}; std::string host; int port = 0; std::string err;
    ~Impl() { if (s != kBadSock) sockClose(s); }
};
Sender::Sender() : p_(new Impl) {}
Sender::~Sender() { delete p_; }
const std::string& Sender::error() const { return p_->err; }
bool Sender::configure(const std::string& host, int port) {
    Impl& d = *p_;
    if (d.s != kBadSock && d.host == host && d.port == port) return true;
    if (d.s != kBadSock) { sockClose(d.s); d.s = kBadSock; }
    netInit();
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { d.err = "cannot find " + host; if (res) freeaddrinfo(res); return false; }
    std::memcpy(&d.to, res->ai_addr, sizeof d.to); freeaddrinfo(res);
    d.s = socket(AF_INET, SOCK_DGRAM, 0);
    if (d.s == kBadSock) { d.err = "cannot open a socket"; return false; }
    d.host = host; d.port = port; d.err.clear();
    return true;
}
bool Sender::send(const std::vector<uint8_t>& pkt) {
    Impl& d = *p_;
    if (d.s == kBadSock) return false;
    if (sockSendTo(d.s, pkt.data(), pkt.size(), d.to) < 0) { d.err = "send failed"; return false; }
    d.err.clear();
    return true;
}

}  // namespace dect2::ham::wsjtx
