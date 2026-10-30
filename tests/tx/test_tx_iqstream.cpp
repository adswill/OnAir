// The transmit IQ on TCP (tx_iqstream.h): every format received by a loopback client and compared with the sent samples, the JSON header and the side
// channel, a client that connects late / leaves / reconnects, a slow client that costs the sender nothing, and a dry-run TxSession (no radio) served over TCP.
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_iqstream.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static sock_t connectTo(int port) {
    netInit();
    const sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { sockClose(s); return kBadSock; }
    return s;
}
// reads until n bytes arrived or the time is up
static std::string readN(sock_t s, size_t n, double sec) {
    std::string out;
    char b[65536];
    const auto t0 = std::chrono::steady_clock::now();
    while (out.size() < n && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < sec) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        const long long r = sockRecv(s, b, std::min(sizeof b, n - out.size()));
        if (r <= 0) break;
        out.append(b, (size_t)r);
    }
    return out;
}
static std::string readLine(sock_t s) {
    std::string l;
    char c;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 3) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        if (sockRecv(s, &c, 1) != 1) break;
        l.push_back(c);
        if (c == '\n') break;
    }
    return l;
}
static void waitClients(const TxIqStream& st, int n) {
    for (int i = 0; i < 300 && st.clients() < n; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
static std::vector<cf32> sig(size_t n) {
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; i++) x[i] = cf32((float)(0.8 * std::cos(0.05 * (double)i)), (float)(-0.6 * std::sin(0.031 * (double)i)));
    x[3] = cf32(1.7f, -2.f);   // above full scale: clipped
    return x;
}

int main() {
    // formats, byte for byte
    const std::vector<cf32> x = sig(5000);
    for (int f = 0; f <= 4; f++) {
        const TxIqFormat fmt = (TxIqFormat)f;
        TxIqFormat back;
        CHECK(txIqFormatParse(txIqFormatName(fmt), back) && back == fmt, "format name round trip %d", f);
        TxIqStreamConfig c; c.enabled = true; c.port = 0; c.metaPort = false; c.format = fmt;
        TxIqStream st; std::string err;
        CHECK(st.start(c, 1e6, 100e6, "test", err), "start %d: %s", f, err.c_str());
        sock_t k = connectTo(st.port());
        CHECK(k != kBadSock, "connect %d", f);
        waitClients(st, 1);
        CHECK(st.clients() == 1, "client counted %d", f);
        st.push(x.data(), x.size());
        const size_t bps = txIqBytesPerSample(fmt);
        const std::string got = readN(k, x.size() * bps, 5);
        CHECK(got.size() == x.size() * bps, "%s: %zu of %zu bytes", txIqFormatName(fmt), got.size(), x.size() * bps);
        std::vector<uint8_t> want(x.size() * bps);
        txIqPack(x.data(), x.size(), fmt, want.data());
        CHECK(got.size() == want.size() && std::memcmp(got.data(), want.data(), want.size()) == 0, "%s: bytes differ", txIqFormatName(fmt));
        if (fmt == TxIqFormat::Cf32) { cf32 y; std::memcpy(&y, got.data() + 8 * 10, 8); CHECK(y == x[10], "cf32 sample 10"); CHECK(std::memcmp(got.data() + 24, &x[3], 8) == 0, "cf32 is not clipped (float is exact)"); }
        if (fmt == TxIqFormat::Cs16) { int16_t i3; std::memcpy(&i3, got.data() + 12, 2); CHECK(i3 == 32767, "cs16 clipped to full scale, got %d", i3); }
        if (fmt == TxIqFormat::Cs8) CHECK((int8_t)got[6] == 127 && (int8_t)got[7] == -127, "cs8 clipped");
        if (fmt == TxIqFormat::Cu8) CHECK((uint8_t)got[6] == 255 && (uint8_t)got[7] == 0, "cu8 clipped");
        sockClose(k);
    }
    TxIqFormat dummy;
    CHECK(!txIqFormatParse("float64", dummy), "unknown format refused");

    // header line, side channel, late join, leave and reconnect
    {
        TxIqStreamConfig c; c.enabled = true; c.port = 0; c.headerLine = true; c.metaPort = true; c.format = TxIqFormat::Cs16;
        // port 0 gives a random data port, and the side channel could not be port + 1: use a fixed free-looking pair
        c.port = 47355;
        TxIqStream st; std::string err;
        CHECK(st.start(c, 2.4e6, 522e6, "dvbt2", err), "start: %s", err.c_str());
        sock_t m = connectTo(47356);
        CHECK(m != kBadSock, "side channel connect");
        const std::string j = readLine(m);
        CHECK(j.find("\"sample_rate\":2400000") != std::string::npos && j.find("\"center_freq\":522000000") != std::string::npos && j.find("\"format\":\"cs16\"") != std::string::npos && j.find("\"mode\":\"dvbt2\"") != std::string::npos && j.back() == '\n', "side channel line: %s", j.c_str());
        sockClose(m);
        sock_t k = connectTo(47355);
        const std::string h = readLine(k);
        CHECK(h == j, "data header equals the side channel line");
        waitClients(st, 1);
        st.push(x.data(), 100);
        CHECK(readN(k, 400, 3).size() == 400, "samples after the header");
        sockClose(k);
        for (int i = 0; i < 100 && st.clients() > 0; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); st.push(x.data(), 100); }
        CHECK(st.clients() == 0, "a client that left is dropped");
        sock_t k2 = connectTo(47355);   // reconnect
        CHECK(!readLine(k2).empty(), "reconnect gets the header again");
        waitClients(st, 1);
        st.push(x.data(), 100);
        CHECK(readN(k2, 400, 3).size() == 400, "samples after reconnect");
        sockClose(k2);
        st.stop();
        CHECK(connectTo(47355) == kBadSock, "port closed after stop");
        // restart on the same port at once
        CHECK(st.start(c, 2.4e6, 522e6, "dvbt2", err), "restart on the same port: %s", err.c_str());
        st.stop();
    }
    // a slow client costs the sender nothing; a second client is served
    {
        TxIqStreamConfig c; c.enabled = true; c.port = 0; c.metaPort = false; c.format = TxIqFormat::Cf32; c.maxPendingBytes = 1 << 20;
        TxIqStream st; std::string err;
        CHECK(st.start(c, 1e6, 0, "t", err), "start slow: %s", err.c_str());
        sock_t slow = connectTo(st.port()), fast = connectTo(st.port());
        waitClients(st, 2);
        std::vector<cf32> big(1 << 16);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; i++) st.push(big.data(), big.size());   // 100 MB at a client that never reads
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(took < 10, "push never blocks: %.1f s", took);
        CHECK(st.blocksDropped() > 0, "a stalled client loses blocks");
        CHECK(st.clients() == 2, "stalled client kept");
        sockClose(slow); sockClose(fast);
    }
    // an address that is not valid and a port in use are reported, not thrown
    {
        TxIqStreamConfig c; c.enabled = true; c.port = 0; c.bindAddr = "not-an-address";
        TxIqStream st; std::string err;
        CHECK(!st.start(c, 1e6, 0, "t", err) && !err.empty(), "bad address reported");
        TxIqStreamConfig d; d.enabled = true; d.port = 0; d.metaPort = false;
        TxIqStream a; CHECK(a.start(d, 1e6, 0, "t", err), "first");
        d.port = a.port();
        TxIqStream b2; CHECK(!b2.start(d, 1e6, 0, "t", err) && err.find("in use") != std::string::npos, "port in use reported: %s", err.c_str());
    }
    // a dry-run transmission (no radio) served over TCP, and the transmission is never refused when the stream cannot open
    {
        TxIqStreamConfig c; c.enabled = true; c.port = 47455; c.metaPort = true; c.format = TxIqFormat::Cs8;
        txIqStreamSetConfig(c);
        TxRequest r; r.modeId = "kaal"; r.audioSrc = 2; r.loop = true; r.tune.centerHz = 433.0e6; r.tune.sampleRate = 48000;
        for (int pass = 0; pass < 2; pass++) {   // twice: stop, start again, connect again
            TxSession s; std::string err;
            CHECK(s.start(makeNullTxSink(), r, {}, err), "dry-run start %d: %s", pass, err.c_str());
            CHECK(s.dryRun(), "dry run");
            sock_t m = connectTo(47456);
            const std::string j = readLine(m);
            sockClose(m);
            CHECK(j.find("\"sample_rate\":48000") != std::string::npos && j.find("\"center_freq\":433000000") != std::string::npos && j.find("\"mode\":\"kaal\"") != std::string::npos, "session header: %s", j.c_str());
            sock_t k = connectTo(47455);
            const std::string got = readN(k, 9600, 8);   // 0.1 s of cs8 at 48 kHz, paced by the null sink
            CHECK(got.size() == 9600, "pass %d: %zu bytes of IQ from the session", pass, got.size());
            bool nonzero = false;
            for (char ch : got) if (ch) nonzero = true;
            CHECK(nonzero, "the streamed IQ is the signal, not silence");
            CHECK(s.iqClients() >= 0, "stream active");
            sockClose(k);
            s.stop();
        }
        TxIqStreamConfig held = c;   // the port is taken: the transmission still starts
        netInit();
        sock_t blocker = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(47455); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(blocker, (sockaddr*)&a, sizeof a); listen(blocker, 1);
        txIqStreamSetConfig(held);
        TxSession s; std::string err;
        CHECK(s.start(makeNullTxSink(), r, {}, err), "a port in use does not stop the transmission: %s", err.c_str());
        bool warned = false;
        for (const auto& l : s.log()) if (l.find("warning") != std::string::npos) warned = true;
        CHECK(warned && s.iqClients() == -1, "warning in the log");
        s.stop();
        sockClose(blocker);
        txIqStreamSetConfig(TxIqStreamConfig());
    }
    return testSummary("test_tx_iqstream");
}
