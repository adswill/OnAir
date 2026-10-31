#include "dect2/source/mount_driver.h"
#include "dect2/source/rotator.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#endif

namespace dect2 {

namespace {

double normAz(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

std::string fmt(const char* f, double a, double b = 0) {
    char buf[96];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

// A TCP connection with timeouts, used by both drivers.
class Conn {
public:
    ~Conn() { close(); }
    bool open(const std::string& host, int port, int timeoutMs, std::string& err) {
        close();
        netInit();
        const std::string h = (host.empty() || host == "localhost") ? "127.0.0.1" : host;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(h.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { err = "unknown host " + host; return false; }
        sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == kBadSock) { freeaddrinfo(res); err = "cannot open a socket"; return false; }
        setBlocking(s, false);
        int rc = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
        freeaddrinfo(res);
        bool ok = rc == 0;
        if (!ok) {   // in progress: wait for writable, then check the socket error
            fd_set wr; FD_ZERO(&wr); FD_SET(s, &wr);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select((int)s + 1, nullptr, &wr, nullptr, &tv) > 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl);
                ok = soerr == 0;
            }
        }
        if (!ok) { sockClose(s); err = "cannot connect to " + host + ":" + std::to_string(port); return false; }
        setBlocking(s, true);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
        s_ = s;
        return true;
    }
    void close() { if (s_ != kBadSock) { sockClose(s_); s_ = kBadSock; } rbuf_.clear(); }
    bool isOpen() const { return s_ != kBadSock; }
    bool writeAll(const std::string& d) {
        const char* p = d.data();
        size_t n = d.size();
        while (n) {
            const long long w = sockSend(s_, p, n);
            if (w <= 0) return false;
            p += w; n -= (size_t)w;
        }
        return true;
    }
    // One '\n'-terminated line (CR/LF stripped). False on timeout or a closed connection.
    bool readLine(std::string& out, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const size_t nl = rbuf_.find('\n');
            if (nl != std::string::npos) {
                out = rbuf_.substr(0, nl);
                rbuf_.erase(0, nl + 1);
                while (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            if (!fill(deadline)) return false;
        }
    }
    // Everything until the peer closes (HTTP with Connection: close), up to maxBytes.
    bool readAll(std::string& out, int timeoutMs, size_t maxBytes = 1 << 20) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (rbuf_.size() < maxBytes) {
            if (!fill(deadline)) break;
        }
        out = rbuf_;
        rbuf_.clear();
        return !out.empty();
    }
private:
    bool fill(std::chrono::steady_clock::time_point deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        fd_set rd; FD_ZERO(&rd); FD_SET(s_, &rd);
        timeval tv{(long)(left / 1000), (int)((left % 1000) * 1000)};
        if (select((int)s_ + 1, &rd, nullptr, nullptr, &tv) <= 0) return false;
        char buf[2048];
        const long long r = sockRecv(s_, buf, sizeof buf);
        if (r <= 0) return false;
        rbuf_.append(buf, (size_t)r);
        return true;
    }
    static void setBlocking(sock_t s, bool block) {
#ifdef _WIN32
        u_long m = block ? 0 : 1;
        ioctlsocket(s, FIONBIO, &m);
#else
        const int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, block ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
    }
    sock_t s_ = kBadSock;
    std::string rbuf_;
};

// ---------------------------------------------------------------- Hamlib rotctld
class RotctldDriver : public MountDriver {
public:
    RotctldDriver(std::string host, int port) : host_(std::move(host)), port_(port) {}
    const char* name() const override { return "Hamlib rotctld"; }
    bool connect(std::string& err) override { return c_.open(host_, port_, 2000, err); }
    void close() override { c_.close(); }
    bool gotoAzEl(double az, double el, std::string& err) override { return rprt(rotctldSetPosCmd(az, el), err); }
    bool stop(std::string& err) override { return rprt("S\n", err); }
    bool readPosition(MountPos& out, std::string& err) override {
        if (!c_.isOpen()) { err = "not connected"; return false; }
        if (!c_.writeAll("p\n")) { err = "write failed"; return false; }
        std::string l1, l2;
        if (!c_.readLine(l1, 1500)) { err = "no reply from rotctld"; return false; }
        int code = 0;
        if (parseRprt(l1, code)) { err = "rotctld error " + std::to_string(code); return false; }
        if (!c_.readLine(l2, 1500)) { err = "incomplete position reply"; return false; }
        double az = 0, el = 0;
        if (!parseRotctldPos(l1, l2, az, el)) { err = "unreadable position from rotctld"; return false; }
        out.azDeg = normAz(az); out.elDeg = el; out.azValid = out.elValid = true;
        return true;
    }
private:
    bool rprt(const std::string& cmd, std::string& err) {
        if (!c_.isOpen()) { err = "not connected"; return false; }
        if (!c_.writeAll(cmd)) { err = "write failed"; return false; }
        std::string l;
        if (!c_.readLine(l, 1500)) { err = "no reply from rotctld"; return false; }
        int code = 0;
        if (!parseRprt(l, code)) { err = "unexpected reply: " + l; return false; }
        if (code != 0) { err = "rotctld error " + std::to_string(code); return false; }
        return true;
    }
    Conn c_;
    std::string host_;
    int port_;
};

// ---------------------------------------------------------------- ASCOM Alpaca
class AlpacaDriver : public MountDriver {
public:
    AlpacaDriver(std::string host, int port, int dev) : host_(std::move(host)), port_(port), dev_(dev) {}
    const char* name() const override { return "ASCOM Alpaca"; }
    bool connect(std::string& err) override {
        std::string v;
        if (!put("connected", "Connected=true", v, err)) return false;
        if (get("canslewaltaz", v, err) && v != "true") { err = "this telescope cannot slew to Alt/Az"; return false; }
        std::string r;
        canMove_ = get("canmoveaxis?Axis=0", r, err) && r == "true";   // primary axis = azimuth on an Alt/Az mount
        err.clear();
        up_ = true;
        return true;
    }
    void close() override {
        if (!up_) return;
        std::string v, e;
        put("connected", "Connected=false", v, e);
        up_ = false;
    }
    bool gotoAzEl(double az, double el, std::string& err) override {
        std::string v;
        moveAxesQuiet();
        return put("slewtoaltazasync", fmt("Azimuth=%.4f&Altitude=%.4f", normAz(az), el), v, err);
    }
    bool stop(std::string& err) override {
        std::string v;
        const bool ok = put("abortslew", "", v, err);
        moveAxesQuiet();
        return ok;
    }
    bool readPosition(MountPos& out, std::string& err) override {
        std::string az, el, sl, e2;
        if (!get("azimuth", az, err)) return false;
        if (!get("altitude", el, err)) return false;
        out.azDeg = normAz(std::atof(az.c_str())); out.elDeg = std::atof(el.c_str());
        out.azValid = out.elValid = true;
        if (get("slewing", sl, e2)) { out.slewing = sl == "true"; out.slewingValid = true; }
        return true;
    }
    bool supportsRates() const override { return canMove_; }
    bool moveAxes(double azRate, double elRate, std::string& err) override {
        std::string v;
        if (!canMove_) { err = "this mount cannot move at a set speed"; return false; }
        const bool a = put("moveaxis", fmt("Axis=0&Rate=%.5f", azRate), v, err);
        const bool b = put("moveaxis", fmt("Axis=1&Rate=%.5f", elRate), v, err);
        moving_ = (azRate != 0 || elRate != 0);
        return a && b;
    }
private:
    void moveAxesQuiet() {   // a slew command must not fight an axis that is still moving
        if (canMove_ && moving_) { std::string e; moveAxes(0, 0, e); }
    }
    std::string base() const { return "/api/v1/telescope/" + std::to_string(dev_) + "/"; }
    bool request(const char* method, const std::string& path, const std::string& body, std::string& value, std::string& err) {
        Conn c;
        if (!c.open(host_, port_, 2000, err)) return false;
        std::string rq = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + host_ + ":" + std::to_string(port_) +
                         "\r\nConnection: close\r\nAccept: application/json\r\n";
        if (std::strcmp(method, "PUT") == 0) {
            rq += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        } else rq += "\r\n";
        if (!c.writeAll(rq)) { err = "write failed"; return false; }
        std::string resp;
        if (!c.readAll(resp, 4000)) { err = "no reply from the Alpaca server"; return false; }
        const size_t he = resp.find("\r\n\r\n");
        if (resp.compare(0, 5, "HTTP/") != 0 || he == std::string::npos) { err = "not an HTTP reply"; return false; }
        const int status = std::atoi(resp.c_str() + resp.find(' ') + 1);
        std::string json = resp.substr(he + 4);
        if (status != 200 && json.find("ErrorNumber") == std::string::npos) { err = "HTTP " + std::to_string(status); return false; }
        return alpacaParseResponse(json, value, err);
    }
    bool get(const std::string& what, std::string& value, std::string& err) {
        std::string path = base() + what + (what.find('?') == std::string::npos ? "?" : "&");
        path += "ClientID=" + std::to_string(clientId_) + "&ClientTransactionID=" + std::to_string(++tx_);
        return request("GET", path, "", value, err);
    }
    bool put(const std::string& what, const std::string& form, std::string& value, std::string& err) {
        std::string body = form;
        if (!body.empty()) body += "&";
        body += "ClientID=" + std::to_string(clientId_) + "&ClientTransactionID=" + std::to_string(++tx_);
        return request("PUT", base() + what, body, value, err);
    }
    std::string host_;
    int port_, dev_;
    bool up_ = false, canMove_ = false, moving_ = false;
    int clientId_ = 4242;
    int tx_ = 0;
};

} // namespace

// ---------------------------------------------------------------- pure helpers
std::string rotctldSetPosCmd(double az, double el) { return "P " + fmt("%.3f %.3f", normAz(az), el) + "\n"; }

bool parseRprt(const std::string& line, int& code) {
    if (line.compare(0, 4, "RPRT") != 0) return false;
    code = std::atoi(line.c_str() + 4);
    return true;
}

bool parseRotctldPos(const std::string& l1, const std::string& l2, double& az, double& el) {
    char* e1 = nullptr; char* e2 = nullptr;
    const double a = std::strtod(l1.c_str(), &e1);
    const double b = std::strtod(l2.c_str(), &e2);
    if (e1 == l1.c_str() || e2 == l2.c_str()) return false;
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    az = a; el = b;
    return true;
}

bool alpacaParseResponse(const std::string& json, std::string& value, std::string& err) {
    value.clear();
    auto field = [&](const char* key, size_t from = 0) -> size_t {
        const size_t k = json.find(std::string("\"") + key + "\"", from);
        if (k == std::string::npos) return k;
        size_t c = json.find(':', k);
        if (c == std::string::npos) return c;
        c++;
        while (c < json.size() && (json[c] == ' ' || json[c] == '\t')) c++;
        return c;
    };
    const size_t en = field("ErrorNumber");
    if (en == std::string::npos) { err = "not an Alpaca reply"; return false; }
    const int num = std::atoi(json.c_str() + en);
    if (num != 0) {
        err = "Alpaca error " + std::to_string(num);
        size_t m = field("ErrorMessage");
        if (m != std::string::npos && m < json.size() && json[m] == '"') {
            const size_t e = json.find('"', m + 1);
            if (e != std::string::npos && e > m + 1) err += ": " + json.substr(m + 1, e - m - 1);
        }
        return false;
    }
    const size_t v = field("Value");
    if (v != std::string::npos) {
        size_t e = v;
        if (json[v] == '"') { e = json.find('"', v + 1); value = json.substr(v + 1, e == std::string::npos ? std::string::npos : e - v - 1); }
        else { while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != ' ') e++; value = json.substr(v, e - v); }
    }
    return true;
}

double mountRateForError(double errDeg, double targetRate, double maxRate, double gain, double deadband) {
    double r = targetRate;
    if (std::fabs(errDeg) > deadband) r += gain * errDeg;
    maxRate = std::fabs(maxRate);
    return std::max(-maxRate, std::min(maxRate, r));
}

std::unique_ptr<MountDriver> makeRotctldDriver(const std::string& host, int port) { return std::make_unique<RotctldDriver>(host, port); }
std::unique_ptr<MountDriver> makeAlpacaDriver(const std::string& host, int port, int dev) { return std::make_unique<AlpacaDriver>(host, port, dev); }

std::unique_ptr<MountDriver> makeMountDriver(const RotatorConfig& cfg) {
    if (cfg.link != RotatorLink::Tcp) return nullptr;
    if (cfg.protocol == RotatorProtocol::Rotctld) return makeRotctldDriver(cfg.host, cfg.tcpPort);
    if (cfg.protocol == RotatorProtocol::Alpaca) return makeAlpacaDriver(cfg.host, cfg.tcpPort, cfg.alpacaDevice);
    return nullptr;
}

bool probeMount(const RotatorConfig& cfg, MountPos& pos, std::string& msg) {
    std::unique_ptr<MountDriver> d = makeMountDriver(cfg);
    if (!d) { msg = "choose Hamlib rotctld or ASCOM Alpaca over TCP"; return false; }
    std::string err;
    if (!d->connect(err)) { msg = err; return false; }
    const bool ok = d->readPosition(pos, err);
    char b[160];
    if (ok) std::snprintf(b, sizeof b, "%s answered: az %.1f, el %.1f%s", d->name(), pos.azDeg, pos.elDeg, d->supportsRates() ? " (takes axis speeds)" : "");
    msg = ok ? b : err;
    d->close();
    return ok;
}

std::vector<std::string> alpacaDiscover(int timeoutMs, const std::string& targetAddr, int udpPort) {
    std::vector<std::string> found;
    netInit();
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) return found;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)udpPort);
    if (inet_pton(AF_INET, targetAddr.c_str(), &to.sin_addr) != 1) { sockClose(s); return found; }
    const char msg[] = "alpacadiscovery1";
    sockSendTo(s, msg, sizeof msg - 1, to);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{(long)(left / 1000), (int)((left % 1000) * 1000)};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) break;
        char buf[512];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const int r = (int)recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
        if (r <= 0) continue;
        buf[r] = 0;
        const char* k = std::strstr(buf, "AlpacaPort");
        if (!k) continue;
        const char* c = std::strchr(k, ':');
        const int port = c ? std::atoi(c + 1) : 0;
        if (port <= 0 || port > 65535) continue;
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        const std::string e = std::string(ip) + ":" + std::to_string(port);
        if (std::find(found.begin(), found.end(), e) == found.end()) found.push_back(e);
    }
    sockClose(s);
    return found;
}

} // namespace dect2
