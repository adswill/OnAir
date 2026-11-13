// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The update mirror: the address templates, the SHA256SUMS parser, and the order of the sources against two fake HTTP servers on 127.0.0.1
// (GitHub and the mirror): the mirror is never asked unless the user turned it on, it answers only after GitHub failed, a package only the
// mirror vouches for needs the explicit yes, GitHub's checksum wins over the mirror's, and a wrong checksum is refused.
#include "dect2/engine/updater.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#ifndef _WIN32
// A one-thread HTTP server: handler(path) -> (status, body). Logs every request path.
struct Fake {
    int fd = -1, port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::mutex mu;
    std::vector<std::string> log;
    std::function<std::pair<int, std::string>(const std::string&)> handler;
    void start() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(fd, (sockaddr*)&a, sizeof a); listen(fd, 8);
        socklen_t l = sizeof a; getsockname(fd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (!stop) {
                fd_set s; FD_ZERO(&s); FD_SET(fd, &s); timeval tv{0, 100000};
                if (select(fd + 1, &s, nullptr, nullptr, &tv) <= 0) continue;
                const int c = accept(fd, nullptr, nullptr);
                if (c < 0) continue;
                std::string req; char b[2048];
                while (req.find("\r\n\r\n") == std::string::npos) { const ssize_t n = recv(c, b, sizeof b, 0); if (n <= 0) break; req.append(b, (size_t)n); }
                const size_t p1 = req.find(' '), p2 = req.find(' ', p1 + 1);
                const std::string path = p1 == std::string::npos ? "" : req.substr(p1 + 1, p2 - p1 - 1);
                { std::lock_guard<std::mutex> lk(mu); log.push_back(path); }
                auto r = handler(path);
                std::string resp = "HTTP/1.1 " + std::to_string(r.first) + " X\r\nContent-Length: " + std::to_string(r.second.size()) + "\r\nConnection: close\r\n\r\n" + r.second;
                size_t off = 0; while (off < resp.size()) { const ssize_t n = send(c, resp.data() + off, resp.size() - off, 0); if (n <= 0) break; off += (size_t)n; }
                close(c);
            }
        });
    }
    size_t count() { std::lock_guard<std::mutex> lk(mu); return log.size(); }
    ~Fake() { stop = true; if (th.joinable()) th.join(); if (fd >= 0) close(fd); }
};

static bool waitPhase(Updater& u, std::function<bool(Updater::Phase)> ok, int ms = 20000) {
    for (int i = 0; i < ms / 20; i++) { if (ok(u.status().phase)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return false;
}
#endif

int main() {
    // ---- the address templates
    const std::string api = "https://api.github.com/repos/adswill/OnAir/releases?per_page=12";
    const std::string asset = "https://github.com/adswill/OnAir/releases/download/v1.0.0/a.tar.gz";
    CHECK(mirrorUrl("https://m.example/{url}", api) == "https://m.example/" + api, "{url} template");
    CHECK(mirrorUrl("https://m.example/p?u={urlenc}&x=1", asset) == "https://m.example/p?u=https%3A%2F%2Fgithub.com%2Fadswill%2FOnAir%2Freleases%2Fdownload%2Fv1.0.0%2Fa.tar.gz&x=1", "{urlenc} template");
    CHECK(mirrorUrl("https://m.example/gh", asset) == "https://m.example/gh/" + asset && mirrorUrl("https://m.example/gh/", asset) == "https://m.example/gh/" + asset, "base address");
    CHECK(mirrorUrl("http://m.example/{url}", api).empty(), "http mirror refused");
    CHECK(mirrorUrl("{url}", api).empty() && mirrorUrl("", api).empty() && mirrorUrl("ftp://m.example/{url}", api).empty(), "empty / not https refused");
    CHECK(mirrorUrl("https://m.example/ {url}", api).empty() && mirrorUrl("https://m.example/\n{url}", api).empty(), "spaces and control characters refused");
    CHECK(mirrorUrl("https://m.example/{url}", "https://evil.example/x").empty() && mirrorUrl("https://m.example/{url}", "https://github.com/other/repo/releases/download/x").empty(), "only the project's own addresses go to a mirror");
    CHECK(mirrorUrl("https://m.example/" + std::string(500, 'a') + "/{url}", api).empty(), "too long refused");
    CHECK(isOfficialUrl(api) && isOfficialUrl(asset) && !isOfficialUrl("https://github.com/adswill/OnAir-evil/releases/x") && !isOfficialUrl("https://api.github.com.evil.example/repos/adswill/OnAir/releases"), "official addresses");
    // ---- who may be contacted
    MirrorConfig c; c.tmpl = "https://m.example/{url}";
    CHECK(!mirrorEnabled(c), "default: the mirror is off");
    c.fallback = true; CHECK(mirrorEnabled(c), "fallback on");
    c.source = UpdateSource::GitHubOnly; CHECK(!mirrorEnabled(c), "GitHub only: never");
    c.source = UpdateSource::MirrorOnly; c.fallback = false; CHECK(mirrorEnabled(c), "mirror only");
    c.tmpl.clear(); CHECK(!mirrorEnabled(c), "no address, no mirror");
    // ---- SHA256SUMS
    const std::string h1(64, 'a'), h2 = "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string sums = h1 + "  one.tar.gz\r\n" + h2 + " *two.deb\nnot a line\n";
    CHECK(checksumFor(sums, "one.tar.gz") == h1 && checksumFor(sums, "two.deb") == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" && checksumFor(sums, "three").empty() && checksumFor(sums, "one.tar").empty(), "SHA256SUMS lines");

#ifndef _WIN32
    const std::string pkg = "fake package bytes 0123456789";
    const std::string good = sha256Bytes(pkg.data(), pkg.size());
    const std::string name = std::string("onair_9.9.9_") + (thisArch() == "arm64" || thisArch() == "aarch64" ? "arm64" : "amd64") + ".deb";
    auto releaseJson = [&](const std::string& digest, const std::string& dl) {
        return "[{\"tag_name\":\"v9.9.9\",\"name\":\"x\",\"draft\":false,\"prerelease\":false,\"body\":\"n\",\"assets\":[{\"name\":\"" + name + "\",\"size\":" + std::to_string(pkg.size()) +
               ",\"browser_download_url\":\"" + dl + "\"" + (digest.empty() ? "" : ",\"digest\":\"sha256:" + digest + "\"") + "}]}]";
    };
    auto isDeb = [](Updater& u) { u.setExecutable("/usr/bin/onair", (int)InstallKind::LinuxDeb); };

    // 1. GitHub down, mirror off (the default): the mirror is never contacted, the check fails
    {
        Fake gh, mi; gh.handler = [](const std::string&) { return std::make_pair(500, std::string("no")); };
        mi.handler = [&](const std::string&) { return std::make_pair(200, releaseJson(good, "x")); };
        gh.start(); mi.start();
        Updater u("1.0.0"); isDeb(u);
        u.setApiUrl("http://127.0.0.1:" + std::to_string(gh.port) + "/rel");
        MirrorConfig m; m.tmpl = "http://127.0.0.1:" + std::to_string(mi.port) + "/{url}"; m.fallback = false;
        u.setMirror(m);
        u.check(false);
        CHECK(waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Failed || p == Updater::Phase::Available; }) && u.status().phase == Updater::Phase::Failed, "mirror off: the check fails");
        CHECK(mi.count() == 0 && gh.count() >= 1, "mirror off: nothing is sent to the mirror");
        // GitHub only, even with the box ticked
        m.fallback = true; m.source = UpdateSource::GitHubOnly; u.setMirror(m); u.check(false);
        waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Failed; }); std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(mi.count() == 0, "GitHub only: still nothing sent to the mirror");
    }
    // 2. GitHub down, fallback on: the mirror answers, GitHub cannot confirm the checksum -> needs the explicit yes; no download without it
    {
        Fake gh, mi;
        gh.handler = [](const std::string&) { return std::make_pair(503, std::string("down")); };
        std::string ghUrl;
        mi.handler = [&](const std::string& path) {
            if (path.find("/rel") != std::string::npos) return std::make_pair(200, releaseJson(good, "http://127.0.0.1:" + std::to_string(gh.port) + "/dl/" + name));
            if (path.find("/dl/") != std::string::npos) return std::make_pair(200, pkg);
            return std::make_pair(404, std::string());
        };
        gh.start(); mi.start();
        Updater u("1.0.0"); isDeb(u);
        u.setApiUrl("http://127.0.0.1:" + std::to_string(gh.port) + "/rel");
        u.setReleaseBase("http://127.0.0.1:" + std::to_string(gh.port) + "/dl/");
        MirrorConfig m; m.tmpl = "http://127.0.0.1:" + std::to_string(mi.port) + "/{url}"; m.fallback = true;
        u.setMirror(m);
        u.check(false);
        CHECK(waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Failed || p == Updater::Phase::Available; }) && u.status().phase == Updater::Phase::Available, "fallback on: the mirror answers after GitHub failed");
        Updater::Status st = u.status();
        CHECK(st.viaMirror && st.release.fromMirror && !st.release.officialVerified && st.mirrorUnverified, "marked as mirror-only, unverified");
        const size_t before = mi.count();
        u.download(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(u.status().phase == Updater::Phase::Available && mi.count() == before, "no download without the explicit yes");
        u.download(true);
        CHECK(waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Ready || p == Updater::Phase::Failed; }), "confirmed download ends");
        CHECK(u.status().phase == Updater::Phase::Ready, "confirmed, checksum from the mirror's list matches: %s", u.status().error.c_str());
        // order: GitHub's asset address first, the mirror second
        bool ghFirst = false; { std::lock_guard<std::mutex> lk(gh.mu); for (auto& p : gh.log) if (p.find("/dl/" + name) != std::string::npos) ghFirst = true; }
        CHECK(ghFirst, "GitHub is tried first for the file");
    }
    // 3. the mirror's checksum is wrong: refused (confirmed or not)
    {
        Fake gh, mi;
        gh.handler = [](const std::string&) { return std::make_pair(503, std::string()); };
        mi.handler = [&](const std::string& path) {
            if (path.find("/rel") != std::string::npos) return std::make_pair(200, releaseJson(std::string(64, '0'), "http://127.0.0.1:" + std::to_string(gh.port) + "/dl/" + name));
            return std::make_pair(200, pkg);
        };
        gh.start(); mi.start();
        Updater u("1.0.0"); isDeb(u);
        u.setApiUrl("http://127.0.0.1:" + std::to_string(gh.port) + "/rel");
        MirrorConfig m; m.tmpl = "http://127.0.0.1:" + std::to_string(mi.port) + "/{url}"; m.source = UpdateSource::MirrorOnly;
        u.setMirror(m);
        u.check(false);
        waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Available; });
        CHECK(gh.count() == 0, "mirror only: GitHub is never contacted");
        u.download(true);
        CHECK(waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Failed || p == Updater::Phase::Ready; }) && u.status().phase == Updater::Phase::Failed && u.status().error.find("checksum") != std::string::npos, "checksum mismatch rejected: %s", u.status().error.c_str());
    }
    // 4. GitHub's own checksum is reachable (SHA256SUMS) while the list came through the mirror: confirmed without the yes; a lying mirror is caught
    for (int lie = 0; lie < 2; lie++) {
        Fake gh, mi;
        const std::string officialSum = good + "  " + name + "\n";
        gh.handler = [&](const std::string& path) {
            if (path.find("/SHA256SUMS") != std::string::npos) return std::make_pair(200, officialSum);
            return std::make_pair(503, std::string());   // the API is down, the release files are up
        };
        mi.handler = [&](const std::string& path) {
            if (path.find("/rel") != std::string::npos) return std::make_pair(200, releaseJson(lie ? std::string(64, 'b') : good, "http://127.0.0.1:" + std::to_string(gh.port) + "/dl/" + name));
            return std::make_pair(200, pkg);
        };
        gh.start(); mi.start();
        Updater u("1.0.0"); isDeb(u);
        u.setApiUrl("http://127.0.0.1:" + std::to_string(gh.port) + "/rel");
        u.setReleaseBase("http://127.0.0.1:" + std::to_string(gh.port) + "/dl/");
        MirrorConfig m; m.tmpl = "http://127.0.0.1:" + std::to_string(mi.port) + "/{url}"; m.fallback = true;
        u.setMirror(m);
        u.check(false);
        waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Available || p == Updater::Phase::Failed; });
        if (!lie) {
            CHECK(u.status().phase == Updater::Phase::Available && u.status().release.officialVerified && !u.status().mirrorUnverified, "SHA256SUMS from GitHub confirms the mirror's list");
            u.download(false);
            CHECK(waitPhase(u, [](Updater::Phase p) { return p == Updater::Phase::Ready || p == Updater::Phase::Failed; }) && u.status().phase == Updater::Phase::Ready, "verified download needs no extra yes: %s", u.status().error.c_str());
        } else {
            CHECK(u.status().phase == Updater::Phase::Failed && u.status().error.find("does not match GitHub") != std::string::npos, "a mirror that disagrees with GitHub is refused: %s", u.status().error.c_str());
        }
    }
#endif
    if (fails == 0) printf("OK\n");
    return fails ? 1 : 0;
}
