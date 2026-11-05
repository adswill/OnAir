// Live RDS control (fm/rds_live.h): the encoder (FmGenerator) takes changes of the station name, text, PTY and flags while it runs; the receiver decodes them back.
// Also the text commands, the UDP command port on the loopback, the file watch and the clock group.
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/fm/rds_live.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Rig {
    FmGenConfig c;
    std::shared_ptr<RdsLive> live = std::make_shared<RdsLive>();
    std::unique_ptr<FmGenerator> g;
    FmReceiver rx;
    FmTelemetry t;
    uint64_t seq = 0;
    Rig() {
        c.rate = 1e6; c.cnrDb = 200; c.stereo = true;
        RdsState s; s.ps = "FIRST   "; s.rt = "First text"; s.pi = 0x4A21; s.pty = 10; s.ct = true;
        live->seed(s);
        c.live = live;
        g.reset(new FmGenerator(c));
        rx.setSilent(true); rx.configure(c.rate); rx.setStationFinder(false);
    }
    void run(double secs) {
        std::vector<cf32> x;
        for (double tt = 0; tt < secs; tt += 50000 / c.rate) {
            x.clear(); g->generate(50000, x); rx.feed(x.data(), x.size());
            FmTelemetry q;
            if (rx.telemetry(q, seq)) { seq = q.seq; t = q; }
        }
    }
};

int main() {
    {   // changes while running
        Rig r;
        r.run(6);
        CHECK(r.t.psName == "FIRST   ", "PS first '%s'", r.t.psName.c_str());
        CHECK(r.t.radioText.compare(0, 10, "First text") == 0, "RT first '%s'", r.t.radioText.c_str());
        CHECK(r.t.rds.clockUtc != 0, "clock group received");
        if (r.t.rds.clockUtc) CHECK(std::llabs((long long)r.t.rds.clockUtc - (long long)time(nullptr)) < 120, "clock within two minutes of now");
        CHECK(r.live->command("PS=SECOND") == "OK", "PS command");
        CHECK(r.live->command("RT=A completely different radio text") == "OK", "RT command");
        CHECK(r.live->command("PTY=4") == "OK", "PTY");
        r.run(10);
        CHECK(r.t.psName == "SECOND  ", "PS after change '%s'", r.t.psName.c_str());
        CHECK(r.t.radioText.compare(0, 36, "A completely different radio text") == 0, "RT after change '%s'", r.t.radioText.c_str());
        CHECK(r.t.ptyText.find("Pop") == std::string::npos && !r.t.ptyText.empty(), "PTY text changed '%s'", r.t.ptyText.c_str());
        printf("after: PS '%s' RT '%s' PTY '%s'\n", r.t.psName.c_str(), r.t.radioText.c_str(), r.t.ptyText.c_str());
    }
    {   // commands
        RdsLive L;
        std::string e;
        CHECK(L.command("PS=ABCDEFGHIJK") == "OK" && L.get().ps == "ABCDEFGH", "long PS cut to 8");
        L.command("PS=AB");
        CHECK(L.get().ps == "AB      ", "short PS padded");
        CHECK(L.command("PI=ZZ").compare(0, 3, "ERR") == 0, "bad PI refused");
        CHECK(L.command("PTY=40").compare(0, 3, "ERR") == 0, "bad PTY refused");
        L.command("PI=c201");
        CHECK(L.get().pi == 0xC201, "PI hex");
        L.command("MS=S");
        CHECK(!L.get().music, "speech");
        L.command("RT=text one");
        const int ab = L.get().abFlag;
        L.command("RT=text two");
        CHECK(L.get().abFlag != ab, "A/B flips on a new text");
        L.command("RT=text two");
        CHECK(L.get().abFlag != ab, "A/B kept when same text");
        CHECK(L.command("?").compare(0, 3, "OK ") == 0, "status");
        CHECK(L.json().find("\"ps\":\"AB      \"") != std::string::npos, "json");
    }
    {   // UDP command port on the loopback, with a token and without
        RdsLive L;
        std::string e;
        CHECK(L.listen(0 + 38917, false, "", e), "listen %s", e.c_str());
        netInit();
        sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(38917); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        const char* m = "PS=UDPNAME\nRT=from udp\n";
        sockSendTo(s, m, strlen(m), to);
        for (int i = 0; i < 30 && L.get().rt != "from udp"; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(L.get().ps == "UDPNAME " && L.get().rt == "from udp", "UDP changed PS '%s' RT '%s'", L.get().ps.c_str(), L.get().rt.c_str());
        // TCP
        sock_t t = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(connect(t, (sockaddr*)&to, sizeof to) == 0, "tcp connect");
        const char* tm = "TP=0\n";
        sockSend(t, tm, strlen(tm));
        char buf[64] = {};
        const auto n = sockRecv(t, buf, sizeof buf - 1);
        CHECK(n > 0 && std::strncmp(buf, "OK", 2) == 0 && !L.get().tp, "TCP reply '%s'", buf);
        sockClose(t); sockClose(s);
        CHECK(L.listen(0, false, "", e), "stop");
        // token
        CHECK(!L.listen(38918, true, "short", e), "lan without a token refused");
        CHECK(L.listen(38918, false, "0123456789abcdef", e), "listen with token");
        s = socket(AF_INET, SOCK_DGRAM, 0);
        to.sin_port = htons(38918);
        const char* bad = "PS=NOTOKEN";
        sockSendTo(s, bad, strlen(bad), to);
        const char* good = "0123456789abcdef;PS=TOKENED";
        sockSendTo(s, good, strlen(good), to);
        for (int i = 0; i < 30 && L.get().ps != "TOKENED "; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(L.get().ps == "TOKENED ", "token datagram applied, bare one not: '%s'", L.get().ps.c_str());
        sockClose(s);
    }
    {   // file watch
        const std::string path = "/tmp/rds_live_test.txt";
        { std::ofstream f(path); f << "Now playing: A - B\n"; }
        RdsLive L;
        L.watchFile(path);
        for (int i = 0; i < 30 && L.get().rt != "Now playing: A - B"; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(L.get().rt == "Now playing: A - B", "file text '%s'", L.get().rt.c_str());
        { std::ofstream f(path); f << "Now playing: C - D\r\n"; }
        for (int i = 0; i < 30 && L.get().rt != "Now playing: C - D"; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(L.get().rt == "Now playing: C - D", "file text changed '%s'", L.get().rt.c_str());
        L.watchFile("");
        remove(path.c_str());
    }
    {   // clock words
        uint16_t b, c, d;
        rdsClockWords(1700000000, 2, true, 10, b, c, d);   // 2023-11-14 22:13:20 UTC
        const int mjd = ((b & 3) << 15) | (c >> 1), hour = ((c & 1) << 4) | (d >> 12), min = (d >> 6) & 63;
        CHECK(mjd == 60262 && hour == 22 && min == 13, "clock words mjd %d %02d:%02d", mjd, hour, min);
    }
    printf(fails ? "FAILED %d\n" : "rds_live ok\n", fails);
    return fails ? 1 : 0;
}
