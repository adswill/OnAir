// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN receive-only packet forwarder: see lorawan_fwd.h.
#include "dect2/mesh/lorawan_fwd.h"
#include "dect2/mesh/mesh_crypto.h"
#include "../net/tcp_client.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <random>

namespace dect2 {

bool semtechIsUplink(const MeshPacket& p) { return p.protocol == 5 && p.crcOk && (p.mtype == 0 || p.mtype == 2 || p.mtype == 4) && p.raw.size() >= 2; }

bool semtechParseEui(const std::string& s, uint8_t eui[8]) {
    std::string h;
    for (char c : s) if (c != ':' && c != '-' && c != ' ') h += c;
    if (h.size() != 16) return false;
    for (int i = 0; i < 8; i++) {
        char* e = nullptr;
        const std::string b = h.substr((size_t)i * 2, 2);
        const unsigned long v = std::strtoul(b.c_str(), &e, 16);
        if (!e || *e) return false;
        eui[i] = (uint8_t)v;
    }
    return true;
}

std::string semtechRxpk(const MeshPacket& p, int64_t us, uint32_t tmst, int chan) {
    std::vector<uint8_t> bytes;
    meshcrypto::hexDecode(p.raw, bytes);
    const time_t t = (time_t)(us / 1000000);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char ts[48];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, (int)(us % 1000000));
    char b[512];
    std::snprintf(b, sizeof b, "{\"time\":\"%s\",\"tmst\":%u,\"chan\":%d,\"rfch\":0,\"freq\":%.6f,\"stat\":1,\"modu\":\"LORA\",\"datr\":\"SF%dBW%d\",\"codr\":\"4/%d\",\"rssi\":%d,\"lsnr\":%.1f,\"size\":%zu,\"data\":\"",
                  ts, tmst, chan, p.freqHz / 1e6, p.sf, (int)std::lround(p.bwHz / 1000), p.cr >= 5 && p.cr <= 8 ? p.cr : 5, (int)std::lround(p.levelDb), p.snrDb, bytes.size());
    return std::string(b) + meshcrypto::base64Encode(bytes.data(), bytes.size()) + "\"}";
}

static std::vector<uint8_t> hdr(uint16_t token, uint8_t ident, const uint8_t eui[8]) {
    std::vector<uint8_t> o = {2, (uint8_t)(token >> 8), (uint8_t)(token & 0xff), ident};
    o.insert(o.end(), eui, eui + 8);
    return o;
}
std::vector<uint8_t> semtechPushData(uint16_t token, const uint8_t eui[8], const std::string& json) { auto o = hdr(token, 0x00, eui); o.insert(o.end(), json.begin(), json.end()); return o; }
std::vector<uint8_t> semtechPullData(uint16_t token, const uint8_t eui[8]) { return hdr(token, 0x02, eui); }

struct SemtechForwarder::Impl {
    std::mutex mu;
    sock_t s = kBadSock;
    sockaddr_in to{};
    uint8_t eui[8] = {};
    SemtechStats st;
    double lastPull = -1e9;
    std::mt19937 rng{std::random_device{}()};
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    void close() { if (s != kBadSock) sockClose(s); s = kBadSock; }
    void send(const std::vector<uint8_t>& b) { if (s != kBadSock) sendto(s, (const char*)b.data(), (int)b.size(), 0, (const sockaddr*)&to, sizeof to); }
    void readAcks() {
        if (s == kBadSock) return;
        char b[1024];
        for (int i = 0; i < 16; i++) {
            const long long n = recv(s, b, sizeof b, 0);
            if (n < 4) break;
            if (b[0] == 2 && b[3] == 0x01) st.acked++;
            else if (b[0] == 2 && b[3] == 0x04) st.pullAcks++;
            // 0x03 PULL_RESP (a downlink to send) is ignored: this forwarder never transmits
        }
    }
};

SemtechForwarder::SemtechForwarder() : p_(std::make_unique<Impl>()) {}
SemtechForwarder::~SemtechForwarder() { p_->close(); }

bool SemtechForwarder::configure(const std::string& host, int port, const std::string& euiText, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->close();
    p_->st.error.clear();
    if (host.empty()) return true;
    if (!semtechParseEui(euiText, p_->eui)) { err = p_->st.error = "the gateway EUI must be 16 hex digits"; return false; }
    netInit();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (port <= 0 || port > 65535 || getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (res) freeaddrinfo(res);
        err = p_->st.error = "cannot find " + host; return false;
    }
    std::memcpy(&p_->to, res->ai_addr, sizeof p_->to);
    freeaddrinfo(res);
    p_->s = socket(AF_INET, SOCK_DGRAM, 0);
    if (p_->s == kBadSock) { err = p_->st.error = "cannot open a socket"; return false; }
    sockSetNonBlocking(p_->s, true);
    p_->lastPull = -1e9;
    return true;
}

void SemtechForwarder::forward(const MeshPacket& p) {
    if (!semtechIsUplink(p)) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->s == kBadSock) return;
    const int64_t us = p.unixTime > 0 ? (int64_t)std::llround(p.unixTime * 1e6) : (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const uint32_t tmst = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - p_->t0).count();
    p_->send(semtechPushData((uint16_t)p_->rng(), p_->eui, "{\"rxpk\":[" + semtechRxpk(p, us, tmst) + "]}"));
    p_->st.pushed++;
}

void SemtechForwarder::poll(double now) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->s == kBadSock) return;
    if (now - p_->lastPull >= 30) { p_->lastPull = now; p_->send(semtechPullData((uint16_t)p_->rng(), p_->eui)); p_->st.pulls++; }
    p_->readAcks();
}

SemtechStats SemtechForwarder::stats() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }

}  // namespace dect2
