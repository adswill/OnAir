#include "dect2/fm/rds_live.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

namespace dect2 {

namespace {
std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
bool parseBool(const std::string& v, bool& out) {
    const std::string u = upper(trim(v));
    if (u == "1" || u == "ON" || u == "YES" || u == "TRUE") { out = true; return true; }
    if (u == "0" || u == "OFF" || u == "NO" || u == "FALSE") { out = false; return true; }
    return false;
}
std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
}

void rdsClockWords(int64_t utc, int offsetHalfHours, bool tp, int pty, uint16_t& b, uint16_t& c, uint16_t& d) {
    const int64_t days = utc / 86400;
    const int sod = (int)(utc - days * 86400);
    const int mjd = (int)(days + 40587);
    const int hour = sod / 3600, minute = (sod / 60) % 60;
    b = (uint16_t)(0x4000 | (tp ? 1 : 0) << 10 | (pty & 31) << 5 | ((mjd >> 15) & 3));
    c = (uint16_t)(((mjd & 0x7FFF) << 1) | ((hour >> 4) & 1));
    const int off = std::max(-24, std::min(24, offsetHalfHours));
    d = (uint16_t)(((hour & 15) << 12) | (minute << 6) | (off < 0 ? 32 : 0) | (std::abs(off) & 31));
}

struct RdsLive::Impl {
    mutable std::mutex mu;
    RdsState st;
    std::atomic<uint64_t> ver{1};
    // command port
    std::atomic<bool> run{false};
    int port = 0;
    std::string token;
    sock_t udp = kBadSock, tcp = kBadSock;
    std::thread th;
    // file watch
    std::string file;
    std::atomic<bool> frun{false};
    std::thread fth;
    std::mutex fmu;
};

RdsLive::RdsLive() : p_(new Impl) {}
RdsLive::~RdsLive() {
    std::string e;
    listen(0, false, "", e);
    watchFile("");
}
RdsLive& RdsLive::global() { static RdsLive g; return g; }

RdsState RdsLive::get() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }
uint64_t RdsLive::version() const { return p_->ver; }

void RdsLive::seed(const RdsState& in) {
    std::lock_guard<std::mutex> lk(p_->mu);
    RdsState s = in;
    if (!p_->file.empty()) s.rt = p_->st.rt;   // a watched file owns the text
    const bool rtChanged = p_->st.rt != s.rt;
    const int ab = p_->st.abFlag;
    p_->st = s;
    p_->st.abFlag = rtChanged ? (ab ^ 1) : ab;
    p_->ver++;
}

bool RdsLive::apply(const std::string& keyIn, const std::string& valIn, std::string& err) {
    const std::string key = upper(trim(keyIn));
    std::lock_guard<std::mutex> lk(p_->mu);
    RdsState& s = p_->st;
    if (key == "PS") {
        std::string v = valIn;
        if (v.size() > 8) v.resize(8);
        v.append(8 - v.size(), ' ');
        s.ps = v;
    } else if (key == "RT" || key == "TEXT") {
        std::string v = valIn;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if (v.size() > 64) v.resize(64);
        if (v != s.rt) s.abFlag ^= 1;
        s.rt = v;
    } else if (key == "PI") {
        char* end = nullptr;
        const std::string v = trim(valIn);
        const long x = strtol(v.c_str(), &end, 16);
        if (v.empty() || *end || x < 0 || x > 0xFFFF) { err = "PI is four hex digits (0000 to FFFF)"; return false; }
        s.pi = (int)x;
    } else if (key == "PTY") {
        char* end = nullptr;
        const std::string v = trim(valIn);
        const long x = strtol(v.c_str(), &end, 10);
        if (v.empty() || *end || x < 0 || x > 31) { err = "PTY is a number from 0 to 31"; return false; }
        s.pty = (int)x;
    } else if (key == "TP") {
        if (!parseBool(valIn, s.tp)) { err = "TP is 0 or 1"; return false; }
    } else if (key == "TA") {
        if (!parseBool(valIn, s.ta)) { err = "TA is 0 or 1"; return false; }
    } else if (key == "MS" || key == "MUSIC") {
        const std::string u = upper(trim(valIn));
        if (u == "M" || u == "MUSIC") s.music = true;
        else if (u == "S" || u == "SPEECH") s.music = false;
        else if (!parseBool(valIn, s.music)) { err = "MS is M (music) or S (speech)"; return false; }
    } else if (key == "CT") {
        if (!parseBool(valIn, s.ct)) { err = "CT is 0 or 1"; return false; }
    } else if (key == "CTOFF") {
        char* end = nullptr;
        const std::string v = trim(valIn);
        const long x = strtol(v.c_str(), &end, 10);
        if (v.empty() || *end || x < -24 || x > 24) { err = "CTOFF is the time offset in half hours (-24 to 24)"; return false; }
        s.ctOffsetHalfHours = (int)x;
    } else {
        err = "unknown key (PS, RT, PI, PTY, TP, TA, MS, CT, CTOFF)";
        return false;
    }
    p_->ver++;
    return true;
}

std::string RdsLive::json() const {
    const RdsState s = get();
    char b[96];
    snprintf(b, sizeof b, "\"pi\":\"%04X\",\"pty\":%d,\"tp\":%s,\"ta\":%s,\"music\":%s,\"ct\":%s,\"ct_offset_half_hours\":%d", s.pi, s.pty, s.tp ? "true" : "false",
             s.ta ? "true" : "false", s.music ? "true" : "false", s.ct ? "true" : "false", s.ctOffsetHalfHours);
    return "{\"ps\":\"" + esc(s.ps) + "\",\"rt\":\"" + esc(s.rt) + "\"," + b + ",\"version\":" + std::to_string(p_->ver.load()) + "}";
}

std::string RdsLive::command(const std::string& lineIn) {
    const std::string line = trim(lineIn);
    if (line.empty()) return "ERR empty";
    const std::string u = upper(line);
    if (u == "?" || u == "GET" || u == "STATUS") return "OK " + json();
    size_t eq = line.find('=');
    if (eq == std::string::npos) eq = line.find(' ');
    if (eq == std::string::npos) return "ERR use KEY=value";
    std::string key = line.substr(0, eq), val = line.substr(eq + 1);
    // the value keeps its spaces (a name or text may begin or end with one); only one leading space after "KEY " is dropped
    std::string err;
    if (!apply(key, val, err)) return "ERR " + err;
    return "OK";
}

// ---------------------------------------------------------------- command port
namespace {
bool waitReadable(sock_t s, int ms) {
    fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select((int)s + 1, &rd, nullptr, nullptr, &tv) > 0;
}
}

bool RdsLive::listen(int port, bool lan, const std::string& token, std::string& err) {
    Impl& I = *p_;
    if (I.run.exchange(false)) {
        if (I.th.joinable()) I.th.join();
        if (I.udp != kBadSock) { sockClose(I.udp); I.udp = kBadSock; }
        if (I.tcp != kBadSock) { sockClose(I.tcp); I.tcp = kBadSock; }
        I.port = 0;
    }
    if (port <= 0) return true;
    if (lan && token.size() < 16) { err = "listening on the network needs a token of at least 16 characters"; return false; }
    netInit();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
    sock_t t = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (t != kBadSock) setsockopt(t, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    if (u == kBadSock || t == kBadSock || bind(u, (sockaddr*)&a, sizeof a) != 0) { err = "cannot listen on the RDS command port"; if (u != kBadSock) sockClose(u); if (t != kBadSock) sockClose(t); return false; }
    if (port == 0 || true) {
        // the TCP side uses the number the UDP side got (port 0 asks for any free one)
        socklen_t l = sizeof a;
        getsockname(u, (sockaddr*)&a, &l);
    }
    if (bind(t, (sockaddr*)&a, sizeof a) != 0 || ::listen(t, 4) != 0) { err = "cannot listen on the RDS command port (TCP)"; sockClose(u); sockClose(t); return false; }
    I.udp = u; I.tcp = t; I.port = ntohs(a.sin_port); I.token = token; I.run = true;
    I.th = std::thread([this, u, t, token] {
        Impl& I = *p_;
        char buf[2048];
        while (I.run) {
            fd_set rd; FD_ZERO(&rd); FD_SET(u, &rd); FD_SET(t, &rd);
            timeval tv{0, 100000};
            if (select((int)std::max(u, t) + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            if (FD_ISSET(u, &rd)) {
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                const auto n = recvfrom(u, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
                if (n > 0) {
                    std::string msg(buf, (size_t)n);
                    bool ok = true;
                    if (!token.empty()) {
                        const std::string pre = token + ";";
                        if (msg.compare(0, pre.size(), pre) == 0) msg.erase(0, pre.size()); else ok = false;
                    }
                    std::string reply = ok ? std::string() : "ERR token";
                    if (ok) {
                        size_t pos = 0;
                        while (pos < msg.size()) {   // several lines in one datagram
                            size_t e = msg.find('\n', pos);
                            if (e == std::string::npos) e = msg.size();
                            const std::string ln = msg.substr(pos, e - pos);
                            if (!trim(ln).empty()) reply = command(ln);
                            pos = e + 1;
                        }
                    }
                    if (!reply.empty()) { reply += "\n"; sendto(u, reply.data(), (int)reply.size(), 0, (sockaddr*)&from, fl); }
                }
            }
            if (FD_ISSET(t, &rd)) {
                sock_t c = accept(t, nullptr, nullptr);
                if (c == kBadSock) continue;
                // one session at a time, served here with short reads (a client that stalls for 10 s is dropped)
                bool authed = token.empty();
                std::string acc;
                auto idle = std::chrono::steady_clock::now();
                while (I.run && std::chrono::steady_clock::now() - idle < std::chrono::seconds(10)) {
                    if (!waitReadable(c, 100)) continue;
                    const auto n = sockRecv(c, buf, sizeof buf);
                    if (n <= 0) break;
                    idle = std::chrono::steady_clock::now();
                    acc.append(buf, (size_t)n);
                    if (acc.size() > 4096) break;
                    size_t e;
                    bool drop = false;
                    while ((e = acc.find('\n')) != std::string::npos) {
                        std::string ln = acc.substr(0, e);
                        acc.erase(0, e + 1);
                        std::string reply;
                        if (!authed) {
                            const std::string t2 = trim(ln);
                            if (upper(t2.substr(0, 5)) == "AUTH=" && t2.substr(5) == token) { authed = true; reply = "OK"; }
                            else { reply = "ERR auth"; drop = true; }
                        } else if (!trim(ln).empty()) reply = command(ln);
                        reply += "\n";
                        sockSend(c, reply.data(), reply.size());
                        if (drop) break;
                    }
                    if (drop) break;
                }
                sockClose(c);
            }
        }
    });
    return true;
}
int RdsLive::listeningPort() const { return p_->run ? p_->port : 0; }

// ---------------------------------------------------------------- file watch
void RdsLive::watchFile(const std::string& path) {
    Impl& I = *p_;
    std::lock_guard<std::mutex> fl(I.fmu);
    if (I.frun.exchange(false) && I.fth.joinable()) I.fth.join();
    { std::lock_guard<std::mutex> lk(I.mu); I.file = path; }
    if (path.empty()) return;
    I.frun = true;
    I.fth = std::thread([this, path] {
        Impl& I = *p_;
        std::string last;
        bool first = true;
        while (I.frun) {
            std::ifstream f(path, std::ios::binary);
            if (f) {
                std::string ln;
                std::getline(f, ln);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                if (first || ln != last) {
                    last = ln;
                    first = false;
                    std::string e;
                    apply("RT", ln, e);
                }
            }
            for (int i = 0; i < 5 && I.frun; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}
std::string RdsLive::watchedFile() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->file; }

} // namespace dect2
