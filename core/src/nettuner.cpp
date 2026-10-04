#include "dect2/nettuner.h"
#include "dect2/hls.h"
#include "netcompat.h"
#include "dect2/timecompat.h"
#ifndef _WIN32
#include <ifaddrs.h>
#endif
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <ctime>

namespace dect2 {

namespace {
constexpr size_t kMaxQueueBytes = 6u << 20;   // a client that falls this far behind loses its oldest data instead of stalling the receiver

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) { case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '"': o += "&quot;"; break; default: o += c; }
    }
    return o;
}
std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    return o;
}
std::string xmltvTime(int64_t t) {
    time_t tt = (time_t)t; struct tm g;
    gmTime(tt, &g);
    char b[40]; strftime(b, sizeof b, "%Y%m%d%H%M%S +0000", &g);
    return b;
}
bool sendAll(sock_t fd, const void* d, size_t n) {
    const char* p = (const char*)d;
    while (n) {
        long long w = sockSend(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}
} // namespace

struct Client {
    sock_t fd = kBadSock;
    int sid = 0;
    ServiceFilter filter;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> q;
    size_t bytes = 0;
    bool done = false;
};

struct NetTuner::Impl {
    Engine& eng;
    NetTunerConfig cfg;
    std::atomic<bool> running{false};
    sock_t lsock = kBadSock;
    std::thread acceptor;
    mutable std::mutex mu;               // clients_, error
    std::list<std::shared_ptr<Client>> clients;
    std::atomic<uint64_t> sent{0};
    std::atomic<int> live{0};   // connection threads still running
    std::string error;

    // HTTP Live Streaming: one pipeline per service that somebody asked for, stopped again when nobody has fetched from it for a while
    struct HlsEntry {
        std::shared_ptr<Client> client;
        std::unique_ptr<HlsPipeline> pipe;
        std::atomic<bool> quit{false};
        std::vector<uint8_t> rest;
        size_t restOff = 0;
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    };
    std::mutex hmu;
    std::map<int, std::shared_ptr<HlsEntry>> hls;

    void dropHls(const std::shared_ptr<HlsEntry>& e) {
        e->quit = true;
        e->client->cv.notify_all();
        e->pipe->stop();
        std::lock_guard<std::mutex> lk(mu);
        clients.remove(e->client);
    }
    void pruneHls() {   // hmu held
        for (auto it = hls.begin(); it != hls.end();) {
            const bool idle = it->second->pipe->idleSeconds() > 30 && std::chrono::steady_clock::now() - it->second->created > std::chrono::seconds(30);
            if (idle || !running) { dropHls(it->second); it = hls.erase(it); } else ++it;
        }
    }
    std::shared_ptr<HlsEntry> hlsFor(int sid) {
        std::lock_guard<std::mutex> lk(hmu);
        pruneHls();
        auto it = hls.find(sid);
        if (it != hls.end()) return it->second;
        auto e = std::make_shared<HlsEntry>();
        e->client = std::make_shared<Client>();
        e->client->sid = sid;
        e->client->filter.select(sid);
        HlsEntry* raw = e.get();
        char tok[16];
        snprintf(tok, sizeof tok, "%08x_", (unsigned)(std::random_device{}()));
        e->pipe = std::make_unique<HlsPipeline>([this, raw](uint8_t* buf, int size) -> int {
            while (running && !raw->quit) {
                if (raw->restOff < raw->rest.size()) {
                    const size_t n = std::min((size_t)size, raw->rest.size() - raw->restOff);
                    memcpy(buf, raw->rest.data() + raw->restOff, n);
                    raw->restOff += n;
                    return (int)n;
                }
                std::vector<uint8_t> chunk;
                {
                    std::unique_lock<std::mutex> lk(raw->client->mu);
                    raw->client->cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return !raw->client->q.empty() || raw->quit || !running; });
                    if (!raw->client->q.empty()) { chunk = std::move(raw->client->q.front()); raw->client->q.pop_front(); raw->client->bytes -= chunk.size(); }
                }
                if (!chunk.empty()) { raw->rest = std::move(chunk); raw->restOff = 0; }
            }
            return 0;
        }, tok);
        { std::lock_guard<std::mutex> l2(mu); clients.push_back(e->client); }
        e->pipe->start();
        hls[sid] = e;
        return e;
    }
    void stopHls() {
        std::lock_guard<std::mutex> lk(hmu);
        for (auto& kv : hls) dropHls(kv.second);
        hls.clear();
    }

    // /hls/<service>/index.m3u8 and the segments next to it
    void serveHls(const std::string& path, const std::function<void(int, const char*, const std::string&)>& reply) {
        const size_t slash = path.find('/', 5);
        if (slash == std::string::npos) { reply(404, "text/plain", "not found"); return; }
        const int sid = atoi(path.c_str() + 5);
        const std::string name = path.substr(slash + 1);
        auto e = hlsFor(sid);
        std::string body, type;
        // the first request starts the pipeline: give it time to produce the first segments
        for (int i = 0; i < 100 && running; i++) {
            if (e->pipe->get(name, body, type)) { reply(200, type.c_str(), body); return; }
            if (!e->pipe->error().empty()) break;
            if (name != "index.m3u8") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        const std::string err = e->pipe->error();
        if (!err.empty()) {
            { std::lock_guard<std::mutex> lk(hmu); auto it = hls.find(sid); if (it != hls.end() && it->second == e) { dropHls(e); hls.erase(it); } }
            reply(500, "text/plain", err);
        } else reply(name == "index.m3u8" ? 503 : 404, "text/plain", "the stream is not ready yet");
    }

    explicit Impl(Engine& e) : eng(e) {}

    // called from the receiver thread for every burst of transport stream packets: must stay fast
    void tap(const uint8_t* pk, size_t n, const TsSnapshot* snap) {
        std::vector<std::shared_ptr<Client>> cs;
        { std::lock_guard<std::mutex> lk(mu); for (auto& c : clients) cs.push_back(c); }
        for (auto& c : cs) {
            std::vector<uint8_t> out;
            out.reserve(n * 188 / 4);
            uint8_t o[188];
            for (size_t i = 0; i < n; i++) if (c->filter.process(pk + i * 188, snap, o)) out.insert(out.end(), o, o + 188);
            if (out.empty()) continue;
            std::lock_guard<std::mutex> lk(c->mu);
            c->bytes += out.size();
            c->q.push_back(std::move(out));
            while (c->bytes > kMaxQueueBytes && c->q.size() > 1) { c->bytes -= c->q.front().size(); c->q.pop_front(); }
            c->cv.notify_one();
        }
    }

    bool authorised(const std::string& query) const {
        if (cfg.key.empty()) return true;
        return query.find("key=" + cfg.key) != std::string::npos;
    }

    static std::string respond(int code, const char* type, const std::string& body) {
        std::string h = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : code == 401 ? " Unauthorized" : code == 500 ? " Internal Server Error" : code == 503 ? " Service Unavailable" : " Not Found") + "\r\nContent-Type: " + type +
                        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n";
        return h + body;
    }

    void serve(sock_t fd) {
        char buf[4096]; size_t have = 0;
        while (have < sizeof buf - 1) {
            long long r = sockRecv(fd, buf + have, sizeof buf - 1 - have);
            if (r <= 0) { sockClose(fd); return; }
            have += (size_t)r; buf[have] = 0;
            if (strstr(buf, "\r\n\r\n")) break;
        }
        std::string req(buf, have);
        std::string path, query, host;
        {
            size_t s = req.find(' '), e = req.find(' ', s + 1);
            if (s == std::string::npos || e == std::string::npos || req.compare(0, 3, "GET") != 0) { const std::string r = respond(404, "text/plain", "not found"); sendAll(fd, r.data(), r.size()); sockClose(fd); return; }
            path = req.substr(s + 1, e - s - 1);
            size_t q = path.find('?');
            if (q != std::string::npos) { query = path.substr(q + 1); path.resize(q); }
            size_t h = req.find("\r\nHost:");
            if (h == std::string::npos) h = req.find("\r\nhost:");
            if (h != std::string::npos) { size_t a = h + 7, b = req.find("\r\n", a); host = req.substr(a, b - a); while (!host.empty() && host[0] == ' ') host.erase(0, 1); }
        }
        if (host.empty()) host = "127.0.0.1:" + std::to_string(cfg.port);
        const std::string keyQ = cfg.key.empty() ? "" : "?key=" + cfg.key;
        auto reply = [&](int code, const char* type, const std::string& body) { std::string s = respond(code, type, body); sendAll(fd, s.data(), s.size()); sockClose(fd); };
        // the segment names of a stream contain a random token that only the (keyed) playlist reveals: players fetch them without the key
        const bool hlsSegment = path.rfind("/hls/", 0) == 0 && path.size() > 3 && path.compare(path.size() - 3, 3, ".ts") == 0;
        if (!hlsSegment && !authorised(query)) { reply(401, "text/plain", "a key is required"); return; }

        const TsSnapshot snap = eng.tsSnapshot();
        std::vector<const TsService*> svc;
        for (const auto& s : snap.services) if (s.havePmt && (s.type == 1 || s.type == 2 || s.type == 3 || s.type == 0x0A || s.type == 0x11 || s.type == 0x16 || s.type == 0x19 || s.type == 0x1F)) svc.push_back(&s);
        const std::string base = "http://" + host;

        if (path.rfind("/hls/", 0) == 0) { serveHls(path, reply); return; }

        if (path.rfind("/stream/", 0) == 0) {
            const int sid = atoi(path.c_str() + 8);
            bool found = false;
            for (auto& s : snap.services) if (s.id == sid) found = true;
            if (!found) { reply(404, "text/plain", "no such service (is the receiver locked?)"); return; }
            int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            const char* hdr = "HTTP/1.0 200 OK\r\nContent-Type: video/MP2T\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
            if (!sendAll(fd, hdr, strlen(hdr))) { sockClose(fd); return; }
            auto c = std::make_shared<Client>();
            c->fd = fd; c->sid = sid; c->filter.select(sid);
            { std::lock_guard<std::mutex> lk(mu); clients.push_back(c); }
            // a closed connection shows up as a failed send; a pending read tells us at once when the viewer goes away
            while (running) {
                std::vector<uint8_t> chunk;
                {
                    std::unique_lock<std::mutex> lk(c->mu);
                    c->cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return !c->q.empty() || !running; });
                    if (!c->q.empty()) { chunk = std::move(c->q.front()); c->q.pop_front(); c->bytes -= chunk.size(); }
                }
                if (!chunk.empty()) { if (!sendAll(fd, chunk.data(), chunk.size())) break; sent += chunk.size(); }
                else if (sockPeekClosed(fd) == 0) break;
            }
            { std::lock_guard<std::mutex> lk(mu); clients.remove(c); }
            sockClose(fd);
            return;
        }
        if (path == "/lineup.m3u" || path == "/playlist.m3u") {
            std::string m = "#EXTM3U\n";
            for (auto* s : svc) {
                m += "#EXTINF:-1 tvg-id=\"" + std::to_string(s->id) + ".onair\" tvg-name=\"" + xmlEscape(s->name) + "\"";
                if (s->lcn) m += " tvg-chno=\"" + std::to_string(s->lcn) + "\"";
                m += "," + s->name + "\n" + base + "/stream/" + std::to_string(s->id) + keyQ + "\n";
            }
            reply(200, "audio/x-mpegurl", m);
        } else if (path == "/guide.xml" || path == "/xmltv.xml") {
            std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tv generator-info-name=\"OnAir\">\n";
            for (auto* s : svc) x += "<channel id=\"" + std::to_string(s->id) + ".onair\"><display-name>" + xmlEscape(s->name) + "</display-name></channel>\n";
            for (auto& kv : eng.epg()) {
                bool known = false;
                for (auto* s : svc) if (s->id == kv.first) known = true;
                if (!known) continue;
                for (const auto& e : kv.second) {
                    x += "<programme start=\"" + xmltvTime(e.start) + "\" stop=\"" + xmltvTime(e.end()) + "\" channel=\"" + std::to_string(kv.first) + ".onair\"><title>" + xmlEscape(e.title) + "</title>";
                    if (!e.text.empty() || !e.extended.empty()) x += "<desc>" + xmlEscape(e.extended.empty() ? e.text : e.extended) + "</desc>";
                    x += "</programme>\n";
                }
            }
            x += "</tv>\n";
            reply(200, "application/xml", x);
        } else if (path == "/discover.json") {
            reply(200, "application/json", "{\"FriendlyName\":\"" + jsonEscape(cfg.name) + "\",\"Manufacturer\":\"OnAir\",\"ModelNumber\":\"HDTC-2US\",\"FirmwareName\":\"hdhomerun_atsc\",\"FirmwareVersion\":\"20240101\",\"DeviceID\":\"0A1B2C3D\",\"DeviceAuth\":\"onair\",\"TunerCount\":1,\"BaseURL\":\"" + base + "\",\"LineupURL\":\"" + base + "/lineup.json\"}");
        } else if (path == "/lineup_status.json") {
            reply(200, "application/json", "{\"ScanInProgress\":0,\"ScanPossible\":0,\"Source\":\"Antenna\",\"SourceList\":[\"Antenna\"]}");
        } else if (path == "/lineup.json") {
            std::string j = "[";
            for (size_t i = 0; i < svc.size(); i++) {
                if (i) j += ",";
                j += "{\"GuideNumber\":\"" + std::to_string(svc[i]->lcn ? svc[i]->lcn : svc[i]->id) + "\",\"GuideName\":\"" + jsonEscape(svc[i]->name) + "\",\"URL\":\"" + base + "/stream/" + std::to_string(svc[i]->id) + keyQ + "\"}";
            }
            reply(200, "application/json", j + "]");
        } else if (path == "/" || path == "/index.html") {
            std::string h = "<!doctype html><meta charset=utf-8><title>OnAir</title><body style=\"font:15px monospace;background:#101214;color:#d0d4d6;padding:20px\"><h2>OnAir network tuner</h2>";
            h += "<p><a style=color:#7fb8cf href=\"/lineup.m3u" + keyQ + "\">lineup.m3u</a> &middot; <a style=color:#7fb8cf href=\"/guide.xml" + keyQ + "\">guide.xml</a></p>";
            if (svc.empty()) h += "<p>No channels yet: start the receiver and wait for a lock.</p>";
            for (auto* s : svc) h += "<p>" + xmlEscape(s->name) + " &mdash; <a style=color:#7fb8cf href=\"/stream/" + std::to_string(s->id) + keyQ + "\">stream</a> &middot; <a style=color:#7fb8cf href=\"/hls/" + std::to_string(s->id) + "/index.m3u8" + keyQ + "\">HLS</a></p>";
            reply(200, "text/html; charset=utf-8", h);
        } else reply(404, "text/plain", "not found");
    }

    void acceptLoop() {
        while (running) {
            sockaddr_in ca{}; socklen_t cl = (socklen_t)sizeof ca;
            sock_t fd = accept(lsock, (sockaddr*)&ca, &cl);
            if (fd == kBadSock) { if (!running) break; continue; }
            live++;
            std::thread([this, fd] { serve(fd); live--; }).detach();
        }
    }
};

NetTuner::NetTuner(Engine& e) : p_(new Impl(e)) {}
NetTuner::~NetTuner() { stop(); delete p_; }

bool NetTuner::start(const NetTunerConfig& c) {
    stop();
    netInit();
    p_->cfg = c; p_->error.clear();
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) { p_->error = "cannot create a socket"; return false; }
    int one = 1;
#ifndef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   // on Windows the option means something else: leave the default
#endif
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)c.port);
    a.sin_addr.s_addr = htonl(c.localOnly ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof a) < 0 || listen(s, 16) < 0) { p_->error = "port " + std::to_string(c.port) + " is not available"; sockClose(s); return false; }
    p_->lsock = s; p_->running = true;
    p_->acceptor = std::thread([this] { p_->acceptLoop(); });
    p_->eng.setPacketTap([this](const uint8_t* pk, size_t n, const TsSnapshot* sn) { p_->tap(pk, n, sn); });
    return true;
}

void NetTuner::stop() {
    if (!p_->running.exchange(false)) return;
    p_->eng.setPacketTap(nullptr);
    p_->stopHls();
    sockShutdown(p_->lsock); sockClose(p_->lsock); p_->lsock = kBadSock;
    if (p_->acceptor.joinable()) p_->acceptor.join();
    std::unique_lock<std::mutex> lk(p_->mu);
    for (auto& c : p_->clients) { sockShutdown(c->fd); c->cv.notify_all(); }
    lk.unlock();
    for (int i = 0; i < 400 && p_->live > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

NetTunerStats NetTuner::stats() const {
    NetTunerStats s;
    s.running = p_->running; s.port = p_->cfg.port; s.bytesSent = p_->sent;
    std::lock_guard<std::mutex> lk(p_->mu);
    s.clients = (int)p_->clients.size(); s.error = p_->error;
    return s;
}

std::vector<std::string> NetTuner::addresses() const {
    std::vector<std::string> r;
    if (p_->cfg.localOnly) { r.push_back("127.0.0.1"); return r; }
#ifdef _WIN32
    ULONG len = 16384;
    std::vector<unsigned char> buf(len);
    auto* ad = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, ad, &len) == ERROR_BUFFER_OVERFLOW) { buf.resize(len); ad = (IP_ADAPTER_ADDRESSES*)buf.data(); }
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, ad, &len) == NO_ERROR) {
        for (auto* a = ad; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                char b[64]; inet_ntop(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, b, sizeof b);
                r.push_back(b);
            }
        }
    }
#else
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (auto* i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & 0x8 /*IFF_LOOPBACK*/)) continue;
            char b[64]; inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, b, sizeof b);
            r.push_back(b);
        }
        freeifaddrs(ifa);
    }
#endif
    return r;
}

} // namespace dect2
