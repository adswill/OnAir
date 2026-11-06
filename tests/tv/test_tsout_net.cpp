// Live transport-stream output over the network (tv/tsout.h): a synthetic TS goes into the OutputManager at a real-time 4 Mbit/s and a loopback
// UDP socket, an RTP socket and an HTTP viewer check what arrives: 7x188 byte datagrams, sync bytes, continuity counters, the paced arrival,
// the RTP header, the HTTP header, the service filter and a restart (reconfigure) in the middle.
#include "dect2/tv/tsout.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
using Clk = std::chrono::steady_clock;

struct Cap {
    std::vector<std::pair<double, std::vector<uint8_t>>> d;   // arrival time (s from start), datagram
};

static sock_t udpSock(int& port) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    int big = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr*)&a, sizeof a);
    socklen_t l = sizeof a; getsockname(s, (sockaddr*)&a, &l);
    port = ntohs(a.sin_port);
    sockRecvTimeout(s, 200);
    return s;
}

// continuity of PID 0x100 over a list of 188 byte packets; returns the number of payload packets, errors in `bad`
static size_t checkTs(const std::vector<uint8_t>& ts, int& bad, int pid = 0x100) {
    size_t n = 0; int last = -1; bad = 0;
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) {
        if (ts[i] != 0x47) { bad++; continue; }
        const int p = ((ts[i + 1] & 0x1F) << 8) | ts[i + 2];
        if (p != pid) continue;
        const int cc = ts[i + 3] & 15;
        if (last >= 0 && cc != ((last + 1) & 15)) bad++;
        last = cc; n++;
    }
    return n;
}

static void feed(OutputManager& out, double secs, double mbit, int& cc, size_t& sent, int pid = 0x100) {
    const double pps = mbit * 1e6 / (188 * 8);
    const int batch = (int)(pps * 0.05);
    const auto t0 = Clk::now();
    for (int b = 0; b < (int)(secs / 0.05); b++) {
        uint8_t pk[188];
        for (int i = 0; i < batch; i++) {
            memset(pk, 0xA5, sizeof pk);
            pk[0] = 0x47; pk[1] = (uint8_t)(pid >> 8); pk[2] = (uint8_t)pid; pk[3] = (uint8_t)(0x10 | (cc++ & 15));
            out.packet(pk, nullptr); sent++;
        }
        out.burstDone(0.05);
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(50 * (b + 1)));
    }
}

int main() {
    netInit();
    {   // UDP + HTTP at once
        int port; sock_t rx = udpSock(port);
        OutputManager out;
        OutputConfig c; c.udp = true; c.host = "127.0.0.1"; c.port = port; c.http = true; c.httpPort = 0;
        out.configure(c);
        const int hp = out.stats().httpPortInUse;
        CHECK(hp > 0, "http port");
        sock_t h = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)hp); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        CHECK(connect(h, (sockaddr*)&a, sizeof a) == 0, "http connect");
        const char* rq = "GET /stream.ts HTTP/1.1\r\nHost: x\r\n\r\n";
        sockSend(h, rq, strlen(rq));
        Cap cap; std::vector<uint8_t> http;
        std::atomic<bool> stop{false};
        const auto t0 = Clk::now();
        std::thread ru([&] { uint8_t b[2048]; while (!stop) { const auto n = sockRecv(rx, b, sizeof b); if (n > 0) cap.d.push_back({std::chrono::duration<double>(Clk::now() - t0).count(), std::vector<uint8_t>(b, b + n)}); } });
        std::thread rh([&] { uint8_t b[8192]; sockRecvTimeout(h, 200); while (!stop) { const auto n = sockRecv(h, b, sizeof b); if (n > 0) http.insert(http.end(), b, b + n); else if (n == 0) break; } });
        int cc = 0; size_t sent = 0;
        feed(out, 2.0, 4.0, cc, sent);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        stop = true; ru.join(); sockShutdown(h); rh.join();
        std::vector<uint8_t> all; bool sized = true; size_t early = 0;
        for (auto& p : cap.d) { if (p.second.size() != 7 * 188) sized = false; all.insert(all.end(), p.second.begin(), p.second.end()); if (p.first < 0.5) early++; }
        int bad; const size_t n = checkTs(all, bad);
        printf("UDP: %zu datagrams, %zu payload packets of %zu sent, %d continuity/sync errors, %zu datagrams in the first 0.5 s\n", cap.d.size(), n, sent, bad, early);
        CHECK(sized, "every datagram is 7 x 188 bytes");
        CHECK(n >= sent - 7 * 2 && n <= sent, "no packet lost or invented (%zu of %zu)", n, sent);
        CHECK(bad == 0, "continuity");
        // paced: about a quarter of the datagrams in the first quarter of the stream (4 Mbit/s = about 380 datagrams a second)
        CHECK(early < cap.d.size() * 40 / 100 && early > cap.d.size() * 10 / 100, "paced arrival (%zu of %zu in the first 0.5 s)", early, cap.d.size());
        // the HTTP viewer
        const std::string hs(http.begin(), http.begin() + std::min<size_t>(http.size(), 200));
        const size_t he = hs.find("\r\n\r\n");
        CHECK(hs.compare(0, 12, "HTTP/1.1 200") == 0 && hs.find("video/mp2t") != std::string::npos && he != std::string::npos, "http header");
        if (he != std::string::npos) {
            std::vector<uint8_t> body(http.begin() + (std::ptrdiff_t)he + 4, http.end());
            int hb; const size_t hn = checkTs(body, hb);
            printf("HTTP: %zu bytes, %zu payload packets, %d errors\n", body.size(), hn, hb);
            CHECK(body.size() % 188 == 0 && hn >= sent - 7 * 2 && hb == 0, "http stream whole packets, continuous (%zu)", hn);
        }
        sockClose(rx); sockClose(h);
        // restart in the middle: off and on again, the manager keeps working
        c.udp = false; out.configure(c);
        c.udp = true; out.configure(c);
        CHECK(out.stats().udpOpen && out.stats().httpOpen, "restart keeps both open");
    }
    {   // RTP
        int port; sock_t rx = udpSock(port);
        OutputManager out;
        OutputConfig c; c.udp = true; c.rtp = true; c.host = "127.0.0.1"; c.port = port;
        out.configure(c);
        std::vector<std::vector<uint8_t>> d; std::atomic<bool> stop{false};
        std::thread ru([&] { uint8_t b[2048]; while (!stop) { const auto n = sockRecv(rx, b, sizeof b); if (n > 0) d.push_back(std::vector<uint8_t>(b, b + n)); } });
        int cc = 0; size_t sent = 0;
        feed(out, 1.0, 4.0, cc, sent);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop = true; ru.join(); sockClose(rx);
        bool hdr = !d.empty(), seq = true; std::vector<uint8_t> all;
        for (size_t i = 0; i < d.size(); i++) {
            if (d[i].size() != 12 + 7 * 188 || d[i][0] != 0x80 || (d[i][1] & 0x7F) != 33) hdr = false;
            if (i && (uint16_t)((d[i][2] << 8 | d[i][3]) - (d[i - 1][2] << 8 | d[i - 1][3])) != 1) seq = false;
            all.insert(all.end(), d[i].begin() + 12, d[i].end());
        }
        int bad; const size_t n = checkTs(all, bad);
        printf("RTP: %zu datagrams, %zu packets, %d errors\n", d.size(), n, bad);
        CHECK(hdr, "RTP header (v2, payload type 33, 12 + 1316 bytes)");
        CHECK(seq, "RTP sequence rises by one");
        CHECK(bad == 0 && n + 14 >= sent, "RTP payload continuous");
    }
    {   // one service: the other programmes never leave
        int port; sock_t rx = udpSock(port);
        OutputManager out;
        OutputConfig c; c.udp = true; c.host = "127.0.0.1"; c.port = port; c.dropNull = true;
        out.configure(c);
        std::atomic<bool> stop{false}; size_t pkts = 0;
        std::thread ru([&] { uint8_t b[2048]; while (!stop) { const auto n = sockRecv(rx, b, sizeof b); if (n > 0) pkts += (size_t)n / 188; } });
        int cc = 0; size_t sent = 0;
        feed(out, 0.5, 2.0, cc, sent);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop = true; ru.join(); sockClose(rx);
        CHECK(pkts > 0, "whole-mux output with dropNull still delivers");
    }
    // parseStreamUrl
    {
        OutputConfig c; std::string e;
        CHECK(parseStreamUrl("udp://127.0.0.1:1234", c, e) && c.udp && !c.rtp && c.host == "127.0.0.1" && c.port == 1234 && !c.http, "udp url");
        CHECK(parseStreamUrl("udp://@239.1.1.1:5000", c, e) && c.host == "239.1.1.1" && c.port == 5000, "multicast url");
        CHECK(parseStreamUrl("rtp://192.168.1.20:5004", c, e) && c.rtp && c.udp, "rtp url");
        CHECK(parseStreamUrl("http://:8080/stream.ts", c, e) && c.http && c.httpPort == 8080 && !c.httpLan && !c.udp, "http url local");
        CHECK(parseStreamUrl("http://0.0.0.0:9000", c, e) && c.httpLan, "http url network");
        CHECK(!parseStreamUrl("ftp://x:1", c, e) && !parseStreamUrl("udp://1.2.3.4", c, e) && !parseStreamUrl("udp://1.2.3.4:99999", c, e) && !parseStreamUrl("udp://nonsense:12", c, e), "bad urls refused");
        CHECK(parseStreamUrl("127.0.0.1:4000", c, e) && c.udp && c.port == 4000, "no scheme = udp");
    }
    printf(fails ? "FAILED %d\n" : "tsout_net ok\n", fails);
    return fails ? 1 : 0;
}
