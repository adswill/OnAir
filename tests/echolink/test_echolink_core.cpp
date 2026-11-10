// EchoLink: GSM 06.10 vectors and codec quality, RTP / RTCP framing, the directory protocol against a mock server on 127.0.0.1, two clients
// talking over loopback, the connection timers on a fake clock, the rate converters, the VOX and the RF bridge rules.
// Nothing here contacts the real EchoLink servers: every socket is on 127.0.0.1.
#include "dect2/echolink/el_audio.h"
#include "dect2/echolink/el_client.h"
#include "dect2/echolink/el_proto.h"
#include "dect2/echolink/gsm610.h"
#include "../../core/src/net/tcp_client.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace dect2::echolink;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#include "gsm_vectors.inc"

static uint64_t fnv(const int16_t* s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        const uint16_t v = (uint16_t)s[i];
        h = (h ^ (v & 0xFF)) * 0x100000001b3ULL;
        h = (h ^ (v >> 8)) * 0x100000001b3ULL;
    }
    return h;
}
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class F> static bool waitFor(F&& f, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; sleepMs(10); }
    return f();
}

// A voiced, speech-like test signal: harmonics of a gliding pitch shaped by two formants, a syllable envelope and some noise
static std::vector<int16_t> speechLike(int n, double level, double dc, double noise, unsigned seed) {
    std::vector<int16_t> x((size_t)n);
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    double ph = 0;
    for (int i = 0; i < n; i++) {
        const double t = i / 8000.0;
        const double f0 = 120 + 30 * std::sin(2 * M_PI * 0.9 * t);
        ph += 2 * M_PI * f0 / 8000;
        double v = 0;
        for (int h = 1; h * f0 < 3500; h++) {
            const double fh = h * f0;
            v += (std::exp(-std::pow((fh - 650) / 250, 2)) + 0.6 * std::exp(-std::pow((fh - 1700) / 350, 2)) + 0.04) * std::sin(h * ph);
        }
        v *= 0.55 + 0.45 * std::sin(2 * M_PI * 2.1 * t);
        const double s = v * level + dc + g(rng) * noise;
        x[(size_t)i] = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(s)));
    }
    return x;
}
static double snrDb(const std::vector<int16_t>& a, const std::vector<int16_t>& b, size_t from) {
    double s = 0, e = 0;
    for (size_t i = from; i < a.size() && i < b.size(); i++) { s += (double)a[i] * a[i]; e += ((double)a[i] - b[i]) * ((double)a[i] - b[i]); }
    return e > 0 ? 10 * std::log10(s / e) : 99;
}
static double tonePower(const int16_t* x, int n, double f, double fs) {
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) { re += x[i] * std::cos(2 * M_PI * f * i / fs); im += x[i] * std::sin(2 * M_PI * f * i / fs); }
    return (re * re + im * im) / ((double)n * n);
}

// ---- GSM 06.10 ---------------------------------------------------------------------------------------------------------------------------
static void testGsmVectors() {
    struct V { const uint8_t* d; size_t n; uint64_t hash; const int16_t* probe; size_t at; const char* name; };
    const V vs[] = {{kVecSpeech, sizeof kVecSpeech, kVecSpeechHash, kVecSpeechAt3200, 3200, "speech"},
                    {kVecRandom, sizeof kVecRandom, kVecRandomHash, kVecRandomFirst, 0, "noise and tones"}};
    for (const V& v : vs) {
        GsmDecoder dec;
        std::vector<int16_t> pcm;
        for (size_t f = 0; f + kGsmFrameBytes <= v.n; f += kGsmFrameBytes) {
            int16_t o[kGsmFrameSamples];
            CHECK(dec.decode(v.d + f, o), "%s frame %zu: signature", v.name, f / kGsmFrameBytes);
            pcm.insert(pcm.end(), o, o + kGsmFrameSamples);
        }
        CHECK(fnv(pcm.data(), pcm.size()) == v.hash, "%s: decoded PCM differs from FFmpeg's GSM decoder (hash %016llx)", v.name, (unsigned long long)fnv(pcm.data(), pcm.size()));
        for (int i = 0; i < 8; i++) CHECK(pcm[v.at + (size_t)i] == v.probe[i], "%s: sample %zu is %d, FFmpeg %d", v.name, v.at + (size_t)i, pcm[v.at + (size_t)i], v.probe[i]);
    }
}

static void testGsmPacking() {
    std::mt19937 rng(5);
    const int lb[8] = {6, 6, 5, 5, 4, 4, 3, 3};
    for (int it = 0; it < 2000; it++) {
        GsmParams p;
        for (int i = 0; i < 8; i++) p.LARc[i] = (int16_t)(rng() % (1u << lb[i]));
        for (int s = 0; s < 4; s++) {
            p.Nc[s] = (int16_t)(rng() % 128); p.bc[s] = (int16_t)(rng() % 4); p.Mc[s] = (int16_t)(rng() % 4); p.xmaxc[s] = (int16_t)(rng() % 64);
            for (int i = 0; i < 13; i++) p.xMc[s][i] = (int16_t)(rng() % 8);
        }
        uint8_t b[kGsmFrameBytes];
        gsmPack(p, b);
        CHECK((b[0] >> 4) == 0xD, "signature");
        GsmParams q;
        CHECK(gsmUnpack(b, q) && std::memcmp(&p, &q, sizeof p) == 0, "pack / unpack round trip %d", it);
        if (fails) return;
    }
    uint8_t bad[kGsmFrameBytes] = {0x10};
    int16_t o[kGsmFrameSamples];
    GsmDecoder d;
    CHECK(!d.decode(bad, o), "a frame without the 0xD signature is refused");
    bool silent = true;
    for (int16_t s : o) silent = silent && s == 0;
    CHECK(silent, "a refused frame decodes to silence");
}

static void testGsmQuality() {
    // level sweep (quiet to near full scale), a DC offset (the encoder's offset compensation), added noise, and a clipped burst
    struct C { double level, dc, noise, minSnr; } cs[] = {
        {300, 0, 0, 6}, {1500, 0, 0, 7}, {5000, 0, 0, 7}, {12000, 0, 0, 7}, {5000, 1500, 0, 6.5}, {5000, -2500, 0, 6}, {5000, 0, 300, 5}, {5000, 0, 1200, 2.5}};
    for (const C& c : cs) {
        std::vector<int16_t> x = speechLike(8000 * 3, c.level, c.dc, c.noise, 11);
        std::vector<int16_t> ref = speechLike(8000 * 3, c.level, 0, c.noise, 11);   // the encoder removes DC: compare with the signal without it
        GsmEncoder e; GsmDecoder d;
        std::vector<int16_t> y;
        for (size_t f = 0; f + kGsmFrameSamples <= x.size(); f += kGsmFrameSamples) {
            uint8_t b[kGsmFrameBytes]; int16_t o[kGsmFrameSamples];
            e.encode(&x[f], b);
            CHECK(d.decode(b, o), "signature");
            y.insert(y.end(), o, o + kGsmFrameSamples);
        }
        const double snr = snrDb(ref, y, 4000);
        printf("GSM level %.0f dc %.0f noise %.0f: SNR %.1f dB\n", c.level, c.dc, c.noise, snr);
        CHECK(snr >= c.minSnr, "SNR %.1f dB < %.1f", snr, c.minSnr);
    }
    // overload: a full-scale square wave must not wrap around (saturating arithmetic)
    std::vector<int16_t> sq(8000);
    for (size_t i = 0; i < sq.size(); i++) sq[i] = ((i / 20) & 1) ? 32767 : -32768;
    GsmEncoder e; GsmDecoder d;
    double peak = 0;
    for (size_t f = 0; f < sq.size(); f += kGsmFrameSamples) {
        uint8_t b[kGsmFrameBytes]; int16_t o[kGsmFrameSamples];
        e.encode(&sq[f], b); d.decode(b, o);
        for (int16_t s : o) peak = std::max(peak, std::fabs((double)s));
    }
    CHECK(peak > 16000, "full-scale input decodes loud (peak %.0f)", peak);
    // silence in, silence out
    std::vector<int16_t> z(1600, 0);
    e.reset(); d.reset();
    for (size_t f = 0; f < z.size(); f += kGsmFrameSamples) {
        uint8_t b[kGsmFrameBytes]; int16_t o[kGsmFrameSamples];
        e.encode(&z[f], b); d.decode(b, o);
        for (int16_t s : o) CHECK(std::abs(s) <= 64, "silence decodes as %d", s);   // the smallest RPE step is not zero
    }
}

// ---- RTP / RTCP ------------------------------------------------------------------------------------------------------------------------------
static void testRtcp() {
    const std::vector<uint8_t> s = makeSdes("LY1ABC", "Kris", "");
    // hand-built from SvxLink's rtp_make_sdes: empty RR, SDES header, items, END, padding
    std::vector<uint8_t> want = {0xC0, 201, 0, 1, 0, 0, 0, 0, 0xC1, 0xCA, 0, 0, 0, 0, 0, 0};
    auto item = [&](uint8_t t, const std::string& x) { want.push_back(t); want.push_back((uint8_t)x.size()); want.insert(want.end(), x.begin(), x.end()); };
    item(1, "CALLSIGN"); item(2, "LY1ABC         Kris"); item(3, "CALLSIGN"); item(4, "08:30");
    want.push_back(0); want.push_back(0);
    while ((want.size() - 8) & 3) want.push_back(0);
    const size_t len = (want.size() - 8) / 4 - 1;
    want[10] = (uint8_t)(len >> 8); want[11] = (uint8_t)len;
    CHECK(s == want, "SDES bytes (%zu vs %zu)", s.size(), want.size());
    CHECK(s.size() % 4 == 0, "SDES is a multiple of 4 bytes");
    CHECK(isRtcpSdes(s.data(), s.size()) && !isRtcpBye(s.data(), s.size()), "SDES is recognised");
    std::string nm, call, name;
    CHECK(sdesItem(s.data(), s.size(), 2, nm) && nm == "LY1ABC         Kris", "NAME item '%s'", nm.c_str());
    CHECK(splitSdesName(nm, call, name) && call == "LY1ABC" && name == "Kris", "split '%s' '%s'", call.c_str(), name.c_str());
    std::string priv;
    CHECK(!sdesItem(s.data(), s.size(), 8, priv), "no PRIV item");
    const std::vector<uint8_t> sp = makeSdes("LY1ABC-L", "Link", "SPEEX");
    CHECK(sdesItem(sp.data(), sp.size(), 8, priv) && priv == "SPEEX", "PRIV item");

    const std::vector<uint8_t> b = makeBye();
    const uint8_t wantBye[] = {0xC0, 201, 0, 1, 0, 0, 0, 0, 0xC1, 0xCB, 0, 3, 0, 0, 0, 0, 7, 'j', 'a', 'n', '2', '0', '0', '2'};
    CHECK(b.size() == sizeof wantBye && std::memcmp(b.data(), wantBye, sizeof wantBye) == 0, "BYE bytes");
    CHECK(isRtcpBye(b.data(), b.size()) && !isRtcpSdes(b.data(), b.size()), "BYE is recognised");

    // every truncation and random bytes: no crash, no false SDES with a callsign
    for (size_t n = 0; n < s.size(); n++) {
        std::string t;
        if (sdesItem(s.data(), n, 2, t)) CHECK(t.size() <= n, "item inside the packet");
    }
    std::mt19937 rng(9);
    for (int it = 0; it < 20000; it++) {
        std::vector<uint8_t> r(rng() % 80);
        for (uint8_t& c : r) c = (uint8_t)rng();
        if (!r.empty() && (it & 1)) r[0] = 0xC0;
        if (r.size() > 1 && (it & 2)) r[1] = 201;
        std::string t;
        sdesItem(r.data(), r.size(), 2, t);
        isRtcpBye(r.data(), r.size());
        CHECK(t.size() <= r.size(), "fuzz");
    }
    CHECK(sanitizeCallsign("LY1ABC\x01;rm -rf") == "LY1ABCrm-rf", "sanitise '%s'", sanitizeCallsign("LY1ABC\x01;rm -rf").c_str());
    CHECK(sanitizeCallsign(std::string(100, 'A')).size() == 32, "callsign length capped");
}

static void testRtp() {
    uint8_t fr[kFramesPerPacket][kGsmFrameBytes];
    for (int f = 0; f < kFramesPerPacket; f++) for (int i = 0; i < kGsmFrameBytes; i++) fr[f][i] = (uint8_t)(f * 40 + i);
    const std::vector<uint8_t> p = makeAudioPacket(0x1234, fr);
    CHECK(p.size() == 144, "audio packet is 144 bytes");
    CHECK(p[0] == 0xC0 && p[1] == 0x03 && p[2] == 0x12 && p[3] == 0x34, "RTP header");
    for (int i = 4; i < 12; i++) CHECK(p[i] == 0, "timestamp / SSRC zero");
    CHECK(classifyAudioPort(p.data(), p.size()) == AudioPortKind::Audio, "classified as audio");
    uint16_t seq; uint8_t pt; const uint8_t* f; int nf;
    CHECK(parseAudioPacket(p.data(), p.size(), seq, pt, f, nf) && seq == 0x1234 && nf == 4 && std::memcmp(f, fr, sizeof fr) == 0, "parse");
    std::vector<uint8_t> sx = p; sx[1] = kPtSpeex;
    CHECK(!parseAudioPacket(sx.data(), sx.size(), seq, pt, f, nf) && pt == kPtSpeex, "Speex refused");
    CHECK(!parseAudioPacket(p.data(), 11, seq, pt, f, nf), "short packet");
    CHECK(parseAudioPacket(p.data(), 12 + 33 * 2 + 5, seq, pt, f, nf) && nf == 2, "partial packet: whole frames only");

    const std::vector<uint8_t> in = makeInfoPacket("OnAir 3.9\nLY1ABC");
    std::string t;
    CHECK(classifyAudioPort(in.data(), in.size()) == AudioPortKind::Info && parseInfoPacket(in.data(), in.size(), t) && t == "OnAir 3.9\nLY1ABC", "info '%s'", t.c_str());
    CHECK(in.back() == 0 && std::memcmp(in.data(), "oNDATA\r", 7) == 0, "info layout");
    const std::vector<uint8_t> ch = makeChatPacket("LY1ABC", "hello\r\nthere");
    CHECK(std::string((const char*)ch.data()) == "oNDATALY1ABC>hellothere\r\n", "chat layout (line breaks removed)");
    CHECK(parseChatPacket(ch.data(), ch.size(), t) && t == "LY1ABC>hellothere", "chat '%s'", t.c_str());
    const uint8_t noNul[] = {'o', 'N', 'D', 'A', 'T', 'A', 'x'};
    CHECK(!parseChatPacket(noNul, sizeof noNul, t), "chat without NUL refused");
}

// ---- directory -------------------------------------------------------------------------------------------------------------------------------
static const char* kList =
    "@@@\n6\n"
    " \nWelcome to the mock server [ON 00:00]\n0\n0.0.0.0\n"
    "LY1ABC\nVilnius [ON 12:34]\n123456\n127.0.0.1\n"
    ".\n\n0\n0.0.0.0\n"
    "LY2XYZ-L\nKaunas 145.500 [BUSY 08:15]\n234567\n10.0.0.2\n"
    "LY3QQ-R\nKlaipeda 438.725  [ON 21:00]\n345678\n10.0.0.3\n"
    "*LITHUANIA*\nConference server  [ON 23:59]\n456789\n10.0.0.4\n"
    "W1AW\nNewington CT [ON 01:02]\n567890\n10.0.0.5\n"
    "+++";

static void testDirectoryFormats() {
    const std::string l = loginCommand("ly1abc", "s3cret", DirStatus::Online, "12:34", "Vilnius KO24\r\n and a very long location text");
    CHECK(l == std::string("lLY1ABC\xAC\xACs3cret\rONLINE3.38(12:34)\rVilnius KO24 and a very lon\r"), "log-in command");
    CHECK(loginCommand("LY1ABC", "p", DirStatus::Busy, "01:02", "x").find("\rBUSY3.40(01:02)\r") != std::string::npos, "busy");
    CHECK(loginCommand("LY1ABC", "p", DirStatus::Offline, "01:02", "x").find("\rOFF-V3.40\r") != std::string::npos, "offline");
    const std::string r = loginCommandRedacted("LY1ABC", DirStatus::Online, "12:34", "Vilnius");
    CHECK(r.find("s3cret") == std::string::npos && r.find("****") != std::string::npos, "redacted");
    CHECK(validCallsign("LY1ABC") && validCallsign("LY1ABC-L") && validCallsign("LY1ABC-R") && validCallsign("*ECHOTEST*") && validCallsign("W1AW"), "valid calls");
    CHECK(!validCallsign("") && !validCallsign("ABC") && !validCallsign("ly1abc") && !validCallsign("LY1 ABC") && !validCallsign("12345"), "invalid calls");

    // every chunk size gives the same list
    for (size_t chunk = 1; chunk <= 40; chunk++) {
        StationListParser p;
        const std::string all = kList;
        for (size_t i = 0; i < all.size(); i += chunk) p.feed(all.data() + i, std::min(chunk, all.size() - i));
        CHECK(p.done() && !p.failed(), "chunk %zu: done", chunk);
        const auto& s = p.stations();
        CHECK(s.size() == 5, "chunk %zu: %zu stations", chunk, s.size());
        if (s.size() != 5) continue;
        CHECK(s[0].call == "LY1ABC" && s[0].description == "Vilnius" && s[0].online && !s[0].busy && s[0].time == "12:34" && s[0].id == 123456 && s[0].ip == "127.0.0.1", "entry 0");
        CHECK(s[1].kind == StationKind::Link && s[1].busy && s[1].time == "08:15" && s[1].description == "Kaunas 145.500", "entry 1 '%s'", s[1].description.c_str());
        CHECK(s[2].kind == StationKind::Repeater && s[3].kind == StationKind::Conference && s[4].kind == StationKind::User, "kinds");
        CHECK(p.message() == "Welcome to the mock server\n" && !p.badPassword(), "message '%s'", p.message().c_str());
    }
    StationListParser bad;
    bad.feed("HTTP/1.1 200 OK\r\n", 17);
    CHECK(bad.failed(), "not a list");
    StationListParser pw;
    const std::string pwl = "@@@\n1\n \nINCORRECT PASSWORD [ON 00:00]\n0\n0.0.0.0\n+++";
    pw.feed(pwl.data(), pwl.size());
    CHECK(pw.done() && pw.badPassword() && pw.stations().empty(), "incorrect password message");

    StationListParser p;
    p.feed(kList, std::strlen(kList));
    CHECK(searchStations(p.stations(), "kaunas").size() == 1, "search description");
    CHECK(searchStations(p.stations(), "ly").size() == 3, "search callsign");
    CHECK(searchStations(p.stations(), "4567").size() == 3, "search node number");
    CHECK(searchStations(p.stations(), "", 1u << (unsigned)StationKind::Conference).size() == 1, "conferences only");
    CHECK(searchStations(p.stations(), "", (1u << (unsigned)StationKind::Link) | (1u << (unsigned)StationKind::Repeater)).size() == 2, "links and repeaters");
}

// The mock directory server: one command per connection like the real one. Password "s3cret" is right; "s" gives kList.
struct MockDir {
    dect2::sock_t ls = dect2::kBadSock;
    int port = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> logins{0}, lists{0}, offs{0};
    std::string lastCommand;
    std::thread th;
    bool start() {
        dect2::netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        socklen_t al = sizeof a; getsockname(ls, (sockaddr*)&a, &al);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    void loop() {
        while (!stop) {
            fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
            timeval tv{0, 50000};
            if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
            dect2::sock_t c = accept(ls, nullptr, nullptr);
            if (c == dect2::kBadSock) continue;
            dect2::sockRecvTimeout(c, 2000);
            std::string cmd; char b[512];
            for (;;) {
                const long long n = dect2::sockRecv(c, b, sizeof b);
                if (n <= 0) break;
                cmd.append(b, (size_t)n);
                if (cmd == "s") break;
                if (cmd[0] == 'l' && std::count(cmd.begin(), cmd.end(), '\r') >= 3) break;
            }
            lastCommand = cmd;
            std::string reply;
            if (cmd == "s") { reply = kList; lists++; }
            else if (cmd.size() > 1 && cmd[0] == 'l') {
                const size_t sep = cmd.find("\xAC\xAC"), cr = cmd.find('\r');
                const std::string pass = sep != std::string::npos && cr != std::string::npos ? cmd.substr(sep + 2, cr - sep - 2) : "";
                reply = pass == "s3cret" ? "OK2.6\r" : "BAD\r";
                if (cmd.find("OFF-V") != std::string::npos) offs++; else logins++;
            }
            dect2::sockSendAll(c, reply.data(), reply.size());
            dect2::sockClose(c);
        }
    }
    ~MockDir() { stop = true; if (th.joinable()) th.join(); if (ls != dect2::kBadSock) dect2::sockClose(ls); }
};

static int pickBase(int salt) {
#ifdef _WIN32
    const int pid = 1234;
#else
    const int pid = (int)getpid();
#endif
    return 21000 + ((pid * 7 + salt * 131) % 1500) * 20;
}

static void testDirectoryClient() {
    MockDir m;
    CHECK(m.start(), "mock server");
    Client c;
    ClientConfig cfg;
    cfg.call = "ly1abc"; cfg.password = "wrong"; cfg.name = "Kris"; cfg.location = "Vilnius";
    cfg.servers = {"127.0.0.1"}; cfg.directoryPort = m.port; cfg.directoryTimeoutMs = 3000;
    cfg.audioPort = pickBase(1);
    c.configure(cfg);
    std::string err;
    bool opened = c.open(err);
    for (int i = 2; !opened && i < 10; i++) { cfg.audioPort = pickBase(i); c.configure(cfg); opened = c.open(err); }
    CHECK(opened, "open: %s", err.c_str());
    c.login();
    CHECK(waitFor([&] { return !c.status().busy && m.logins == 1; }, 4000), "log-in sent");
    sleepMs(50);
    ClientStatus s = c.status();
    CHECK(!s.loggedIn && s.directory.find("refused") != std::string::npos, "wrong password refused: '%s'", s.directory.c_str());
    CHECK(m.lastCommand.find("wrong") != std::string::npos && m.lastCommand.rfind("lLY1ABC\xAC\xAC", 0) == 0, "the password goes to the server only");
    cfg.password = "s3cret";
    c.configure(cfg);
    c.login();
    CHECK(waitFor([&] { return c.status().loggedIn; }, 4000), "logged in: '%s'", c.status().directory.c_str());
    c.refreshList();
    CHECK(waitFor([&] { return !c.status().busy && c.stations().size() == 5; }, 4000), "station list (%zu)", c.stations().size());
    CHECK(c.status().listAgeSec >= 0, "list age");
    for (const std::string& l : c.takeLog()) {
        CHECK(l.find("s3cret") == std::string::npos && l.find("wrong") == std::string::npos, "log line holds the password: %s", l.c_str());
    }
    CHECK(c.status().directory.find("s3cret") == std::string::npos, "status holds no password");
    c.close();
    CHECK(m.offs == 1, "close logs out (OFF) once: %d", m.offs.load());

    // a server that is not there
    Client d;
    cfg.directoryPort = 1;   // nothing listens on 127.0.0.1:1
    cfg.audioPort = pickBase(20);
    d.configure(cfg);
    bool o2 = d.open(err);
    for (int i = 21; !o2 && i < 30; i++) { cfg.audioPort = pickBase(i); d.configure(cfg); o2 = d.open(err); }
    d.login();
    CHECK(waitFor([&] { return !d.status().busy && !d.status().directory.empty(); }, 5000), "unreachable server answers");
    CHECK(!d.status().loggedIn && d.status().directory.find("cannot reach 127.0.0.1") != std::string::npos, "'%s'", d.status().directory.c_str());
    d.close();
}

// ---- two clients over loopback -------------------------------------------------------------------------------------------------------------
static bool openAt(Client& c, ClientConfig& cfg, int salt) {
    std::string err;
    for (int i = 0; i < 12; i++) { cfg.audioPort = pickBase(salt + i); c.configure(cfg); if (c.open(err)) return true; }
    printf("open failed: %s\n", err.c_str());
    return false;
}

static void testLoopbackQso() {
    Client a, b, x;
    ClientConfig ca, cb, cx;
    ca.call = "LY1ABC"; ca.name = "Kris"; ca.info = "OnAir test A"; ca.servers.clear();
    cb.call = "LY2XYZ-L"; cb.name = "Link"; cb.info = "Link B\nsecond line"; cb.servers.clear();
    cx.call = "LY3QQ"; cx.name = "Third"; cx.servers.clear();
    CHECK(openAt(a, ca, 40) && openAt(b, cb, 60) && openAt(x, cx, 80), "ports");
    CHECK(a.connectTo("127.0.0.1", b.localAudioPort()), "connect");
    CHECK(waitFor([&] { return a.status().qso == QsoState::Connected && b.status().qso == QsoState::Connected; }, 3000), "both connected");
    CHECK(a.status().peerCall == "LY2XYZ-L" && b.status().peerCall == "LY1ABC" && b.status().peerName == "Kris", "names '%s' '%s'", a.status().peerCall.c_str(), b.status().peerCall.c_str());
    CHECK(waitFor([&] { return a.status().peerInfo == "Link B\nsecond line" && b.status().peerInfo == "OnAir test A"; }, 2000), "info texts '%s'", a.status().peerInfo.c_str());

    // a third station calling the busy link is told BYE and stays idle
    CHECK(x.connectTo("127.0.0.1", b.localAudioPort()), "third connect");
    CHECK(waitFor([&] { return x.status().qso == QsoState::Idle; }, 3000), "busy station refuses");
    CHECK(b.status().peerCall == "LY1ABC", "the first QSO stays");

    // A talks: 2 s of a 1 kHz tone at -12 dBFS
    a.setTransmit(true);
    std::vector<int16_t> tone(8000 * 2);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = (int16_t)(8200 * std::sin(2 * M_PI * 1000.0 * (double)i / 8000));
    for (size_t i = 0; i < tone.size(); i += 640) { a.pushTx(&tone[i], 640); sleepMs(20); }
    a.setTransmit(false);
    CHECK(waitFor([&] { return b.rxBuffered() >= 15000; }, 3000), "B received %d samples", b.rxBuffered());
    std::vector<int16_t> rx(16000);
    const int got = b.pullRx(rx.data(), (int)rx.size());
    CHECK(got >= 15000, "pulled %d", got);
    if (got >= 4800) {
        const double p1k = tonePower(rx.data() + 1600, 3200, 1000, 8000), p2k = tonePower(rx.data() + 1600, 3200, 2000, 8000);
        double rms = 0;
        for (int i = 1600; i < 4800; i++) rms += (double)rx[(size_t)i] * rx[(size_t)i];
        rms = std::sqrt(rms / 3200);
        printf("loopback: 1 kHz amplitude %.0f (sent 8200), 2 kHz %.0f, rms %.0f, lost %llu\n", 2 * std::sqrt(p1k), 2 * std::sqrt(p2k), rms, (unsigned long long)b.status().lost);
        CHECK(2 * std::sqrt(p1k) > 8200 * 0.7 && 2 * std::sqrt(p1k) < 8200 * 1.3, "tone level");
        CHECK(p1k > 100 * p2k, "tone is clean");
    }
    CHECK(b.status().lost == 0 && a.status().packetsOut >= 24, "packets %llu out", (unsigned long long)a.status().packetsOut);

    CHECK(a.sendChat("hello from A"), "chat");
    std::vector<std::string> chats;
    CHECK(waitFor([&] { for (auto& s : b.takeChat()) chats.push_back(s); return !chats.empty(); }, 2000), "chat arrives");
    CHECK(!chats.empty() && chats[0] == "LY1ABC>hello from A", "chat '%s'", chats.empty() ? "" : chats[0].c_str());

    b.disconnect();
    CHECK(waitFor([&] { return a.status().qso == QsoState::Idle; }, 2000), "BYE ends the QSO on the other side");
    bool sawBye = false;
    for (const std::string& l : a.takeLog()) sawBye = sawBye || l.find("LY2XYZ-L disconnected") != std::string::npos;
    CHECK(sawBye, "the log says who disconnected");

    // not accepting calls
    cb.acceptIncoming = false;
    b.configure(cb);
    CHECK(a.connectTo("127.0.0.1", b.localAudioPort()), "connect again");
    CHECK(waitFor([&] { return a.status().qso == QsoState::Idle; }, 3000), "refused when calls are off");
    a.close(); b.close(); x.close();
}

// ---- timers on a fake clock, loss and reordering --------------------------------------------------------------------------------------
static void testSessionTimers() {
    struct Out { bool ctrl; std::string ip; int port; std::vector<uint8_t> p; };
    std::vector<Out> out;
    Session s;
    s.setLocal("LY1ABC", "Kris", "");
    s.setSend([&](bool c, const std::string& ip, int port, const std::vector<uint8_t>& p) { out.push_back({c, ip, port, p}); });
    CHECK(s.connect("10.1.2.3", 5198, 0), "connect");
    CHECK(out.size() == 1 && out[0].ctrl && out[0].port == 5199 && isRtcpSdes(out[0].p.data(), out[0].p.size()), "first SDES to 5199");
    int64_t t = 0;
    int sdes = 1;
    for (; t <= 60000 && s.state() == QsoState::Connecting; t += 100) {
        const size_t before = out.size();
        s.tick(t);
        sdes += (int)(out.size() - before);
    }
    CHECK(s.state() == QsoState::Idle && sdes == kConnectRetries && t >= 49000 && t <= 51000, "gave up after %d SDES at %lld ms", sdes, (long long)t);

    out.clear();
    Session r;
    r.setLocal("LY1ABC", "Kris", "");
    r.setSend([&](bool c, const std::string& ip, int port, const std::vector<uint8_t>& p) { out.push_back({c, ip, port, p}); });
    r.connect("10.1.2.3", 5198, 0);
    const std::vector<uint8_t> peer = makeSdes("LY2XYZ", "Peer");
    r.onCtrl("10.9.9.9", 5199, peer.data(), peer.size(), 100);   // someone else: refused with BYE, the call goes on
    CHECK(r.state() == QsoState::Connecting && isRtcpBye(out.back().p.data(), out.back().p.size()) && out.back().ip == "10.9.9.9", "stranger refused");
    r.onCtrl("10.1.2.3", 40001, peer.data(), peer.size(), 200);   // the peer answers from a NAT port
    CHECK(r.state() == QsoState::Connected && r.peerCall() == "LY2XYZ", "connected");
    CHECK(!out.back().ctrl && out.back().port == 5198 && classifyAudioPort(out.back().p.data(), out.back().p.size()) == AudioPortKind::Info, "info sent on connect");
    // keep-alives every 10 s to the port the peer used; the peer's SDES every 10 s keeps it up for minutes
    int keep = 0;
    for (t = 200; t <= 180000; t += 100) {
        const size_t before = out.size();
        r.tick(t);
        for (size_t i = before; i < out.size(); i++) if (out[i].ctrl && isRtcpSdes(out[i].p.data(), out[i].p.size())) { keep++; CHECK(out[i].port == 40001, "keep-alive port"); }
        if (t % 10000 == 0) r.onCtrl("10.1.2.3", 40001, peer.data(), peer.size(), t);
    }
    CHECK(r.state() == QsoState::Connected && keep >= 17 && keep <= 19, "kept up, %d keep-alives", keep);
    // packets lost, duplicated and reordered
    uint8_t fr[4][33];
    for (auto& f : fr) { std::memset(f, 0, 33); f[0] = 0xD0; }
    const uint16_t seqs[] = {100, 101, 103, 103, 102, 104, 110, 65535, 111};
    for (uint16_t q : seqs) { const auto p = makeAudioPacket(q, fr); r.onAudio("10.1.2.3", 5198, p.data(), p.size(), t); }
    CHECK(r.packetsLost() == 1 + 5 && r.packetsLate() == 3 && r.takeAudio().size() == 6, "lost %llu late %llu", (unsigned long long)r.packetsLost(), (unsigned long long)r.packetsLate());
    // sequence wrap
    Session w;
    w.setLocal("LY1ABC", "", "");
    w.setSend([&](bool, const std::string&, int, const std::vector<uint8_t>&) {});
    w.setAcceptIncoming(true);
    w.onCtrl("10.1.2.4", 5199, peer.data(), peer.size(), 0);
    CHECK(w.state() == QsoState::Connected, "incoming accepted");
    for (uint16_t q : {(uint16_t)65534, (uint16_t)65535, (uint16_t)0, (uint16_t)1}) { const auto p = makeAudioPacket(q, fr); w.onAudio("10.1.2.4", 5198, p.data(), p.size(), 0); }
    CHECK(w.packetsLost() == 0 && w.packetsLate() == 0 && w.takeAudio().size() == 4, "sequence wraps at 65535");
    // silence from the peer: gone after 50 s
    for (t = 0; t <= 60000 && w.state() == QsoState::Connected; t += 100) w.tick(t);
    CHECK(w.state() == QsoState::Idle && t >= 49000 && t <= 51000, "time-out at %lld ms", (long long)t);
    // audio from a stranger is ignored
    Session z;
    z.setLocal("LY1ABC", "", "");
    z.setSend([&](bool, const std::string&, int, const std::vector<uint8_t>&) {});
    z.onCtrl("10.1.2.5", 5199, peer.data(), peer.size(), 0);
    const auto p = makeAudioPacket(1, fr);
    z.onAudio("10.6.6.6", 5198, p.data(), p.size(), 0);
    CHECK(z.packetsIn() == 0, "stranger's audio ignored");
}

// ---- audio helpers ---------------------------------------------------------------------------------------------------------------------------
static void testAudio() {
    for (double f : {300.0, 1000.0, 3000.0, 5000.0, 9000.0}) {
        Down6 d;
        std::vector<float> in(48000);
        for (size_t i = 0; i < in.size(); i++) in[i] = 0.5f * (float)std::sin(2 * M_PI * f * (double)i / 48000);
        std::vector<int16_t> o;
        d.process(in.data(), (int)in.size(), o);
        const double a = 2 * std::sqrt(tonePower(o.data() + 1000, 4000, f > 4000 ? 8000 - f : f, 8000)) / 32767;
        printf("down 48k->8k %5.0f Hz: gain %.4f\n", f, a / 0.5);
        if (f <= 3000) CHECK(std::fabs(a / 0.5 - 1) < 0.06, "pass band %.0f Hz gain %.3f", f, a / 0.5);
        else CHECK(a / 0.5 < 0.003, "stop band %.0f Hz leaks %.4f", f, a / 0.5);
    }
    Up6 u;
    std::vector<int16_t> t8(8000);
    for (size_t i = 0; i < t8.size(); i++) t8[i] = (int16_t)(16000 * std::sin(2 * M_PI * 1000 * (double)i / 8000));
    std::vector<float> o48;
    u.process(t8.data(), (int)t8.size(), o48);
    CHECK(o48.size() == 48000, "x6");
    std::vector<int16_t> o16(o48.size());
    for (size_t i = 0; i < o48.size(); i++) o16[i] = (int16_t)std::lround(o48[i] * 32767);
    const double a1 = 2 * std::sqrt(tonePower(o16.data() + 6000, 24000, 1000, 48000)), im = 2 * std::sqrt(tonePower(o16.data() + 6000, 24000, 7000, 48000));
    CHECK(std::fabs(a1 / 16000 - 1) < 0.06 && im < a1 * 0.003, "up: 1 kHz %.0f, image %.1f", a1, im);

    Vox v;
    v.configure({-30, 400});
    std::vector<int16_t> quiet(8000, 50), loud(1600);
    for (size_t i = 0; i < loud.size(); i++) loud[i] = (int16_t)(3000 * std::sin(0.7 * (double)i));
    CHECK(!v.process(quiet.data(), (int)quiet.size()), "quiet stays closed");
    CHECK(v.process(loud.data(), 160), "opens on the first loud 20 ms");
    CHECK(v.process(quiet.data(), 160 * 19), "held");
    CHECK(!v.process(quiet.data(), 160 * 2), "closes after 400 ms");

    RfBridge b;
    BridgeOut o = b.step(20, true, true);
    CHECK(!o.keyRadio && !o.forwardRfToEchoLink, "off by default");
    b.configure({true, false, 500, 180});
    o = b.step(20, true, true);
    CHECK(!o.keyRadio && !o.forwardRfToEchoLink, "enabled but the licence note not acknowledged: nothing");
    b.configure({true, true, 500, 10});
    o = b.step(20, false, true);
    CHECK(o.keyRadio && !o.forwardRfToEchoLink, "EchoLink audio keys the radio");
    o = b.step(20, true, true);
    CHECK(o.keyRadio && !o.forwardRfToEchoLink, "half duplex: the radio's own receive is not sent back");
    int tail = 0;
    while ((o = b.step(20, false, false)).keyRadio && tail < 2000) tail += 20;
    CHECK(tail >= 460 && tail <= 520, "tail %d ms", tail);
    o = b.step(20, true, false);
    CHECK(o.forwardRfToEchoLink && !o.keyRadio, "the radio's receive goes to EchoLink");
    o = b.step(20, true, true);
    CHECK(o.forwardRfToEchoLink && !o.keyRadio, "the side that started keeps the channel");
    o = b.step(20, false, false);
    int ms = 0;
    bool timedOut = false;
    for (; ms < 20000; ms += 20) { o = b.step(20, false, true); if (o.timedOut) { timedOut = true; break; } }
    CHECK(timedOut && ms >= 9900 && ms <= 10100, "transmit time-out at %d ms", ms);
    o = b.step(20, false, true);
    CHECK(!o.keyRadio && o.timedOut, "locked out while EchoLink keeps talking");
    o = b.step(20, false, false);
    o = b.step(20, false, true);
    CHECK(o.keyRadio, "keys again after a pause");
    b.configure({false, true, 500, 10});
    CHECK(!b.step(20, false, true).keyRadio, "turned off: nothing");
}

int main() {
    testGsmVectors();
    testGsmPacking();
    testGsmQuality();
    testRtcp();
    testRtp();
    testDirectoryFormats();
    testSessionTimers();
    testAudio();
    testDirectoryClient();
    testLoopbackQso();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("echolink: all passed\n");
    return 0;
}
