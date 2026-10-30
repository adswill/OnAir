#include "dect2/integrations/mqtt.h"
#include "../net/tcp_client.h"
#include "../net/tls_conn.h"
#include <chrono>

namespace dect2::integrations::mqtt {

namespace {
void putStr(std::string& o, const std::string& s) {
    const size_t n = s.size() > 65535 ? 65535 : s.size();
    o += char(n >> 8);
    o += char(n & 0xff);
    o.append(s, 0, n);
}
bool getStr(const std::string& b, size_t& pos, std::string& s) {
    if (pos + 2 > b.size()) return false;
    const size_t n = ((uint8_t)b[pos] << 8) | (uint8_t)b[pos + 1];
    pos += 2;
    if (pos + n > b.size()) return false;
    s = b.substr(pos, n);
    pos += n;
    return true;
}
std::string frame(uint8_t type, uint8_t flags, const std::string& body) {
    std::string o;
    o += char((type << 4) | (flags & 0x0f));
    putVarInt(o, (uint32_t)body.size());
    o += body;
    return o;
}
}  // namespace

void putVarInt(std::string& out, uint32_t v) {
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        out += char(b);
    } while (v);
}

int getVarInt(const uint8_t* p, size_t n, uint32_t& v) {
    v = 0;
    for (size_t i = 0; i < 4; i++) {
        if (i >= n) return 0;
        v |= (uint32_t)(p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) return (int)i + 1;
    }
    return -1;
}

int parsePacket(const std::string& buf, Packet& out) {
    if (buf.size() < 2) return 0;
    const uint8_t b0 = (uint8_t)buf[0];
    const uint8_t type = b0 >> 4;
    uint32_t len = 0;
    const int k = getVarInt((const uint8_t*)buf.data() + 1, buf.size() - 1, len);
    if (k < 0) return -1;
    if (k == 0) return 0;
    if (type == 0 || type == 15) return -1;
    const size_t total = 1 + (size_t)k + len;
    if (buf.size() < total) return 0;
    out.type = type;
    out.flags = b0 & 0x0f;
    out.body = buf.substr(1 + (size_t)k, len);
    return (int)total;
}

std::string encodeConnect(const ConnectOpts& o) {
    std::string b;
    putStr(b, "MQTT");
    b += char(4);
    uint8_t fl = 0;
    if (o.cleanSession) fl |= 0x02;
    if (!o.willTopic.empty()) { fl |= 0x04; if (o.willRetain) fl |= 0x20; }
    if (!o.user.empty()) { fl |= 0x80; if (!o.password.empty()) fl |= 0x40; }
    b += char(fl);
    b += char(o.keepAlive >> 8);
    b += char(o.keepAlive & 0xff);
    putStr(b, o.clientId);
    if (!o.willTopic.empty()) { putStr(b, o.willTopic); putStr(b, o.willPayload); }
    if (!o.user.empty()) { putStr(b, o.user); if (!o.password.empty()) putStr(b, o.password); }
    return frame(CONNECT, 0, b);
}

bool decodeConnect(const std::string& b, ConnectOpts& o) {
    size_t pos = 0;
    std::string proto;
    if (!getStr(b, pos, proto) || proto != "MQTT" || pos + 4 > b.size()) return false;
    if (b[pos++] != 4) return false;
    const uint8_t fl = (uint8_t)b[pos++];
    o.keepAlive = (uint16_t)(((uint8_t)b[pos] << 8) | (uint8_t)b[pos + 1]);
    pos += 2;
    o.cleanSession = (fl & 0x02) != 0;
    if (!getStr(b, pos, o.clientId)) return false;
    o.willTopic.clear(); o.willPayload.clear(); o.user.clear(); o.password.clear();
    o.willRetain = (fl & 0x20) != 0;
    if (fl & 0x04) { if (!getStr(b, pos, o.willTopic) || !getStr(b, pos, o.willPayload)) return false; }
    if (fl & 0x80) { if (!getStr(b, pos, o.user)) return false; }
    if (fl & 0x40) { if (!getStr(b, pos, o.password)) return false; }
    return pos == b.size();
}

std::string encodeConnack(bool sp, uint8_t rc) { return frame(CONNACK, 0, std::string{char(sp ? 1 : 0), char(rc)}); }
bool decodeConnack(const std::string& b, bool& sp, uint8_t& rc) {
    if (b.size() != 2) return false;
    sp = (b[0] & 1) != 0;
    rc = (uint8_t)b[1];
    return true;
}

std::string encodePublish(const Publish& p) {
    std::string b;
    putStr(b, p.topic);
    if (p.qos > 0) { b += char(p.pid >> 8); b += char(p.pid & 0xff); }
    b += p.payload;
    return frame(PUBLISH, (uint8_t)((p.dup ? 8 : 0) | ((p.qos & 3) << 1) | (p.retain ? 1 : 0)), b);
}

bool decodePublish(const Packet& pk, Publish& p) {
    if (pk.type != PUBLISH) return false;
    p.dup = (pk.flags & 8) != 0;
    p.qos = (pk.flags >> 1) & 3;
    p.retain = (pk.flags & 1) != 0;
    if (p.qos == 3) return false;
    size_t pos = 0;
    if (!getStr(pk.body, pos, p.topic)) return false;
    p.pid = 0;
    if (p.qos > 0) {
        if (pos + 2 > pk.body.size()) return false;
        p.pid = (uint16_t)(((uint8_t)pk.body[pos] << 8) | (uint8_t)pk.body[pos + 1]);
        pos += 2;
    }
    p.payload = pk.body.substr(pos);
    return true;
}

std::string encodePuback(uint16_t pid) { return frame(PUBACK, 0, std::string{char(pid >> 8), char(pid & 0xff)}); }
bool decodePuback(const std::string& b, uint16_t& pid) {
    if (b.size() != 2) return false;
    pid = (uint16_t)(((uint8_t)b[0] << 8) | (uint8_t)b[1]);
    return true;
}
std::string encodePingreq() { return frame(PINGREQ, 0, ""); }
std::string encodePingresp() { return frame(PINGRESP, 0, ""); }
std::string encodeDisconnect() { return frame(DISCONNECT, 0, ""); }

const char* connackText(uint8_t rc) {
    switch (rc) {
    case 0: return "accepted";
    case 1: return "the broker does not support MQTT 3.1.1";
    case 2: return "the broker refused the client id";
    case 3: return "the broker is not available";
    case 4: return "wrong user name or password";
    case 5: return "not authorised by the broker";
    default: return "the broker refused the connection";
    }
}

bool topicNameValid(const std::string& t) {
    if (t.empty() || t.size() > 65535) return false;
    for (char c : t) if (c == '+' || c == '#' || c == 0) return false;
    return true;
}

// ---- client ----
struct Client::Impl {
    ClientConfig cfg;
    sock_t s = kBadSock;
    tls::Ctx* ctx = nullptr;
    tls::Conn* tc = nullptr;
    std::string rx;
    uint16_t nextPid = 0;
    std::chrono::steady_clock::time_point lastWrite = std::chrono::steady_clock::now();

    bool up() const { return s != kBadSock; }
    void close() {
        if (tc) { tls::close(tc); tc = nullptr; }
        if (ctx) { tls::freeCtx(ctx); ctx = nullptr; }
        if (s != kBadSock) { sockClose(s); s = kBadSock; }
        rx.clear();
    }
    bool write(const std::string& d) {
        const bool ok = tc ? tls::write(tc, d.data(), d.size()) == (long)d.size() : sockSendAll(s, d.data(), d.size());
        if (ok) lastWrite = std::chrono::steady_clock::now();
        return ok;
    }
    // one packet within ms; false on timeout, close or garbage
    bool readPacket(Packet& pk, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            const int k = parsePacket(rx, pk);
            if (k < 0) return false;
            if (k > 0) { rx.erase(0, (size_t)k); return true; }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            sockRecvTimeout(s, (int)left);
            char buf[2048];
            const long n = tc ? tls::read(tc, buf, sizeof buf) : (long)sockRecv(s, buf, sizeof buf);
            if (n <= 0) {
                if (std::chrono::steady_clock::now() >= end) return false;
                if (!tc && n < 0) {   // a timeout is not the end of the connection but the caller asked for a packet: report it
#ifndef _WIN32
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
                }
                return false;
            }
            rx.append(buf, (size_t)n);
        }
    }
};

Client::Client() : p_(new Impl) {}
Client::~Client() { p_->close(); delete p_; }
void Client::configure(const ClientConfig& c) { p_->cfg = c; }
bool Client::connected() const { return p_->up(); }
double Client::idleSeconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_->lastWrite).count(); }

bool Client::connect(std::string& err) {
    p_->close();
    const ClientConfig& c = p_->cfg;
    if (c.host.empty()) { err = "no broker address"; return false; }
    if (c.tls && !tls::available()) { err = "this build has no TLS: use port 1883 on a trusted network, or a TLS proxy"; return false; }   // never a quiet downgrade
    p_->s = tcpConnect(c.host, c.port, c.connectTimeoutMs, err);
    if (p_->s == kBadSock) return false;
    if (c.tls) {
        sockRecvTimeout(p_->s, c.connectTimeoutMs);
        std::string e2;
        p_->ctx = tls::makeClientCtx(e2, false);
        if (p_->ctx) p_->tc = tls::connect(p_->ctx, p_->s, c.host, e2);
        if (!p_->tc) { err = "TLS: " + (e2.empty() ? std::string("handshake failed") : e2); p_->close(); return false; }
    }
    if (!p_->write(encodeConnect(c.connect))) { err = "the broker closed the connection"; p_->close(); return false; }
    Packet pk;
    if (!p_->readPacket(pk, c.connectTimeoutMs) || pk.type != CONNACK) { err = "no answer from the broker (is this an MQTT port?)"; p_->close(); return false; }
    bool sp = false;
    uint8_t rc = 0;
    if (!decodeConnack(pk.body, sp, rc)) { err = "the broker's answer was not understood"; p_->close(); return false; }
    if (rc != 0) { err = std::string("the broker said: ") + connackText(rc); p_->close(); return false; }
    return true;
}

bool Client::publish(const std::string& topic, const std::string& payload, int qos, bool retain, std::string& err) {
    if (!p_->up()) { err = "not connected"; return false; }
    if (!topicNameValid(topic)) { err = "the topic is not valid"; return false; }
    Publish pb;
    pb.topic = topic; pb.payload = payload; pb.qos = qos > 0 ? 1 : 0; pb.retain = retain;
    if (pb.qos) { if (++p_->nextPid == 0) p_->nextPid = 1; pb.pid = p_->nextPid; }
    if (!p_->write(encodePublish(pb))) { err = "the broker closed the connection"; p_->close(); return false; }
    if (!pb.qos) return true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_->cfg.ackTimeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        Packet pk;
        if (left <= 0 || !p_->readPacket(pk, (int)left)) { err = "the broker did not confirm"; p_->close(); return false; }
        uint16_t id = 0;
        if (pk.type == PUBACK && decodePuback(pk.body, id) && id == pb.pid) return true;
    }
}

bool Client::ping(std::string& err) {
    if (!p_->up()) { err = "not connected"; return false; }
    if (!p_->write(encodePingreq())) { err = "the broker closed the connection"; p_->close(); return false; }
    Packet pk;
    if (!p_->readPacket(pk, p_->cfg.ackTimeoutMs) || pk.type != PINGRESP) { err = "the broker stopped answering"; p_->close(); return false; }
    return true;
}

void Client::disconnect() {
    if (p_->up()) p_->write(encodeDisconnect());
    p_->close();
}

}  // namespace dect2::integrations::mqtt
