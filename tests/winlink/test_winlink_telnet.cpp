// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Winlink over Telnet, against a mock CMS on 127.0.0.1 in this process only (never the real CMS): the "Callsign :" / "Password :"
// login, then a whole B2F session with the secure login; and the setting that keeps Telnet off by default.
#include "winlink_harness.h"
#include "../../core/src/net/tcp_client.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
using namespace wltest;
#include "../common/test_util.h"

int main() {
    netInit();
    const std::time_t t0 = 1700000000;
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    CHECK(ls != kBadSock && bind(ls, (const sockaddr*)&a, sizeof a) == 0 && listen(ls, 1) == 0, "listen on 127.0.0.1");
    socklen_t al = sizeof a;
    getsockname(ls, (sockaddr*)&a, &al);
    const int port = ntohs(a.sin_port);
    MockGateway gw;
    gw.waiting.push_back(msg("LY1GW", kMyCall, "telnet test", "Hello over TCP\r\n" + std::string(2000, 't'), t0));
    std::string loginCall, loginPw;
    std::atomic<bool> serverDone{false};
    std::thread srv([&] {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c == kBadSock) { serverDone = true; return; }
        sockRecvTimeout(c, 200);
        auto readLine = [&](std::string& out) {
            char ch;
            for (int i = 0; i < 200; i++) {
                const long long k = sockRecv(c, &ch, 1);
                if (k == 1) { if (ch == '\r') return true; if (ch != '\n') out += ch; }
                else if (k == 0) return false;
            }
            return false;
        };
        const char* p1 = "Callsign :\r\n";
        sockSendAll(c, p1, strlen(p1));
        readLine(loginCall);
        const char* p2 = "Password :\r\n";
        sockSendAll(c, p2, strlen(p2));
        readLine(loginPw);
        gw.begin([&](const uint8_t* d, size_t n) { sockSendAll(c, d, n); });
        gw.s->start();
        char buf[1024];
        for (int i = 0; i < 300 && !gw.s->done() && !gw.s->failed(); i++) {
            const long long k = sockRecv(c, buf, sizeof buf);
            if (k > 0) gw.s->onBytes((const uint8_t*)buf, (size_t)k);
            else if (k == 0) break;
        }
        // the client closes after FQ
        for (int i = 0; i < 20; i++) { if (sockRecv(c, buf, sizeof buf) == 0) break; }
        sockClose(c);
        serverDone = true;
    });
    const std::string dir = tempDir("telnet");
    Mailbox mb(dir);
    mb.save(Folder::Outbox, msg(kMyCall, "friend@example.com", "from telnet", "sent over TCP", t0 + 1));
    WinlinkStation st(mb);
    StationConfig sc;
    sc.mycall = kMyCall; sc.password = kPassword; sc.locator = "KO24";
    sc.gateway.transport = Transport::Telnet; sc.gateway.host = "127.0.0.1"; sc.gateway.port = port;
    std::string err;
    CHECK(StationConfig().telnetEnabled == false, "Telnet is off by default");
    CHECK(!st.start(sc, 0, err), "refused while Telnet is off");
    sc.telnetEnabled = true;
    CHECK(st.start(sc, 0, err), "start: %s", err.c_str());
    const auto tStart = std::chrono::steady_clock::now();
    double now = 0;
    while (st.running() && std::chrono::steady_clock::now() - tStart < std::chrono::seconds(30)) {
        st.tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        now += 0.005;
    }
    if (st.running()) st.stop(now), st.stop(now);
    srv.join();
    sockClose(ls);
    printf("telnet: %s, %d in, %d out, login call \"%s\"\n", st.lastOk() ? "ok" : st.lastError().c_str(), st.received(), st.sent(), loginCall.c_str());
    CHECK(st.lastOk() && st.received() == 1 && st.sent() == 1, "telnet session: %s", st.lastError().c_str());
    CHECK(loginCall == kMyCall && loginPw == "CMSTelnet", "login lines \"%s\" \"%s\"", loginCall.c_str(), loginPw.c_str());
    CHECK(gw.s && gw.s->loginOk() && gw.got.size() == 1 && gw.got[0].subject() == "from telnet", "the mock CMS got the message");
    CHECK(mb.list(Folder::Inbox).size() == 1 && mb.list(Folder::Sent).size() == 1 && mb.list(Folder::Outbox).empty(), "mailbox folders after the session");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return testSummary("winlink telnet");
}
