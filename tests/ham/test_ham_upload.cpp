// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham logbook uploads against a fake HTTP server on loopback (no real network): request shapes, answers, retry queue, persistence, nothing sent while off.
#include "dect2/ham/ham_upload.h"
#include "../../core/src/net/tcp_client.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace dect2;
using namespace dect2::ham;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Fake {
    sock_t ls = kBadSock; int port = 0;
    std::atomic<int> hits{0}; std::atomic<bool> quit{false};
    std::mutex mu; std::vector<std::string> reqs;           // "path\nbody"
    int status = 200; std::string override_;
    std::thread th;
    static std::string answerFor(const std::string& path) {
        if (path == "/api") return "RESULT=OK&LOGID=1";
        if (path == "/realtime.php") return "OK";
        return "Result: 1 out of 1 records added";
    }
    void start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 8);
        socklen_t l = sizeof a; getsockname(ls, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (!quit) {
                fd_set r; FD_ZERO(&r); FD_SET(ls, &r); timeval tv{0, 100000};
                if (select((int)ls + 1, &r, nullptr, nullptr, &tv) <= 0) continue;
                sock_t c = accept(ls, nullptr, nullptr);
                if (c == kBadSock) continue;
                sockRecvTimeout(c, 2000);
                std::string in; char b[4096]; size_t need = 0;
                for (;;) {
                    long long k = sockRecv(c, b, sizeof b); if (k <= 0) break; in.append(b, (size_t)k);
                    const size_t h = in.find("\r\n\r\n");
                    if (h != std::string::npos) {
                        if (!need) { const size_t cl = in.find("Content-Length: "); need = h + 4 + (cl == std::string::npos ? 0 : (size_t)atoi(in.c_str() + cl + 16)); }
                        if (in.size() >= need) break;
                    }
                }
                hits++;
                const size_t sp = in.find(' '), sp2 = in.find(' ', sp + 1), h = in.find("\r\n\r\n");
                const std::string path = in.substr(sp + 1, sp2 - sp - 1), body = h == std::string::npos ? "" : in.substr(h + 4);
                { std::lock_guard<std::mutex> lk(mu); reqs.push_back(path + "\n" + body); }
                const std::string ans = override_.empty() ? answerFor(path) : override_;
                const std::string resp = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(ans.size()) + "\r\nConnection: close\r\n\r\n" + ans;
                sockSendAll(c, resp.data(), resp.size()); sockClose(c);
            }
        });
    }
    void stop() { quit = true; if (th.joinable()) th.join(); sockClose(ls); }
    // The test transport: same wire format, to the fake instead of the real host (the uploader already checked the URL is https).
    UpResponse post(const std::string& url, const std::string& body) {
        UpResponse r; std::string err;
        const size_t p = url.find('/', 8);
        const std::string path = p == std::string::npos ? "/" : url.substr(p);
        sock_t s = tcpConnect("127.0.0.1", port, 3000, err);
        if (s == kBadSock) { r.err = err; return r; }
        const std::string q = "POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        sockSendAll(s, q.data(), q.size()); sockRecvTimeout(s, 3000);
        std::string in; char b[2048];
        for (;;) { long long k = sockRecv(s, b, sizeof b); if (k <= 0) break; in.append(b, (size_t)k); }
        sockClose(s);
        if (in.compare(0, 5, "HTTP/") != 0) { r.err = "no response"; return r; }
        r.status = atoi(in.c_str() + 9);
        const size_t h = in.find("\r\n\r\n"); r.body = h == std::string::npos ? "" : in.substr(h + 4);
        return r;
    }
};

int main() {
    Fake f; f.start();
    HamUploader up([&f](const std::string& u, const std::string& b) { return f.post(u, b); });
    QsoRecord q; q.call = "DL1XYZ"; q.grid = "JO62"; q.mode = "FT8"; q.rstSent = "-05"; q.rstRcvd = "-10"; q.myCall = "LY1ABC"; q.myGrid = "KO24"; q.freqHz = 14074000; q.startUtc = q.endUtc = 1700000000;
    const std::string path = "test_ham_upload_queue.tsv"; std::remove(path.c_str());
    up.load(path);

    // everything off: logged, polled, nothing sent
    up.enqueue(q, 1000);
    CHECK(up.poll(1010) == 0 && f.hits == 0, "nothing sent while off");
    UpConfig c; c.qrzKey = "SECRETKEY"; c.clubEmail = "a@b.c"; c.clubPassword = "pw&1"; c.clubApiKey = "apikey"; c.eqslUser = "LY1ABC"; c.eqslPassword = "eqpw";
    up.setConfig(c);
    CHECK(up.poll(1020) == 0 && f.hits == 0, "credentials alone do not switch a service on");
    c.autoUpload = true; up.setConfig(c);
    CHECK(up.poll(1030) == 0 && f.hits == 0, "services still off: nothing sent even with auto upload");
    c.on[0] = c.on[1] = c.on[2] = true; up.setConfig(c);
    CHECK(up.poll(1040) == 0 && f.hits == 0, "a QSO logged while the services were off is not sent by auto upload");
    up.uploadNow(1050);
    CHECK(up.poll(1060) == 3 && f.hits == 3, "upload now sends to all three: %d", f.hits.load());
    for (const UpItem& i : up.items()) for (int k = 0; k < 3; k++) CHECK(i.s[k].st == 'u', "uploaded %d", k);
    {
        std::lock_guard<std::mutex> lk(f.mu);
        bool qrz = false, club = false, eq = false;
        for (const auto& r : f.reqs) {
            const std::string line = r.substr(0, r.find('\n'));
            if (line == "/api") qrz = r.find("KEY=SECRETKEY") != std::string::npos && r.find("ACTION=INSERT") != std::string::npos && r.find("ADIF=") != std::string::npos && r.find("DL1XYZ") != std::string::npos;
            if (line == "/realtime.php") club = r.find("email=a%40b.c") != std::string::npos && r.find("password=pw%261") != std::string::npos && r.find("callsign=LY1ABC") != std::string::npos && r.find("api=apikey") != std::string::npos;
            if (line.find("ImportADIF") != std::string::npos) eq = r.find("EQSL_USER") != std::string::npos && r.find("EQSL_PSWD") != std::string::npos;
            CHECK(line.find("SECRET") == std::string::npos && line.find('?') == std::string::npos, "no secret or query string in the URL");
        }
        CHECK(qrz && club && eq, "request shapes %d %d %d", qrz, club, eq);
    }

    // auto upload after logging, rate limiting (kMinGapSec between requests per service)
    const int h0 = f.hits;
    up.enqueue(q, 2000); up.enqueue(q, 2000);
    CHECK(up.poll(2000) == 3, "auto: first item, one request per service");
    CHECK(up.poll(2001) == 0, "rate limit holds the next ones");
    CHECK(up.poll(2004) == 3 && f.hits == h0 + 6, "next after the gap");

    // transient failure: retried later with a back off; permanent failure: stays failed with a reason
    f.status = 503; f.override_ = "down";
    up.enqueue(q, 3000);
    up.poll(3000);
    auto its = up.items();
    CHECK(its.back().s[0].st == 'p' && its.back().s[0].attempts == 1 && its.back().s[0].nextTry > 3000, "transient: kept for retry");
    const int h1 = f.hits;
    CHECK(up.poll(3010) == 0 && f.hits == h1, "no retry before the back off");
    f.status = 200; f.override_ = "RESULT=FAIL&REASON=invalid api key";
    up.poll(3000 + 60 * 8);
    its = up.items();
    CHECK(its.back().s[0].st == 'f' && its.back().s[0].reason.find("invalid api key") != std::string::npos, "permanent: failed with the reason: %s", its.back().s[0].reason.c_str());
    f.override_.clear();

    // the persisted queue survives a restart
    HamUploader up2([&f](const std::string& u, const std::string& b) { return f.post(u, b); });
    up2.setConfig(c); up2.load(path);
    CHECK(up2.items().size() == up.items().size() && up2.items().back().s[0].st == 'f' && up2.items().back().s[0].reason.find("invalid") != std::string::npos, "reload");
    const int h2 = f.hits;
    up2.uploadNow(5000); up2.poll(5000);
    CHECK(f.hits > h2 && up2.items().back().s[0].st == 'u', "failed ones retried by upload now");

    // disabling again stops everything; the production transport refuses plain http
    UpConfig off = c; off.on[0] = off.on[1] = off.on[2] = false; up2.setConfig(off); up2.enqueue(q, 6000);
    const int h3 = f.hits;
    CHECK(up2.poll(6100) == 0 && f.hits == h3, "off again: nothing sent");
    UpConfig noKey = c; noKey.qrzKey.clear();
    CHECK(!noKey.ready(UpService::Qrz) && c.ready(UpService::Qrz), "ready needs credentials");
    CHECK(buildUpRequest(UpService::Qrz, c, q).url.compare(0, 8, "https://") == 0 && buildUpRequest(UpService::ClubLog, c, q).url.compare(0, 8, "https://") == 0 && buildUpRequest(UpService::Eqsl, c, q).url.compare(0, 8, "https://") == 0, "https only");
    CHECK(!curlTransport()("http://example.com/", "a=b").err.empty(), "plain http refused by the production transport");

    f.stop(); std::remove(path.c_str());
    printf(fails ? "FAILED %d\n" : "ham_upload OK\n", fails);
    return fails != 0;
}
