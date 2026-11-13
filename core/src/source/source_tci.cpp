// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TCI (Transceiver Control Interface of Expert Electronics' ExpertSDR3, spec 1.9 to 2.1; also served by SunSDR and other programs), receive only. A WebSocket
// connection (ws://host:50001): text messages hold commands "name:arg,arg;" (several per message), binary messages hold stream blocks of a 64-byte
// header (16 little-endian uint32: receiver, sample_rate, format, codec, crc, length, type, channels, reserv[8]) and the samples. After connecting the
// server sends its initialization commands (device, protocol, vfo_limits, trx_count, ..., ready). We ask for iq_samplerate, put dds (and vfo) on the
// centre, and send iq_start; type 0 blocks carry the IQ of that receiver. Only these are ever sent: iq_samplerate, dds, vfo, iq_start, iq_stop, and start (once, only when the server reports "stop;": ExpertSDR3 sends no IQ while its radio is off). No
// transmit command (trx, tune, drive, ...) and no audio stream command is sent.
// Verified from the public spec: the command names and arguments, the header, the stream/sample types, the IQ rates 48/96/192/384 kHz, the default port.
// Assumed (marked ASSUMED): the URL path "/", length being the real-sample count (the payload size is used instead), the VFO following the DDS being
// accepted when it stays in the panorama, no RF gain existing (TCI has none for the receiver: only the AGC, which stays the server's).
#include "../net/tcp_client.h"
#include "dect2/common/diag.h"
#include "../net/ws_client.h"
#include "native_common.h"
#include "dect2/net/netradio.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace dect2 {
namespace native {

namespace {

constexpr int kConnectMs = 3000, kStallMs = 5000;
constexpr size_t kMaxMessage = 1u << 20;
constexpr const char* kPath = "/";   // ASSUMED: the spec names no path
constexpr size_t kHeader = 64;
const double kRates[] = {48000, 96000, 192000, 384000};

std::string lowerStr(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

std::string trimStr(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\r' || s[b] == '\n' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == '\t')) e--;
    return s.substr(b, e - b);
}

bool splitHostPort(const std::string& a, std::string& host, int& port) {
    const size_t c = a.rfind(':');
    if (c == std::string::npos || c == 0) return false;
    host = a.substr(0, c);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    port = atoi(a.c_str() + c + 1);
    return port > 0 && port < 65536;
}

uint32_t rd32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// what the server said about itself in its initialization commands
struct TciInfo {
    bool ready = false, haveProto = false, receiveOnly = false;
    std::string device, program, version;
    double vfoLo = 0, vfoHi = 0;
    int trx = 1;
    double iqRate = 0;   // the rate the server confirmed
    bool stopped = false;   // the server reported "stop;" (its radio is switched off: ExpertSDR3 sends no IQ then)
};
std::mutex gInfoMu;
std::map<std::string, TciInfo> gInfo;
TciInfo infoFor(const std::string& addr) { std::lock_guard<std::mutex> lk(gInfoMu); auto it = gInfo.find(addr); return it == gInfo.end() ? TciInfo{} : it->second; }
void noteInfo(const std::string& addr, const TciInfo& i) { std::lock_guard<std::mutex> lk(gInfoMu); gInfo[addr] = i; }

typedef std::vector<std::pair<std::string, std::vector<std::string>>> Cmds;

void absorb(const Cmds& cs, TciInfo& i) {
    for (const auto& c : cs) {
        const std::string& n = c.first;
        const auto& a = c.second;
        if (n == "ready") i.ready = true;
        else if (n == "device" && !a.empty()) i.device = a[0];
        else if (n == "protocol" && !a.empty()) { i.program = a[0]; i.version = a.size() > 1 ? a[1] : ""; i.haveProto = true; }
        else if (n == "vfo_limits" && a.size() >= 2) { i.vfoLo = atof(a[0].c_str()); i.vfoHi = atof(a[1].c_str()); }
        else if (n == "trx_count" && !a.empty()) i.trx = std::min(8, std::max(1, atoi(a[0].c_str())));
        else if (n == "receive_only" && !a.empty()) i.receiveOnly = lowerStr(a[0]) == "true";
        else if (n == "stop") i.stopped = true;
        else if (n == "start") i.stopped = false;
        else if (n == "iq_samplerate" && !a.empty()) { const double r = atof(a[0].c_str()); if (r > 0 && r < 1e8) i.iqRate = r; }
    }
}

std::string addrOf(const std::string& host, int port) { return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port); }

// connection, upgrade, then the initialization commands up to "ready"
bool connectAndAsk(WsClient& ws, const std::string& host, int port, int timeoutMs, TciInfo& info, std::string& err) {
    const std::string addr = addrOf(host, port);
    const sock_t s = tcpConnect(host, port, timeoutMs, err);
    if (s == kBadSock) { DIAG_ERROR(975921, "Connecting to the TCI server at %s failed (%s), so the source stays closed.", addr.c_str(), err.c_str()); err = "cannot connect to a TCI server at " + addr + ": " + err +
        " (the TCI server is a part of ExpertSDR3 on the computer the SunSDR is connected to, not of the SunSDR itself: start ExpertSDR3, switch TCI on in its settings"
        " and, to reach it from another computer, set its TCI address to 0.0.0.0 instead of 127.0.0.1)"; return false; }
    ws.attach(s);
    sockRecvTimeout(s, timeoutMs);
    std::string e;
    if (!ws.handshake(host, port, kPath, e)) { DIAG_ERROR(348271, "The WebSocket handshake with the TCI server at %s failed (%s), so the source stays closed.", addr.c_str(), e.c_str()); err = addr + ": " + e; return false; }
    std::vector<unsigned char> m;
    Cmds cs;
    for (int k = 0; k < 256 && !info.ready; k++) {
        const auto kind = ws.readMessage(m, kMaxMessage);
        if (kind == WsClient::Kind::Timeout) break;
        if (kind == WsClient::Kind::Closed || kind == WsClient::Kind::Bad) { DIAG_ERROR(886132, "The TCI server at %s closed the connection or sent bad data during start-up, so the source stays closed.", addr.c_str()); err = addr + " closed the connection or sent bad data"; return false; }
        if (kind == WsClient::Kind::Text && tciParseCommands(std::string(m.begin(), m.end()), cs)) absorb(cs, info);
    }
    if (!info.ready && !info.haveProto) { DIAG_ERROR(953443, "The server at %s answered but not as a TCI server, so the source stays closed (is it the TCI port, default 50001?).", addr.c_str()); err = addr + " answered, but not as a TCI server (no protocol or ready command); is it the TCI port (default 50001)?"; return false; }
    return true;
}

void closeWs(WsClient& ws) {
    if (ws.sock() == kBadSock) return;
    // A graceful end: the commands sent last (iq_stop) must reach the server. Closing a socket that still has unread IQ in its receive queue
    // makes Windows send a reset, and the reset throws away what was sent but not yet read by the server (iq_stop never arrived there).
    // So: the close frame, the send side shut, and the incoming data read away for a moment until the server ends the stream or goes quiet.
    ws.sendClose();
#ifdef _WIN32
    shutdown(ws.sock(), SD_SEND);
#else
    shutdown(ws.sock(), SHUT_WR);
#endif
    sockRecvTimeout(ws.sock(), 100);
    {
        char junk[8192];
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400) && sockRecv(ws.sock(), junk, sizeof junk) > 0) {}
    }
    sockClose(ws.sock());
    ws.attach(kBadSock);
}

} // namespace

bool tciParseCommands(const std::string& text, Cmds& out) {
    out.clear();
    size_t p = 0;
    for (;;) {
        const size_t e = text.find(';', p);
        if (e == std::string::npos) break;
        const std::string c = trimStr(text.substr(p, e - p));
        p = e + 1;
        if (c.empty()) continue;
        const size_t col = c.find(':');
        std::string name = lowerStr(col == std::string::npos ? c : c.substr(0, col));
        std::vector<std::string> args;
        if (col != std::string::npos) {
            size_t q = col + 1;
            for (;;) {
                size_t k = c.find(',', q);
                if (k == std::string::npos) k = c.size();
                args.push_back(trimStr(c.substr(q, k - q)));
                if (k >= c.size()) break;
                q = k + 1;
            }
        }
        if (name.empty() || name.size() > 64 || args.size() > 64) continue;
        out.push_back({name, args});
    }
    return !out.empty();
}

bool tciProbe(const std::string& host, int port, int timeoutMs, std::string& err) {
    WsClient ws;
    TciInfo info;
    const bool ok = connectAndAsk(ws, host, port, timeoutMs, info, err);
    if (ok) noteInfo(addrOf(host, port), info);
    closeWs(ws);
    return ok;
}

class TciSource : public NativeSource {
public:
    explicit TciSource(std::string addr) : addr_(std::move(addr)) {}
    ~TciSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::string host;
        int port = 0;
        if (!splitHostPort(addr_, host, port)) { DIAG_ERROR(543843, "The TCI address %s is not in host:port form, so no connection is tried.", addr_.c_str()); err = "TCI: bad address " + addr_ + " (host:port)"; openFailure_ = OpenFailure::Final; return false; }
        TciInfo info;
        std::string e;
        if (!connectAndAsk(ws_, host, port, kConnectMs, info, e)) { DIAG_ERROR(457869, "Connecting to the TCI server %s failed (%s), so the source stays closed.", addr_.c_str(), e.c_str()); err = "TCI: " + e; openFailure_ = OpenFailure::Retry; return false; }
        noteInfo(addr_, info);
        { std::lock_guard<std::mutex> lk(infoMu_); info_ = info; }
        sentFreq_ = sentRate_ = -1;
        const int rx = std::atoi(radioOption(s, "receiver", "0").c_str());
        rx_ = std::min(std::max(rx, 0), std::max(info.trx - 1, 0));
        sockRecvTimeout(ws_.sock(), kStallMs);
        fprintf(stderr, "TCI: connected to %s at %s (%s %s)\n", info.device.empty() ? "a server" : info.device.c_str(), addr_.c_str(), info.program.c_str(), info.version.c_str());
        fflush(stderr);
        if (lostAt_.time_since_epoch().count() != 0) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - lostAt_).count();
            const uint64_t n = (uint64_t)std::llround(sec * (rate_ > 0 ? rate_ : 0));
            if (ring_) ring_->noteLost(n);
            fprintf(stderr, "TCI: reconnected after %.1f s, about %llu samples lost\n", sec, (unsigned long long)n);
            fflush(stderr);
            lostAt_ = {};
        }
        if (!configure(s, err, false)) return false;
        if (!ws_.sendText("iq_start:" + std::to_string(rx_) + ";")) { err = "TCI: the connection to " + addr_ + " dropped"; return false; }
        return true;
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        if (ws_.sock() == kBadSock) return false;
        TciInfo i;
        { std::lock_guard<std::mutex> lk(infoMu_); i = info_; }
        std::string warn;
        bool ok = true;
        double rate = kRates[3];   // the smallest one at or above the request, else the largest
        for (double r : kRates) if (r >= (s.sampleRate > 0 ? s.sampleRate : 0) - 1) { rate = r; break; }
        if ((long long)rate != sentRate_) { ok = ok && ws_.sendText("iq_samplerate:" + std::to_string((long long)rate) + ";"); sentRate_ = (long long)rate; }
        // the panorama's centre; the VFO is put on it, too, so it stays inside the band
        const long long hz = std::llround(std::max(s.centerHz, 0.0));
        if (hz != sentFreq_) {
            ok = ok && ws_.sendText("dds:" + std::to_string(rx_) + "," + std::to_string(hz) + ";");
            ok = ok && ws_.sendText("vfo:" + std::to_string(rx_) + ",0," + std::to_string(hz) + ";");
            sentFreq_ = hz;
        }
        if (!ok) { err = "TCI: the connection to " + addr_ + " dropped"; return false; }
        rate_ = rate;   // until the server names the rate it really runs (iq_samplerate, or the block header)
        requested_ = s.sampleRate;
        if (i.vfoHi > 0 && (s.centerHz < i.vfoLo || s.centerHz > i.vfoHi)) addNote(warn, freqSpecNote("TCI", s.centerHz, i.vfoLo, i.vfoHi));
        if (!warn.empty()) err = warn;
        else checkRate(s.sampleRate, err);
        return true;
    }
    void closeDevice() override {
        if (ws_.sock() != kBadSock) ws_.sendText("iq_stop:" + std::to_string(rx_) + ";");
        closeWs(ws_);
    }
    bool restartForRate() const override { return false; }   // iq_samplerate is accepted while streaming
    void streamLoop() override {
        std::vector<unsigned char> m;
        Cmds cs;
        uint64_t bad = 0;
        bool gotIq = false;
        startSent_ = false;
        while (run_) {
            const auto kind = ws_.readMessage(m, kMaxMessage);
            if (kind == WsClient::Kind::Timeout && !gotIq) { fprintf(stderr, "TCI: connected to %s but no IQ arrived in %d s (is the radio switched on in ExpertSDR3, and is the receiver number right?)\n", addr_.c_str(), kStallMs / 1000); fflush(stderr); }
            if (kind == WsClient::Kind::Closed || kind == WsClient::Kind::Timeout) break;
            if (kind == WsClient::Kind::Bad) { DIAG(935324, "The TCI server %s sent bad data, so the connection is dropped.", addr_.c_str()); fprintf(stderr, "TCI: bad data from %s, dropping the connection\n", addr_.c_str()); fflush(stderr); break; }
            if (kind == WsClient::Kind::Text) {
                if (!tciParseCommands(std::string(m.begin(), m.end()), cs)) continue;
                TciInfo i;
                { std::lock_guard<std::mutex> lk(infoMu_); absorb(cs, info_); i = info_; }
                if (i.iqRate > 0 && std::fabs(i.iqRate - rate_) > 0.5) rate_ = i.iqRate;
                // ExpertSDR3 sends IQ only while its radio runs ("start;"). It reports "stop;" when the radio is switched off: start it, once per
                // connection (a receive-only command; whoever stopped it on purpose can stop it again)
                if (i.stopped && !startSent_ && !gotIq) {
                    startSent_ = true;
                    ws_.sendText("start;");
                    fprintf(stderr, "TCI: the server's radio is stopped, sending start\n");
                    fflush(stderr);
                }
                continue;
            }
            // binary: the 64-byte header, then the samples; only the IQ stream (type 0) of the receiver we started
            if (m.size() < kHeader) { bad++; continue; }
            const uint32_t recv = rd32(&m[0]), srate = rd32(&m[4]), fmt = rd32(&m[8]), length = rd32(&m[20]), type = rd32(&m[24]), ch = rd32(&m[28]);
            if (type != 0 || recv != (uint32_t)rx_) continue;
            if ((ch != 2 && ch != 0) || fmt > 3) { bad++; continue; }   // channels 0: a server that leaves it out (IQ is always two)
            const size_t bps = fmt == 0 ? 2 : fmt == 1 ? 3 : 4;
            const size_t payload = m.size() - kHeader;
            const size_t n = payload / (bps * 2);
            if (n == 0 || payload % (bps * 2) || (length != 0 && (size_t)length * bps != payload && (size_t)length * bps * 2 != payload)) { bad++; continue; }
            if (srate >= 8000 && srate < 1e8 && std::fabs(srate - rate_) > 0.5) rate_ = srate;
            conv_.resize(n);
            const unsigned char* b = &m[kHeader];
            for (size_t k = 0; k < n; k++) {
                float v[2];
                for (int c = 0; c < 2; c++) {
                    const unsigned char* q = b + (2 * k + c) * bps;
                    if (fmt == 0) v[c] = (int16_t)(q[0] | q[1] << 8) / 32768.f;
                    else if (fmt == 1) v[c] = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) / 2147483648.f;   // sign comes from the top byte
                    else if (fmt == 2) v[c] = (int32_t)rd32(q) / 2147483648.f;
                    else { const uint32_t u = rd32(q); float f; memcpy(&f, &u, 4); v[c] = std::isfinite(f) ? f : 0.f; }
                }
                conv_[k] = cf32(v[0], v[1]);
            }
            if (!gotIq) { gotIq = true; fprintf(stderr, "TCI: IQ is arriving (%u Hz, format %u)\n", srate, fmt); fflush(stderr); }
            push(conv_.data(), n);
        }
        if (bad) { fprintf(stderr, "TCI: %llu malformed or unusable blocks ignored\n", (unsigned long long)bad); fflush(stderr); }
        if (run_) {
             DIAG(966498, "The connection to the TCI server at %s was lost, so the source tries to reconnect.", addr_.c_str());
             fprintf(stderr, "TCI: lost the connection to the server at %s\n", addr_.c_str());
            fflush(stderr);
            lostAt_ = std::chrono::steady_clock::now();
        }
    }
    void interrupt() override {   /* read side only: closeDevice still has to send iq_stop */
        if (ws_.sock() == kBadSock) return;
#ifdef _WIN32
        sockRecvTimeout(ws_.sock(), 50);   // Windows answers data that arrives after shutdown(SD_RECEIVE) with a reset: that would cut the connection before iq_stop is sent. A short receive timeout ends the read loop instead
#else
        sockShutdownRead(ws_.sock());
#endif
    }

private:
    std::string addr_;
    WsClient ws_;
    std::mutex infoMu_;
    TciInfo info_;
    int rx_ = 0;
    bool startSent_ = false;
    long long sentFreq_ = -1, sentRate_ = -1;
    std::chrono::steady_clock::time_point lostAt_{};
};

static void setCaps(DeviceInfo& d, const TciInfo& i) {
    d.minRateHz = kRates[0];
    d.maxRateHz = kRates[3];
    d.rateRanges.clear();
    for (double r : kRates) d.rateRanges.push_back({r, r});
    d.minFreqHz = i.vfoHi > 0 ? i.vfoLo : 1e4;   // from vfo_limits when the server told them
    d.maxFreqHz = i.vfoHi > 0 ? i.vfoHi : 6e9;
    d.gainMinDb = d.gainMaxDb = 0;   // TCI has no receiver RF gain (only the server's AGC)
    d.settings.clear();
    if (i.trx > 1) {
        std::vector<std::string> v, n;
        for (int k = 0; k < i.trx; k++) { v.push_back(std::to_string(k)); n.push_back("Receiver " + std::to_string(k + 1)); }
        d.settings.push_back(choiceSetting("receiver", "Receiver", "Which of the server's receivers to take the IQ of.", v, n, "0", true));
    }
}

DeviceInfo tciDevice(const std::string& host, int port) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "tci";
    d.nativeArgs = addrOf(host, port);
    d.serial = d.nativeArgs;
    d.name = "TCI " + d.nativeArgs + " (network)";
    setCaps(d, infoFor(d.nativeArgs));
    return d;
}

void tciCaps(DeviceInfo& d) { setCaps(d, infoFor(d.nativeArgs)); }

std::unique_ptr<IqSource> makeTci(const DeviceInfo& d) { return std::make_unique<TciSource>(d.nativeArgs); }

} // namespace native
} // namespace dect2
