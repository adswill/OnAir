// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Discord Rich Presence: frame encoding, JSON escaping, and a fake Discord IPC server (unix only).
#include "platform/discord_rpc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#ifndef _WIN32
struct Frame { uint32_t op; std::string body; };
static bool rd(int fd, void* b, size_t n, int ms) {
    char* p = (char*)b;
    while (n) { pollfd pf{fd, POLLIN, 0}; if (poll(&pf, 1, ms) <= 0) return false; ssize_t g = recv(fd, p, n, 0); if (g <= 0) return false; p += g; n -= (size_t)g; }
    return true;
}
static bool readFrame(int fd, Frame& f, int ms) {
    unsigned char h[8];
    if (!rd(fd, h, 8, ms)) return false;
    f.op = h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24;
    uint32_t n = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
    f.body.assign(n, '\0');
    return n == 0 || rd(fd, &f.body[0], n, ms);
}
static void reply(int fd, uint32_t op, const char* j) { std::string s = discord::encodeFrame(op, j); send(fd, s.data(), s.size(), 0); }

static void fakeServerTest() {
    char dir[] = "/tmp/odrXXXXXX";
    CHECK(mkdtemp(dir));
    setenv("TMPDIR", dir, 1); unsetenv("XDG_RUNTIME_DIR");
    std::string path = std::string(dir) + "/discord-ipc-0";
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un sa{}; sa.sun_family = AF_UNIX; strcpy(sa.sun_path, path.c_str());
    CHECK(bind(ls, (sockaddr*)&sa, sizeof sa) == 0 && listen(ls, 1) == 0);

    std::vector<Frame> got;
    std::atomic<int> n{0};
    std::atomic<bool> done{false};
    std::thread srv([&] {
        pollfd pf{ls, POLLIN, 0};
        if (poll(&pf, 1, 5000) <= 0) { done = true; return; }
        int c = accept(ls, nullptr, nullptr);
        Frame f;
        while (readFrame(c, f, 4000)) {
            got.push_back(f); n = (int)got.size();
            if (f.op == 0) reply(c, 1, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\"}");
            else if (f.op == 1) reply(c, 1, "{\"cmd\":\"SET_ACTIVITY\"}");
            else if (f.op == 2) break;
        }
        close(c); done = true;
    });
    {
        discord::Rpc rpc;
        rpc.setIntervals(0.0, 0.2);
        rpc.setAppId("12345");
        discord::Presence p; p.enabled = true; p.running = true; p.mode = "dPMR / PMR446"; p.freq = "446.003 MHz"; p.startUnix = 1700000000;
        rpc.set(p);
        for (int i = 0; i < 50 && n < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        discord::Presence off;   // disabled
        rpc.set(off);
        for (int i = 0; i < 50 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    srv.join();
    CHECK(got.size() == 4);
    if (got.size() == 4) {
        CHECK(got[0].op == 0 && got[0].body == "{\"v\":1,\"client_id\":\"12345\"}");
        CHECK(got[1].op == 1);
        CHECK(got[1].body.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos);
        CHECK(got[1].body.find("\"details\":\"Listening to dPMR / PMR446\"") != std::string::npos);
        CHECK(got[1].body.find("\"state\":\"446.003 MHz\"") != std::string::npos);
        CHECK(got[1].body.find("\"timestamps\":{\"start\":1700000000}") != std::string::npos);
        CHECK(got[1].body.find("\"large_image\":\"onair\"") != std::string::npos);
        CHECK(got[2].op == 1 && got[2].body.find("\"activity\":null") != std::string::npos);
        CHECK(got[3].op == 2);
    }
    close(ls); unlink(path.c_str()); rmdir(dir);
}
#endif

int main() {
    CHECK(discord::jsonEscape("a\"b\\c\n\t\x01") == "a\\\"b\\\\c\\n\\t\\u0001");
    CHECK(discord::jsonEscape("\xc3\x9cn") == "\xc3\x9cn");
    std::string f = discord::encodeFrame(1, "abc");
    CHECK(f.size() == 11 && f[0] == 1 && f[1] == 0 && f[4] == 3 && f[5] == 0 && f.substr(8) == "abc");
    std::string f2 = discord::encodeFrame(0x01020304, std::string(300, 'x'));
    CHECK((unsigned char)f2[0] == 4 && f2[3] == 1 && (unsigned char)f2[4] == 0x2c && f2[5] == 1);
    discord::Presence idle; idle.enabled = true;
    std::string j = discord::activityJson(idle, 42, "7");
    CHECK(j.find("\"pid\":42") != std::string::npos && j.find("\"details\":\"Idle\"") != std::string::npos && j.find("\"state\"") == std::string::npos && j.find("\"nonce\":\"7\"") != std::string::npos);
    discord::Presence q; q.enabled = true; q.running = true; q.mode = "A\"B"; q.freq = "File playback";
    CHECK(discord::activityJson(q, 1, "1").find("Listening to A\\\"B") != std::string::npos);
#ifndef _WIN32
    fakeServerTest();
#endif
    printf(fails ? "discord_rpc: %d failed\n" : "discord_rpc: ok\n", fails);
    return fails ? 1 : 0;
}
