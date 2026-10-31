// The directory heartbeat: a thread that POSTs a signed JSON body over HTTPS every few minutes, and keeps a status file that says what happened in words a
// person can act on. The decisions about what is sent live in web_directory_proto.cpp; this file is the key file, the HTTP client and the loop.
//
// Every request goes through the `curl` program (the same way the updater downloads, core/src/engine/updater.cpp): it has a hard time limit
// (--max-time), a connect limit, can be stopped at once (stop() never waits for a name lookup), uses the system's trusted roots and proxy settings, and works in
// builds without OpenSSL (the Windows packages). Only when curl is missing is the built-in TLS client used (when the build has it).
#include "web_directory.h"
#include "../../core/src/net/netcompat.h"
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/net/tls_conn.h"
#include "dect2/engine/updater.h"
#include "dect2/mesh/mesh_crypto.h"
#include "server_config.h"
#include "web_directory_proto.h"
#include "web_json.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

namespace onair_web {

namespace {

namespace mc = dect2::meshcrypto;
namespace fs = std::filesystem;

void randomSeed(uint8_t seed[32]) {
#ifndef _WIN32
    std::ifstream u("/dev/urandom", std::ios::binary);
    if (u.read((char*)seed, 32) && u.gcount() == 32) return;
#endif
    std::random_device rd;
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(rd() & 0xFF);
}

long long nowSec() { return (long long)time(nullptr); }

struct Target { std::string url, host, scheme; int port = 443; bool loopbackHttp = false; };

bool splitUrl(const std::string& dirUrl, Target& t, std::string& err) {
    std::string u, e;
    if (!dirNormalizeEndpoint(dirUrl, u, e)) { err = "web_directory_url: " + e; return false; }
    t.url = u;
    t.scheme = u.compare(0, 5, "https") == 0 ? "https" : "http";
    std::string rest = u.substr(t.scheme.size() + 3);
    std::string hp = rest.substr(0, rest.find('/'));
    t.loopbackHttp = t.scheme == "http";
    t.port = t.scheme == "https" ? 443 : 80;
    if (hp.find('[') == std::string::npos) {
        const size_t colon = hp.find(':');
        if (colon != std::string::npos) { t.port = atoi(hp.c_str() + colon + 1); hp.resize(colon); }
    }
    if (t.port <= 0 || t.port > 65535) { err = "web_directory_url: bad port"; return false; }
    t.host = hp;
    return true;
}

// ---------------------------------------------------------------- the HTTP client
struct Http {
    int status = 0;                  // 0: no answer
    std::string body, date, errClass, err;
    long ms = 0;
};

// curl's exit codes to our error classes
std::string classOfCurl(int rc) {
    switch (rc) {
        case 6: case 5: return "dns";
        case 7: return "refused";
        case 28: return "timeout";
        case 35: case 52: case 55: case 56: return "tls";
        case 51: case 58: case 59: case 60: case 64: case 77: case 83: case 90: return "tls_cert";
        case 1: case 3: return "proto";
        case -2: return "no_curl";
        default: return "other";
    }
}

// The headers of the last response in curl -i output (a proxy adds its own "200 Connection established" block first), and the body after them.
bool splitResponse(const std::string& raw, Http& h) {
    size_t pos = 0, hdrStart = std::string::npos, bodyAt = 0;
    while (raw.compare(pos, 5, "HTTP/") == 0) {
        const size_t endH = raw.find("\r\n\r\n", pos);
        if (endH == std::string::npos) return false;
        hdrStart = pos;
        bodyAt = endH + 4;
        pos = bodyAt;
        const int st = atoi(raw.c_str() + hdrStart + 9);
        if (st != 100 && raw.compare(pos, 5, "HTTP/") != 0) break;   // the final block
    }
    if (hdrStart == std::string::npos) return false;
    h.status = atoi(raw.c_str() + hdrStart + 9);
    const std::string hdr = raw.substr(hdrStart, bodyAt - hdrStart);
    for (size_t i = 0; i < hdr.size();) {
        size_t e = hdr.find("\r\n", i);
        if (e == std::string::npos) e = hdr.size();
        const std::string line = hdr.substr(i, e - i);
        if (line.size() > 5 && strncasecmp(line.c_str(), "date:", 5) == 0) { size_t a = 5; while (a < line.size() && line[a] == ' ') a++; h.date = line.substr(a); }
        i = e + 2;
    }
    h.body = raw.substr(bodyAt, 8192);
    return true;
}

Http viaCurl(const std::string& method, const std::string& url, const std::string& body, int timeoutSec, const std::string& tmpDir, bool allowPlain, const std::atomic<bool>& cancel,
             const std::string& ua) {
    Http h;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> a = {"-sS", "-i", "--connect-timeout", std::to_string(std::min(10, timeoutSec)), "--max-time", std::to_string(timeoutSec), "--max-redirs", "0",
                                  "--proto", allowPlain ? "=http,https" : "=https", "-A", ua, "-H", "Accept: application/json"};
    std::string bodyFile;
    if (method == "POST") {
        std::error_code ec;
        fs::create_directories(tmpDir, ec);
        bodyFile = (fs::path(tmpDir) / ("hb-" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007ULL) + ".json")).string();
        { std::string werr; if (!onair_server::writeHostFile(bodyFile, body, werr, false)) { h.errClass = "other"; h.err = "cannot write a temporary file in " + tmpDir; return h; } }
        a.push_back("-X"); a.push_back("POST");
        a.push_back("-H"); a.push_back("Content-Type: application/json");
        a.push_back("--data-binary"); a.push_back("@" + bodyFile);
    }
    a.push_back(url);
    std::string out;
    const int rc = dect2::curlRun(a, &out, cancel);
    if (!bodyFile.empty()) onair_server::removeHostFile(bodyFile);
    h.ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (rc == -1) { h.errClass = "cancelled"; h.err = "cancelled"; return h; }
    if (rc != 0) { h.errClass = classOfCurl(rc); h.err = "curl error " + std::to_string(rc); return h; }
    if (!splitResponse(out, h)) { h.errClass = "proto"; h.err = "no valid HTTP answer"; }
    return h;
}

// The built-in TLS client: used only when curl is missing. Blocking name lookup (no time limit of its own), so it is the fallback, not the way.
Http viaBuiltin(const std::string& method, const Target& t, const std::string& path, const std::string& body, const std::string& ua) {
    using namespace dect2;
    Http h;
    if (t.scheme != "https" || !tls::available()) { h.errClass = "no_curl"; return h; }
    std::string err;
    sock_t s = tcpConnect(t.host, t.port, 10000, err);
    if (s == kBadSock) {
        h.errClass = err.find("cannot find") != std::string::npos ? "dns" : err.find("refused") != std::string::npos ? "refused" : err.find("no answer") != std::string::npos ? "timeout" : "other";
        h.err = err;
        return h;
    }
    sockRecvTimeout(s, 10000);
    tls::Ctx* ctx = tls::makeClientCtx(err);
    if (!ctx) { sockClose(s); h.errClass = "tls"; h.err = err; return h; }
    tls::Conn* tc = tls::connect(ctx, s, t.host, err);
    if (!tc) h.errClass = err.find("certificate") != std::string::npos ? "tls_cert" : "tls", h.err = err;
    else {
        const std::string hostHdr = t.port == 443 ? t.host : t.host + ":" + std::to_string(t.port);
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + hostHdr + "\r\nUser-Agent: " + ua + "\r\nAccept: application/json\r\nConnection: close\r\n";
        if (method == "POST") req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n" + body;
        if (tls::write(tc, req.data(), req.size()) == (long)req.size()) {
            std::string raw;
            char buf[2048];
            for (int guard = 0; guard < 16 && raw.size() < 12000; guard++) {
                const long n = tls::read(tc, buf, sizeof buf);
                if (n <= 0) break;
                raw.append(buf, (size_t)n);
            }
            if (!splitResponse(raw, h)) { h.errClass = "proto"; h.err = "no valid HTTP answer"; }
        } else { h.errClass = "tls"; h.err = "could not send"; }
        tls::close(tc);
    }
    tls::freeCtx(ctx);
    sockClose(s);
    return h;
}

Http httpDo(const std::string& method, const Target& t, const std::string& path, const std::string& body, int timeoutSec, const std::string& tmpDir, const std::atomic<bool>& cancel,
            const std::string& ua) {
    Http h = viaCurl(method, t.url.substr(0, t.url.find('/', t.scheme.size() + 3)) + path, body, timeoutSec, tmpDir, t.loopbackHttp, cancel, ua);
    if (h.errClass == "no_curl") {
        Http b = viaBuiltin(method, t, path, body, ua);
        if (b.errClass != "no_curl") return b;
    }
    return h;
}

DirOutcome outcomeOf(const Http& h, const Target& t, long long* skew, bool* haveSkew) {
    *haveSkew = false;
    if (h.status == 0) return dirClassifyNet(h.errClass, h.err, t.host);
    long long d = 0;
    if (!h.date.empty() && dirParseHttpDate(h.date, d)) { *skew = d - nowSec(); *haveSkew = true; }
    return dirClassifyHttp(h.status, h.body, *skew, *haveSkew);
}

// Key file: make it when it is not there. seed (32 bytes), pub hex and id.
bool loadKey(const DirectoryConfig& c, uint8_t seed[32], std::string& pub, std::string& id, std::string& err) {
    if (c.keyFile.empty()) { err = "no place for the directory key"; return false; }
    onair_server::ConfigFile kf;
    kf.load(c.keyFile);
    std::vector<uint8_t> s;
    if (!mc::hexDecode(kf.get("seed"), s) || s.size() != 32) {
        randomSeed(seed);
        kf.set("seed", mc::hexEncode(seed, 32));
        kf.set("public_key", dirPubHex(seed));
        if (!kf.save(c.keyFile, err)) { err = "cannot write the directory key file: " + err; return false; }
        fprintf(stderr, "onair-server: made a new directory key in %s (keep it: it proves this server is the one listed)\n", c.keyFile.c_str());
    } else {
        memcpy(seed, s.data(), 32);
    }
    pub = dirPubHex(seed);
    id = dirIdFromPub(pub);
    return true;
}

class Impl : public DirectoryHeartbeat {
public:
    ~Impl() override { stop(); }

    bool start(const DirectoryConfig& c, std::function<std::string()> info, std::string& err) override {
        if (!prepare(c, std::move(info), err)) return false;
        DirRecord r;
        std::string first = info_();
        if (first.empty()) first = directoryStandInInfo(c_);   // the web server may still be starting: the code does not depend on live numbers
        if (!dirBuildRecord(c_, first, nowSec(), id_, pub_, r, err)) return false;
        const std::string code = dirAck(r, c_.url);
        if (c_.ack != code) {
            printf("onair-server: %s", dirPreview(r, c_.url).c_str());
            err = "web_directory is on, but nothing is sent yet: to confirm exactly this, add  web_directory_ack=" + code + "  to the settings file";
            return false;
        }
        cancel_ = false;
        th_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override {
        cancel_ = true;
        { std::lock_guard<std::mutex> g(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

    bool removeNow(const DirectoryConfig& c, std::string& err) override {
        if (!prepare(c, nullptr, err)) return false;
        DirRecord r;
        r.action = "delete";
        r.id = id_;
        r.pub = pub_;
        r.ts = nowSec();
        cancel_ = false;
        const Http h = httpDo("POST", target_, target_.url.substr(target_.url.find('/', target_.scheme.size() + 3)) + "api/v1/heartbeat", dirBodyJson(r, dirSign(seed_, r)), 15, tmpDir(), cancel_, ua());
        long long skew = 0;
        bool have = false;
        const DirOutcome o = outcomeOf(h, target_, &skew, &have);
        if (!o.ok && !(h.status == 404)) { err = o.message + (o.advice.empty() ? "" : " " + o.advice); return false; }
        return true;
    }

private:
    std::string tmpDir() const { return (fs::path(c_.keyFile).parent_path() / "tmp").string(); }
    std::string ua() const { return "OnAir-Server/" + c_.version; }

    // key file and target; also used by removeNow() without a confirmation
    bool prepare(const DirectoryConfig& c, std::function<std::string()> info, std::string& err) {
        c_ = c;
        info_ = std::move(info);
        if (c_.url.empty()) { err = "web_directory_url is empty"; return false; }
        if (!splitUrl(c_.url, target_, err)) return false;
        if (info_ && c_.publicUrl.empty()) { err = "web_public_url is empty: it is the address listeners open (https://...)"; return false; }
        if (!loadKey(c_, seed_, pub_, id_, err)) return false;
        return true;
    }

    void writeStatus(const DirStatus& s) {
        const std::string path = directoryStatusPath(c_);
        if (path.empty()) return;
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::string werr;
        onair_server::writeHostFile(path, dirStatusJson(s), werr, true);
    }

    void loop() {
        DirStatus st;
        st.id = id_;
        st.url = c_.publicUrl;
        st.directory = c_.url;
        int fails = 0;
        const std::string path = target_.url.substr(target_.url.find('/', target_.scheme.size() + 3)) + "api/v1/heartbeat";
        for (;;) {
            st.state = "sending";
            st.lastTry = nowSec();
            st.attempts++;
            writeStatus(st);
            std::string err;
            DirRecord r;
            DirOutcome o;
            const std::string infoNow = info_ ? info_() : std::string();
            if (info_ && infoNow.empty()) {
                o.code = "local";
                o.message = "The OnAir web server on this computer does not answer, so there is nothing to list.";
                o.advice = "Start sharing first (or wait a few seconds if it is just starting).";
            } else if (!dirBuildRecord(c_, infoNow, nowSec(), id_, pub_, r, err)) {
                o.code = "local";
                o.message = err.empty() ? "The settings for the listing are not valid." : err;
                o.advice = "Fix the setting named above and list again.";
            } else {
                const Http h = httpDo("POST", target_, path, dirBodyJson(r, dirSign(seed_, r)), 15, tmpDir(), cancel_, ua());
                if (h.errClass == "cancelled") return;
                bool have = false;
                long long skew = 0;
                o = outcomeOf(h, target_, &skew, &have);
                st.haveSkew = have;
                st.skewSec = skew;
            }
            st.last = o;
            if (o.ok) {
                if (st.fails > 0 || st.lastOk == 0) printf("onair-server: listed in the directory %s (id %s)\n", c_.url.c_str(), id_.c_str());
                fails = 0;
                st.fails = 0;
                st.lastOk = nowSec();
                st.state = "ok";
            } else {
                fails++;
                st.fails = fails;
                st.state = "error";
                if (fails == 1 || fails % 10 == 0) fprintf(stderr, "onair-server: directory heartbeat failed: %s %s\n", o.message.c_str(), o.advice.c_str());
            }
            const int wait = o.ok ? std::max(30, c_.intervalSec) : dirRetryDelay(fails, o.retrySoon, c_.intervalSec);
            st.nextTry = nowSec() + wait;
            writeStatus(st);
            std::unique_lock<std::mutex> g(m_);
            if (cv_.wait_for(g, std::chrono::seconds(wait), [this] { return stop_; })) return;
        }
    }

    DirectoryConfig c_;
    std::function<std::string()> info_;
    Target target_;
    uint8_t seed_[32] = {};
    std::string pub_, id_;
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::atomic<bool> cancel_{false};
};

std::string stepLine(const char* step, const DirOutcome& o, long ms, bool haveSkew, long long skew) {
    auto q = [](const std::string& v) { return "\"" + jsonEsc(v) + "\""; };
    return std::string("{\"step\":\"") + step + "\",\"ok\":" + (o.ok ? "true" : "false") + ",\"ms\":" + std::to_string(ms) + ",\"http\":" + std::to_string(o.http) + ",\"code\":" + q(o.code) +
           ",\"message\":" + q(o.message) + ",\"advice\":" + q(o.advice) + ",\"skew_sec\":" + (haveSkew ? std::to_string(skew) : std::string("null")) + "}";
}

} // namespace

std::string directoryStandInInfo(const DirectoryConfig& c) {
    return "{\"proto\":1,\"name\":\"" + jsonEsc(c.name) + "\",\"users\":0,\"max_users\":" + std::to_string(c.maxUsers) + "}";
}

std::string directoryStatusPath(const DirectoryConfig& c) {
    if (c.keyFile.empty()) return {};
    return (fs::path(c.keyFile).parent_path() / "web_directory_status.json").string();
}

bool directoryPreview(const DirectoryConfig& c, const std::string& infoJson, std::string& preview, std::string& ack, std::string& err) {
    if (c.url.empty()) { err = "web_directory_url is empty"; return false; }
    Target t;
    if (!splitUrl(c.url, t, err)) return false;
    if (c.publicUrl.empty()) { err = "web_public_url is empty: it is the address listeners open (https://...)"; return false; }
    uint8_t seed[32];
    std::string pub, id;
    if (!loadKey(c, seed, pub, id, err)) return false;
    DirRecord r;
    if (!dirBuildRecord(c, infoJson, nowSec(), id, pub, r, err)) return false;
    preview = dirPreview(r, c.url);
    ack = dirAck(r, c.url);
    return true;
}

std::string fetchLocalInfo(int port, const std::atomic<bool>& cancel) {
    const Http h = viaCurl("GET", "http://127.0.0.1:" + std::to_string(port) + "/web/v1/info", "", 4, "", true, cancel, "OnAir-Server-Local");
    return h.status == 200 ? h.body : std::string();
}

void directoryCheck(const DirectoryConfig& c, int localPort, const std::function<void(const std::string&)>& emit, const std::atomic<bool>& cancel) {
    const std::string ua = "OnAir-Server/" + c.version;
    Target t;
    std::string err;
    {   // 1. the directory answers (a GET of its health route: nothing is registered or changed)
        if (!splitUrl(c.url, t, err)) {
            DirOutcome o;
            o.code = "invalid";
            o.message = err;
            o.advice = "Use an address like https://web.on-air.dev";
            emit(stepLine("directory", o, 0, false, 0));
        } else {
            const std::string path = t.url.substr(t.url.find('/', t.scheme.size() + 3)) + "api/v1/health";
            const Http h = httpDo("GET", t, path, "", 12, (fs::path(c.keyFile.empty() ? "." : c.keyFile).parent_path() / "tmp").string(), cancel, ua);
            bool have = false;
            long long skew = 0;
            DirOutcome o = outcomeOf(h, t, &skew, &have);
            if (h.status == 200 && h.body.find("\"ok\":true") == std::string::npos && h.body.find("\"ok\": true") == std::string::npos) {
                o = DirOutcome{};
                o.code = "other";
                o.http = 200;
                o.message = "The address answered, but it is not an OnAir directory.";
                o.advice = "Check the directory address (the default is https://web.on-air.dev).";
            } else if (o.ok) {
                o.message = "The directory " + t.host + " answers (" + std::to_string(h.ms) + " ms).";
                if (have && (skew > 240 || skew < -240)) { o.ok = false; o = dirClassifyHttp(400, "{\"error\":\"invalid\",\"details\":[\"ts is too far from the directory's clock\"]}", skew, true); }
            }
            emit(stepLine("directory", o, h.ms, have, skew));
        }
    }
    if (cancel) return;
    if (localPort > 0) {   // 2. the local web server
        const Http h = viaCurl("GET", "http://127.0.0.1:" + std::to_string(localPort) + "/web/v1/info", "", 5, "", true, cancel, ua);
        DirOutcome o;
        if (h.status == 200 && h.body.find("\"proto\"") != std::string::npos) { o.ok = true; o.http = 200; o.code = "ok"; o.message = "The OnAir web server on this computer answers."; }
        else {
            o.code = "local";
            o.http = h.status;
            o.message = "The OnAir web server on this computer (port " + std::to_string(localPort) + ") does not answer.";
            o.advice = "Start sharing first. If it was started, it may still be starting, or another program uses port " + std::to_string(localPort) + ".";
        }
        emit(stepLine("local", o, h.ms, false, 0));
    }
    if (cancel) return;
    if (!c.publicUrl.empty()) {   // 3. this server's public address, as a listener's browser would reach it
        std::string pu;
        if (!dirNormalizeUrl(c.publicUrl, pu, err)) {
            DirOutcome o;
            o.code = "invalid";
            o.message = "The public address is not valid: " + err;
            o.advice = "It must be a public https:// address. Use the built-in tunnel for an automatic one.";
            emit(stepLine("public", o, 0, false, 0));
        } else {
            Target pt;
            pt.url = pu;
            const Http h = viaCurl("GET", pu + "web/v1/info", "", 12, "", false, cancel, ua);
            const DirOutcome o = dirClassifyPublic(h.status, h.body.find("\"proto\"") != std::string::npos, h.errClass, pu);
            emit(stepLine("public", o, h.ms, false, 0));
        }
    }
    emit("{\"step\":\"done\",\"ok\":true,\"ms\":0,\"http\":0,\"code\":\"done\",\"message\":\"\",\"advice\":\"\",\"skew_sec\":null}");
}

DirectoryHeartbeat* makeDirectoryHeartbeat() { return new Impl; }

} // namespace onair_web
