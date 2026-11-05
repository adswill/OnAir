// The KiwiSDR source (source_kiwisdr.cpp) against a fake KiwiSDR on localhost: the WebSocket upgrade on /kiwi/<time>/SND, "SET auth", the MSG hand-shake
// (version, audio_rate, sample_rate), the set-up and tuning commands, keepalive, SND IQ frames (flags, sequence, S-meter, GPS block, big-endian s16), a
// sequence gap, short / odd / compressed / oversize frames, a busy or refusing server, a time limit and the reconnect pause. Also the pure pieces
// (MSG, SND and W/F parsing fuzzed, the interpolator, the public list parser, the network radio list text with a password).
// No real KiwiSDR was used: the fake is what kiwiclient's sources say, nothing more.
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/net/ws_client.h"
#include "../../core/src/source/kiwisdr_proto.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/kiwisdr.h"
#include "dect2/net/netradio.h"
// KIWI_MINI (a quick build of only the Kiwi pieces while developing): the NetRadio list and probe checks are left out
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

template <class F> static bool eventually(F f, int ms = 4000) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}

static std::string msgFrame(const std::string& body) { return std::string("MSG") + std::string(1, '\0') + body; }
static std::string sndFrame(uint32_t seq, int n, int16_t i, int16_t q, uint8_t flags = 0x08, uint16_t smeter = 600, int gpsStatus = 1) {
    std::string f = "SND";
    f += (char)flags;
    for (int b = 0; b < 4; b++) f += (char)(seq >> (8 * b) & 255);
    f += (char)(smeter >> 8); f += (char)(smeter & 255);
    f += (char)gpsStatus; f += (char)0;
    const uint32_t gs = 123456, gn = 500000000;
    for (int b = 0; b < 4; b++) f += (char)(gs >> (8 * b) & 255);
    for (int b = 0; b < 4; b++) f += (char)(gn >> (8 * b) & 255);
    for (int k = 0; k < n; k++) { f += (char)(i >> 8 & 255); f += (char)(i & 255); f += (char)(q >> 8 & 255); f += (char)(q & 255); }
    return f;
}

struct Fake {
    enum Mode { Normal, Busy, BadPw, Down, Plain } mode = Normal;
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true};
    std::atomic<int> dropNow{0}, timeLimit{0}, junk{0}, gap{0}, hugeFrame{0}, connections{0}, resetSeq{0}, slowFrame{0};
    std::atomic<int> noIq{0};   // 1: MSG lines instead of IQ frames, 2: mono audio frames instead of IQ (the connection stays open: a stall)
    std::mutex mu;
    std::vector<std::string> log;   // each text command of the client
    std::string path;
    bool streaming = false, gotMod = false, gotAgc = false;
    std::atomic<uint32_t> seq{100};

    bool has(const std::string& l) { std::lock_guard<std::mutex> lk(mu); for (auto& x : log) if (x == l) return true; return false; }
    bool hasPrefix(const std::string& p) { std::lock_guard<std::mutex> lk(mu); for (auto& x : log) if (x.compare(0, p.size(), p) == 0) return true; return false; }
    int countPrefix(const std::string& p) { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto& x : log) n += x.compare(0, p.size(), p) == 0; return n; }
    std::string last(const std::string& p) { std::lock_guard<std::mutex> lk(mu); std::string r; for (auto& x : log) if (x.compare(0, p.size(), p) == 0) r = x; return r; }
    bool anyContains(const std::string& p) { std::lock_guard<std::mutex> lk(mu); for (auto& x : log) if (x.find(p) != std::string::npos) return true; return false; }

    bool start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        sockSetNonBlocking(ls, true);
        th = std::thread([this] { loop(); });
        return true;
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }

    static void wsSend(sock_t c, int op, const std::string& p) {
        std::string f;
        f += (char)(0x80 | op);
        if (p.size() < 126) f += (char)p.size();
        else if (p.size() < 65536) { f += (char)126; f += (char)(p.size() >> 8); f += (char)(p.size() & 255); }
        else { f += (char)127; for (int i = 7; i >= 0; i--) f += (char)((uint64_t)p.size() >> (8 * i) & 255); }
        sockSendAll(c, (f + p).data(), f.size() + p.size());
    }
    bool rd(sock_t c, void* d, size_t n, bool idleOk) {
        char* p = (char*)d;
        size_t got = 0;
        int idle = 0;
        while (got < n) {
            const long long k = sockRecv(c, p + got, n - got);
            if (k > 0) { got += (size_t)k; continue; }
            if (k == 0) return false;
            if (got == 0 && idleOk) return false;
            if (++idle > 100) return false;
        }
        return true;
    }

    void serve(sock_t c) {
        sockRecvTimeout(c, 20);
        std::string head;
        char ch;
        while (head.size() < 4096 && run) {
            if (sockRecv(c, &ch, 1) <= 0) { if (!run) return; continue; }
            head += ch;
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        if (mode == Plain) { const char* g = "HTTP/1.1 404 Not Found\r\n\r\n"; sockSendAll(c, g, strlen(g)); return; }
        const size_t k = head.find("Sec-WebSocket-Key: ");
        if (k == std::string::npos) return;
        { std::lock_guard<std::mutex> lk(mu); path = head.substr(4, head.find(' ', 4) - 4); }
        const std::string key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
        const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n";
        sockSendAll(c, r.data(), r.size());
        { std::lock_guard<std::mutex> lk(mu); streaming = false; gotMod = gotAgc = false; }
        int tick = 0;
        while (run) {
            if (dropNow.exchange(0)) return;
            if (timeLimit.exchange(0)) { wsSend(c, 2, msgFrame("inactivity_timeout=1")); std::this_thread::sleep_for(std::chrono::milliseconds(50)); return; }
            if (hugeFrame.exchange(0)) wsSend(c, 2, std::string(70000, 'x'));
            if (junk.exchange(0)) {
                wsSend(c, 2, "SN");                                         // shorter than a tag + header
                wsSend(c, 2, sndFrame(seq++, 8, 1, 1).substr(0, 15));       // header cut in the GPS block
                wsSend(c, 2, sndFrame(seq++, 8, 1, 1, 0x18));               // compressed
                wsSend(c, 2, sndFrame(seq++, 8, 1, 1, 0x00));               // mono audio, not IQ
                wsSend(c, 2, sndFrame(seq++, 0, 1, 1));                     // no samples
                wsSend(c, 2, sndFrame(seq++, 9000, 1, 1));                  // more samples than a frame has
                wsSend(c, 2, "W/F\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");    // a waterfall line: not asked for, ignored
                wsSend(c, 2, msgFrame("no_equals_token a=%zz b=%41 foo"));
                wsSend(c, 10, "pong");
            }
            if (slowFrame.exchange(0)) {   // a frame that arrives in two parts with a pause longer than the client's receive timeout (a slow link)
                std::string fr = sndFrame(seq++, 512, 16384, -8192, 0x08, 700), w;
                w += (char)0x82; w += (char)126; w += (char)(fr.size() >> 8); w += (char)(fr.size() & 255);
                w += fr.substr(0, 100);
                sockSendAll(c, w.data(), w.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(1400));
                sockSendAll(c, fr.data() + 100, fr.size() - 100);
            }
            if (gap.exchange(0)) seq += 5;
            if (resetSeq.exchange(0)) seq = 0;   // the server counts from a new start
            bool s;
            { std::lock_guard<std::mutex> lk(mu); s = streaming; }
            if (s && (++tick % 3) == 0) {
                const int q = noIq.load();
                if (q == 1) wsSend(c, 2, msgFrame("stats_cpu=12"));
                else if (q == 2) wsSend(c, 2, sndFrame(seq++, 512, 1, 1, 0x00));
                else wsSend(c, 2, sndFrame(seq++, 512, 16384, -8192, 0x08, 700));
            }
            unsigned char h[2];
            if (!rd(c, h, 2, true)) { if (!run) return; if (sockPeekClosed(c) == 0) return; continue; }
            const int op = h[0] & 15;
            size_t len = h[1] & 127;
            if (len == 126) { unsigned char x[2]; if (!rd(c, x, 2, false)) return; len = x[0] << 8 | x[1]; }
            unsigned char m[4];
            if (!rd(c, m, 4, false)) return;
            std::string p(len, 0);
            if (len && !rd(c, &p[0], len, false)) return;
            for (size_t i = 0; i < len; i++) p[i] ^= m[i & 3];
            if (op == 8) return;
            if (op != 1) continue;
            { std::lock_guard<std::mutex> lk(mu); log.push_back(p); }
            if (p.compare(0, 8, "SET auth") == 0) {
                if (mode == Busy) { wsSend(c, 2, msgFrame("sample_rate=11998.9")); wsSend(c, 2, msgFrame("too_busy=4")); continue; }
                if (mode == BadPw) { wsSend(c, 2, msgFrame("sample_rate=11998.9")); wsSend(c, 2, msgFrame("badp=1")); continue; }
                if (mode == Down) { wsSend(c, 2, msgFrame("down=0")); continue; }
                // the order of a real KiwiSDR v1.8xx: sample_rate first, then badp=0 (accepted), version, audio_rate
                wsSend(c, 2, msgFrame("sample_rate=11998.939594"));
                wsSend(c, 2, msgFrame("client_public_ip=127.0.0.1"));
                wsSend(c, 2, msgFrame("rx_chans=8 firmware_sel=5"));
                wsSend(c, 2, msgFrame("badp=0"));
                wsSend(c, 2, msgFrame("version_maj=1 version_min=800"));
                wsSend(c, 2, msgFrame("center_freq=15000000 bandwidth=30000000 adc_clk_nom=66666600"));
                wsSend(c, 2, msgFrame("audio_init=0 audio_rate=12000"));
            } else if (p.compare(0, 7, "SET mod") == 0 || p.compare(0, 7, "SET agc") == 0) {   // a real Kiwi streams only after both
                std::lock_guard<std::mutex> lk(mu);
                if (p[4] == 'm') gotMod = true; else gotAgc = true;
                if (gotMod && gotAgc) streaming = true;
            }
        }
    }
    void loop() {
        while (run) {
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            sockSetNonBlocking(c, false);
            connections++;
            serve(c);
            sockShutdown(c);
            sockClose(c);
        }
    }
};

static bool readIq(IqRing& ring, std::vector<cf32>& buf) {
    size_t got = 0;
    for (int i = 0; i < 500 && got < buf.size(); i++) { got += ring.read(buf.data() + got, buf.size() - got); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return got == buf.size();
}

static double goertzel(const std::vector<std::complex<float>>& x, size_t from, size_t n, double hz, double fs) {
    std::complex<double> acc(0, 0);
    for (size_t k = 0; k < n; k++) acc += std::complex<double>(x[from + k]) * std::polar(1.0, -2 * kiwi::kPi * hz * (double)k / fs);
    return std::abs(acc) / (double)n;
}

static void pureTests() {
    // MSG
    std::vector<std::pair<std::string, std::string>> kv;
    const std::string m = msgFrame("version_maj=1 version_min=8 name=a%20b flag too_busy=3");
    CHECK(kiwi::parseMsg((const unsigned char*)m.data(), m.size(), kv) && kv.size() == 5);
    CHECK(kv[2].first == "name" && kv[2].second == "a b" && kv[3].first == "flag" && kv[3].second.empty() && kv[4].second == "3");
    CHECK(!kiwi::parseMsg((const unsigned char*)"SND", 3, kv) && !kiwi::parseMsg((const unsigned char*)"MS", 2, kv));
    CHECK(kiwi::urlDecode("%4") == "%4" && kiwi::urlDecode("%zz") == "%zz" && kiwi::urlDecode("a%2Fb") == "a/b");
    CHECK(kiwi::urlEncode("p w;1") == "p%20w%3B1" && kiwi::authCmd("") == "SET auth t=kiwi p=#" && kiwi::authCmd("a b") == "SET auth t=kiwi p=a%20b");
    CHECK(kiwi::tuneCmd(7074500.0, 5000) == "SET mod=iq low_cut=-5000 high_cut=5000 freq=7074.500");
    // SND
    const std::string f = sndFrame(0xFFFFFFFEu, 3, -2, 300, 0x0A, 1270);
    kiwi::Snd s;
    CHECK(kiwi::parseSnd((const unsigned char*)f.data(), f.size(), s) == kiwi::SndResult::Ok);
    CHECK(s.seq == 0xFFFFFFFEu && s.samples == 3 && s.iq && (s.flags & kiwi::kOverflow) && std::fabs(s.rssiDbm - 0.0) < 1e-9 && s.gpsSec == 123456 && s.gpsNsec == 500000000 && s.gpsStatus == 1);
    float out[6];
    kiwi::decodeIq((const unsigned char*)f.data(), s, out, false);
    CHECK(std::fabs(out[0] + 2 / 32768.f) < 1e-9 && std::fabs(out[1] - 300 / 32768.f) < 1e-9);
    kiwi::decodeIq((const unsigned char*)f.data(), s, out, true);
    CHECK(std::fabs(out[0] - 300 / 32768.f) < 1e-9 && std::fabs(out[1] + 2 / 32768.f) < 1e-9);
    CHECK(kiwi::parseSnd((const unsigned char*)f.data(), 9, s) == kiwi::SndResult::TooShort && kiwi::parseSnd((const unsigned char*)f.data(), 19, s) == kiwi::SndResult::TooShort);
    CHECK(kiwi::parseSnd((const unsigned char*)f.data(), 22, s) == kiwi::SndResult::OddSize && s.samples == 0);
    CHECK(kiwi::parseSnd((const unsigned char*)"XXX0123456789", 13, s) == kiwi::SndResult::NotSnd);
    // W/F
    kiwi::Wf w;
    std::string wf = "W/F" + std::string(1, 'x') + std::string("\x01\0\0\0\x02\0\0\0\x03\0\0\0", 12) + std::string(1024, 'p');
    CHECK(kiwi::parseWf((const unsigned char*)wf.data(), wf.size(), w) && w.xBin == 1 && w.flagsZoom == 2 && w.seq == 3 && w.pixels == 1024);
    CHECK(!kiwi::parseWf((const unsigned char*)wf.data(), 15, w));
    // fuzz: random and mutated input never crash, and what is accepted keeps its bounds
    std::mt19937 rng(11);
    for (int i = 0; i < 40000; i++) {
        std::string x = (i & 1) ? f : m;
        if (rng() % 3 == 0) x.resize(rng() % (x.size() + 1));
        for (int k = 0, n = rng() % 5; k < n && !x.empty(); k++) x[rng() % x.size()] = (char)rng();
        if (rng() % 7 == 0) { x.resize(rng() % 100); for (auto& c : x) c = (char)rng(); }
        kiwi::parseMsg((const unsigned char*)x.data(), x.size(), kv);
        CHECK(kv.size() <= 64);
        const auto r = kiwi::parseSnd((const unsigned char*)x.data(), x.size(), s);
        if (r == kiwi::SndResult::Ok || r == kiwi::SndResult::OddSize) CHECK(s.sampleOff + s.samples * 4 <= x.size());
        kiwi::parseWf((const unsigned char*)x.data(), x.size(), w);
    }
    // the interpolator: a 3 kHz tone stays, its image at 12 - 3 = 9 kHz goes (> 40 dB), the shift moves it to 3 - 20 = -17 kHz
    for (int L : {21, 84}) {
        kiwi::Upsampler u;
        u.configure(L, 0);
        std::vector<std::complex<float>> in(1200), out;
        for (size_t k = 0; k < in.size(); k++) in[k] = std::polar(0.5f, (float)(2 * kiwi::kPi * 3000.0 * (double)k / 12000.0));
        u.process(in.data(), in.size(), out);
        CHECK(out.size() == in.size() * (size_t)L);
        const double fs = 12000.0 * L, skip = 20000;
        const double tone = goertzel(out, (size_t)skip, 4000, 3000, fs), img = goertzel(out, (size_t)skip, 4000, 9000, fs);
        CHECK(std::fabs(tone - 0.5) < 0.02);
        CHECK(img < tone * 0.01);
        kiwi::Upsampler v;
        v.configure(L, 20000);
        std::vector<std::complex<float>> o2;
        v.process(in.data(), in.size(), o2);
        CHECK(goertzel(o2, (size_t)skip, 4000, -17000, fs) > 0.45);
    }
}

static void listTests() {
    const std::string js =
        "var kiwisdr_com = [\n{ \"name\": \"Test Kiwi 1\", \"sdr_hw\": \"KiwiSDR 2 v1.800\", \"bands\": \"0-30000000\", \"users\": 1, \"users_max\": 4, \"gps\": \"(51.5, -0.12)\","
        " \"antenna\": \"Loop \\u00e9\", \"snr\": \"32,28\", \"loc\": \"London, UK\", \"offline\": \"no\", \"url\": \"http://kiwi.example.org:8074\", \"x\": {\"a\":[1,2,{\"b\":3}]} },\n"
        "{ \"name\": \"No url\", \"users\": 0 },\n"
        "{ \"name\": \"Full\", \"users\": 4, \"users_max\": 4, \"url\": \"http://10.1.2.3\", \"offline\": \"yes\", \"gps\": \"(-33.9, 151.2)\" },\n"
        "{ \"name\": \"Bad host\", \"url\": \"http://a b:80\" },\n"
        "{ \"name\": \"V6\", \"url\": \"http://[2001:db8::1]:8073/\", \"bands\": \"100000-30000000\" }\n];";
    auto v = parseKiwiList(js);
    CHECK(v.size() == 3);
    if (v.size() == 3) {
        CHECK(v[0].host == "kiwi.example.org" && v[0].port == 8074 && v[0].users == 1 && v[0].usersMax == 4 && v[0].hasPosition && std::fabs(v[0].lat - 51.5) < 1e-9 && v[0].snr == 32 && v[0].location == "London, UK");
        CHECK(v[0].antenna == "Loop \xc3\xa9");
        CHECK(v[1].host == "10.1.2.3" && v[1].port == 8073 && v[1].full() && v[1].offline);
        CHECK(v[2].host == "2001:db8::1" && v[2].port == 8073 && v[2].loHz == 100000 && !v[2].hasPosition);
        kiwiSortByDistance(v, 55.0, 24.0);   // Lithuania: London is nearer than Sydney; no position goes last
        CHECK(v[0].host == "kiwi.example.org" && v[1].host == "10.1.2.3" && v[2].distanceKm < 0);
        CHECK(std::fabs(v[0].distanceKm - 1700) < 150);
    }
    CHECK(parseKiwiList("").empty() && parseKiwiList("not json").empty() && parseKiwiList("[").empty() && parseKiwiList("[{").empty() && parseKiwiList("[1,2]").empty());
    CHECK(parseKiwiList("[{\"url\":\"http://h:99999\"}]").size() == 1 && parseKiwiList("[{\"url\":\"http://h:99999\"}]")[0].port == 8073);
    std::mt19937 rng(5);
    for (int i = 0; i < 20000; i++) {   // truncations and byte flips of a valid list: no crash, no hang
        std::string x = js.substr(0, rng() % (js.size() + 1));
        for (int k = 0, n = rng() % 6; k < n && !x.empty(); k++) x[rng() % x.size()] = (char)rng();
        parseKiwiList(x);
    }
    CHECK(parseKiwiList(std::string(200000, '[')).empty());
    CHECK(parseKiwiList("[" + std::string(300000, '{')).empty());
    // GPS time: week second 123456 + the week of "now"
    const double now = 1.79e9;   // some day in 2026
    const double t = kiwiGpsToUnix(123456, 500000000, now);
    CHECK(t > 0 && std::fabs(t - now) < 302400 + 1 && std::fabs(std::fmod(t - 315964800.0 + 18, 604800.0) - 123456.5) < 1e-3);
    CHECK(kiwiGpsToUnix(700000, 0, now) < 0);
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    pureTests();
    listTests();
    native::kiwiSetBackoffForTest(80, 400);
    native::kiwiSetStallForTest(3000);   // the cut frame of the slow-link step pauses 1.4 s (past the 1 s receive timeout): the stall limit needs a wide margin over it on a loaded runner

    // network radio list: the type, its default port, the password in the list text (and nowhere else)
    NetRadio nr;
    nr.type = NetRadio::KiwiSdr;
    nr.host = "127.0.0.1";
    nr.remote = "s3cret pass";
    CHECK(netRadioDefaultPort(NetRadio::KiwiSdr) == 8073 && std::string(netRadioTypeName(NetRadio::KiwiSdr)) == "KiwiSDR");
    const std::string txt = netRadiosToString({nr});
    CHECK(txt == "kiwisdr,127.0.0.1,0,s3cret pass");
    CHECK(netRadiosFromString(txt).size() == 1 && netRadiosFromString(txt)[0] == nr);
    NetRadio nr2 = nr; nr2.remote = "a;b";
    CHECK(netRadiosFromString(netRadiosToString({nr2}))[0].remote == "ab");   // ';' would end the item

    // refusals at the start: told at once, the connection is not repeated
    for (auto mode : {Fake::Busy, Fake::BadPw, Fake::Down, Fake::Plain}) {
        Fake f; f.mode = mode; CHECK(f.start());
        NetRadio r = nr; r.port = f.port; r.remote = "";
        std::string e;
        CHECK(!probeNetRadio(r, 1500, e) && !e.empty());
        if (mode == Fake::Busy) CHECK(e.find("slots") != std::string::npos);
        if (mode == Fake::BadPw) CHECK(e.find("password") != std::string::npos);
        if (mode == Fake::Down) CHECK(e.find("down") != std::string::npos);
        const int before = f.connections;
        auto d = netRadioDevices({r})[0];
        auto src = native::makeKiwiSdr(d);
        IqRing ring(1u << 18);
        TuneSettings t; t.centerHz = 7074e3;
        std::string err;
        CHECK(!src->start(t, ring, err) && err.find("KiwiSDR") != std::string::npos);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(f.connections == before + 1);   // exactly one try
        f.stop();
    }

    Fake srv;
    CHECK(srv.start());
    NetRadio r = nr;
    r.port = srv.port;
    std::string err;
    CHECK(probeNetRadio(r, 2000, err));
    CHECK(srv.hasPrefix("SET auth t=kiwi p=s3cret%20pass"));
    CHECK(kiwiStatus("127.0.0.1:" + std::to_string(srv.port)).version == "1.800");

    const auto devs = netRadioDevices({r});
    CHECK(devs.size() == 1);
    if (devs.empty()) { srv.stop(); return 1; }
    DeviceInfo d = devs[0];
    CHECK(d.kind == DeviceInfo::Native && d.board == "kiwisdr" && d.nativeArgs == "127.0.0.1:" + std::to_string(srv.port) && d.name == netRadioDeviceName(r));
    CHECK(d.name.find("s3cret") == std::string::npos && d.serial.find("s3cret") == std::string::npos && d.nativeArgs.find("s3cret") == std::string::npos);
    CHECK(d.minRateHz == 252000 && d.maxRateHz == 1008000 && d.minFreqHz == 10e3 && d.maxFreqHz == 30e6 && d.gainMaxDb == 120);

    auto src = native::makeKiwiSdr(d);
    IqRing ring(1u << 22);
    TuneSettings t;
    t.centerHz = 7074e3 + 20000;   // a mode with a 20 kHz tune offset: the dial is 7074 kHz
    t.sampleRate = 2e6;
    t.radio["tune_offset_hz"] = "20000";
    err.clear();
    CHECK(src->start(t, ring, err));
    if (!err.empty()) fprintf(stderr, "start: %s\n", err.c_str());
    CHECK(eventually([&] { return srv.hasPrefix("SET mod=iq"); }));
    { std::lock_guard<std::mutex> lk(srv.mu); CHECK(srv.path.rfind("/kiwi/", 0) == 0 && srv.path.size() > 10 && srv.path.compare(srv.path.size() - 4, 4, "/SND") == 0); }
    CHECK(srv.has("SET mod=iq low_cut=-5000 high_cut=5000 freq=7074.000"));
    CHECK(srv.has("SET ident_user=OnAir") && srv.has("SET compression=0") && srv.has("SET AR OK in=12000 out=44100"));
    CHECK(eventually([&] { return srv.hasPrefix("SET agc=1 ") && srv.hasPrefix("SET keepalive"); }));
    CHECK(std::fabs(src->sampleRate() - 252000) < 1);
    std::vector<cf32> buf(30000);
    CHECK(readIq(ring, buf));
    // the fake sends a constant (0.5, -0.25) = DC at the Kiwi's centre = the dial; with the 20 kHz tune offset it sits at -20 kHz
    {
        std::vector<std::complex<float>> v(buf.begin() + 12000, buf.end());
        const double a = goertzel(v, 0, 15000, -20000, 252000), b = goertzel(v, 0, 15000, 0, 252000);
        CHECK(std::fabs(a - std::abs(std::complex<double>(0.5, -0.25))) < 0.02);
        CHECK(b < 0.02);
    }
    // status: S-meter 70.0 - 127 = -57 dBm, GPS time
    CHECK(eventually([&] { return kiwiStatus(d.nativeArgs).connected && kiwiStatus(d.nativeArgs).frames > 3; }));
    { const auto st = kiwiStatus(d.nativeArgs); CHECK(std::fabs(st.rssiDbm + 57.0) < 0.01 && st.gpsValid && st.gpsSec == 123456 && st.gpsNsec == 500000000); }

    // retune live: only what changed is sent; manual gain
    t.centerHz = 14074e3 + 20000;
    t.gainDb = 40;
    t.radio["agc"] = "manual";
    t.radio["passband"] = "3000";
    CHECK(src->retune(t, err));
    CHECK(eventually([&] { return srv.has("SET mod=iq low_cut=-3000 high_cut=3000 freq=14074.000") && srv.has("SET agc=0 hang=0 thresh=-100 slope=6 decay=1000 manGain=40"); }));
    CHECK(srv.countPrefix("SET mod=iq") == 2);
    t.centerHz = 100e3 * 1e3;   // 100 MHz: out of the Kiwi's range: clamped to 30 MHz, a note
    err.clear();
    CHECK(src->retune(t, err));
    CHECK(eventually([&] { return srv.hasPrefix("SET mod=iq low_cut=-3000 high_cut=3000 freq=30000.000") || srv.hasPrefix("SET mod=iq low_cut=-3000 high_cut=3000 freq=29980.000"); }));
    t.centerHz = 7074e3 + 20000;
    CHECK(src->retune(t, err));

    // keepalive: about one a second
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    CHECK(srv.countPrefix("SET keepalive") >= 3 && srv.countPrefix("SET keepalive") <= 8);

    // a sequence gap is filled with silence and counted; junk frames are ignored; the stream goes on
    ring.clear();
    const uint64_t g0 = kiwiStatus(d.nativeArgs).gapFrames;
    srv.gap = 1;
    srv.junk = 1;
    CHECK(eventually([&] { return kiwiStatus(d.nativeArgs).gapFrames >= g0 + 5; }));
    CHECK(eventually([&] { return kiwiStatus(d.nativeArgs).badFrames >= 4; }));
    std::vector<cf32> b2(40000);
    CHECK(readIq(ring, b2));
    bool finite = true;
    for (auto& v : b2) finite = finite && std::isfinite(v.real()) && std::isfinite(v.imag());
    CHECK(finite);
    CHECK(srv.connections == 2);   // the probe and the stream; no reconnect for junk

    // the server's sequence starts again from 0: the frames are followed, not dropped for good (issue #257)
    srv.resetSeq = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ring.clear();
    CHECK(readIq(ring, b2));
    CHECK(srv.connections == 2);

    // a stall with the connection still open: messages, or audio frames, but no IQ. The source says so and connects again (issue #257)
    for (int q : {1, 2}) {
        const int cs = srv.connections;
        const char* want = q == 1 ? "messages but no IQ" : "audio frames but no IQ";
        srv.noIq = q;
        CHECK(eventually([&] { return kiwiStatus(d.nativeArgs).message.find(want) != std::string::npos; }, 6000));
        srv.noIq = 0;
        CHECK(eventually([&] { return srv.connections > cs; }, 8000));
        ring.clear();
        CHECK(readIq(ring, buf));
    }

    // a frame cut in two by a slow link, the pause longer than the receive timeout: the rest is waited for, the connection stays (#257)
    {
        // a reconnect of the step before can still be on its way on a loaded machine: take the count once it has been still for 2 s
        for (int i = 0, last = -1; i < 10; i++) { const int c = srv.connections; if (c == last) break; last = c; std::this_thread::sleep_for(std::chrono::milliseconds(2000)); }
        const int cs = srv.connections;
        srv.slowFrame = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        CHECK(srv.connections == cs);
        ring.clear();
        CHECK(readIq(ring, buf));
        CHECK(srv.connections == cs);
    }

    // an oversize message: the connection is dropped, and opened again after the pause (80 ms in the test)
    const int c0 = srv.connections;
    srv.hugeFrame = 1;
    CHECK(eventually([&] { return srv.connections > c0; }, 8000));
    ring.clear();
    CHECK(readIq(ring, buf));
    CHECK(srv.countPrefix("SET mod=iq") >= 3);   // the new connection got the tuning

    // a time limit from the server: told, then connected again after the longer pause
    const int c1 = srv.connections;
    srv.timeLimit = 1;
    CHECK(eventually([&] { return kiwiStatus(d.nativeArgs).message.find("time limit") != std::string::npos; }, 3000));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(eventually([&] { return srv.connections > c1; }, 8000));
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(waited > 0.3);   // a hold of at least 12 x the minimum pause (80 ms) -- not the next second
    ring.clear();
    CHECK(readIq(ring, buf));

    // the server goes away and comes back
    const int c2 = srv.connections;
    srv.dropNow = 1;
    CHECK(eventually([&] { return srv.connections > c2; }, 8000));
    ring.clear();
    CHECK(readIq(ring, buf));
    src->stop();
    CHECK(!kiwiStatus(d.nativeArgs).connected);
    srv.stop();

    if (fails) { fprintf(stderr, "%d checks failed\n", fails); return 1; }
    printf("test_kiwisdr ok\n");
    return 0;
}
