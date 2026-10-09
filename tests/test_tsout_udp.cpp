// UDP streaming when frames come out of the receiver unevenly: several frames finish at once now and then (a busy CPU, the GPU working
// in batches). Every datagram must still reach the network; before, the whole queue was thrown away (VLC stuttered every few seconds).
#include "dect2/tsout.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace dect2;

int main() {
#ifdef _WIN32
    printf("tsout udp: skipped on Windows\n");
    return 0;
#else
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    int big = 8 << 20;
    setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(rx, (sockaddr*)&a, sizeof a) != 0) { printf("bind failed\n"); return 1; }
    socklen_t al = sizeof a;
    getsockname(rx, (sockaddr*)&a, &al);
    timeval tv{0, 200000};
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    OutputManager out;
    OutputConfig c;
    c.udp = true;
    c.host = "127.0.0.1";
    c.port = ntohs(a.sin_port);
    out.configure(c);

    uint8_t pkt[188];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
    size_t got = 0;
    std::thread reader([&] {
        uint8_t buf[2048];
        int idle = 0;
        while (idle < 15) {
            ssize_t n = recv(rx, buf, sizeof buf, 0);
            if (n > 0) { got++; idle = 0; } else idle++;
        }
    });
    int fails = 0;
    // 1. DVB-T 8K GI 1/4 64-QAM 3/4: about 16 packets after every 1.12 ms symbol, handed over in real time for 3 s (this is how the
    //    DVB-T, ISDB-T, DTMB and ATSC receivers deliver them).
    const double sym = 10240.0 / (64e6 / 7);
    size_t pk = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < (int)(3.0 / sym); s++) {
        for (int i = 0; i < 16; i++) out.packet(pkt, nullptr);
        pk += 16;
        out.burstDone(sym);
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((s + 1) * sym)));
    }
    // 2. DVB-T2 32K frames of 236 ms (~4700 packets) arriving in clumps of five with a pause of five frames between.
    const double span = 0.236;
    for (int f = 0; f < 20; f++) {
        for (int i = 0; i < 4700; i++) out.packet(pkt, nullptr);
        pk += 4700;
        out.burstDone(span);
        if (f % 5 == 4) std::this_thread::sleep_for(std::chrono::duration<double>(5 * span));
    }
    reader.join();
    const OutputStats st = out.stats();
    const size_t minimum = pk / 7;   // every packet must arrive (padding with nulls only adds datagrams)
    if (st.udpDropped != 0) { printf("FAIL: %llu datagrams dropped\n", (unsigned long long)st.udpDropped); fails++; }
    if (st.udpSendErrors != 0) { printf("FAIL: %llu send errors\n", (unsigned long long)st.udpSendErrors); fails++; }
    if (got < minimum) { printf("FAIL: received %zu datagrams, need at least %zu\n", got, minimum); fails++; }
    if (got > minimum * 11 / 10) { printf("FAIL: %zu datagrams for %zu packets: too much null padding\n", got, pk); fails++; }
    printf("tsout udp: %zu packets, sent %llu datagrams, received %zu (minimum %zu), catch-ups %llu: %s\n", pk,
           (unsigned long long)st.udpDatagrams, got, minimum, (unsigned long long)st.udpCatchUps, fails ? "FAILED" : "all passed");
    close(rx);
    return fails ? 1 : 0;
#endif
}
