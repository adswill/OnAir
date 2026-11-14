// Part of test_videogen (ctest -R "^videogen$"): the link between the Digital video generator program and OnAir (vglink.h, #458).
//  1. packets: RTP (payload type 33, 7 TS packets) and compound RTCP (RR + SDES + APP, BYE) both ways, foreign and broken ones
//     refused; key=value text both ways
//  2. the transmitter's info: usable rate per generator, shared by the generators in the multiplex
//  3. server and clients on a free local port: slots (asked, taken, the next free one), Status, Start / Rate / Stop into the hub's rate,
//     the stream into the hub's queue, a Start empties the queue, TxInfo to every generator, Raise / Quit by token, Bye, a generator
//     leaving (its rate goes to 0), the ninth generator refused, OnAir away and back on the same ports, lost datagrams counted
#include "dect2/tv/videogen.h"
#include "dect2/tv/vglink.h"
#include "vg_test_pipe.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dect2::vgen;

namespace {
int gFail = 0;
void fail(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::printf("FAIL: %s\n", b);
    gFail++;
}
#define LCHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

bool waitFor(const std::function<bool()>& f, double seconds = 5) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

void testFraming() {
    // RTP: 7 TS packets, payload type 33, sequence and SSRC back
    std::vector<uint8_t> ts(188 * 9, 0xFF);
    for (size_t i = 0; i < 9; i++) ts[i * 188] = 0x47;
    const std::vector<uint8_t> r = vgRtp(ts.data(), 9, 0xFFFF, 900, 0x12345678);
    LCHECK(r.size() == 12 + 7 * 188 && r[0] == 0x80 && r[1] == 33, "RTP: V=2, PT 33, at most 7 TS packets (%zu bytes)", r.size());
    VgPacket m;
    LCHECK(vgParseRtp(r.data(), r.size(), m) && m.seq == 0xFFFF && m.ssrc == 0x12345678 && m.data.size() == 7 * 188, "RTP back");
    std::vector<uint8_t> pt96 = r;
    pt96[1] = 96;
    LCHECK(!vgParseRtp(pt96.data(), pt96.size(), m), "another payload type is not ours");
    LCHECK(!vgParseRtp(r.data(), 8, m), "a short datagram is refused");
    // RTCP: RR + SDES + APP, its text back; a BYE with its reason
    const std::vector<uint8_t> c = vgRtcp(VgMsg::State, "rate=1000\n", 0xABCD, "onair-videogen", "Club");
    LCHECK(c.size() % 4 == 0 && c[1] == 201, "RTCP: compound, starting with RR, in 32-bit words");
    std::vector<VgPacket> got;
    LCHECK(vgParseRtcp(c.data(), c.size(), got) && got.size() == 1 && got[0].type == VgMsg::State && got[0].ssrc == 0xABCD &&
           std::string(got[0].data.begin(), got[0].data.end()) == "rate=1000\n", "RTCP APP back");
    got.clear();
    const std::vector<uint8_t> by = vgRtcp(VgMsg::Bye, "closed", 7, "x");
    LCHECK(vgParseRtcp(by.data(), by.size(), got) && got.size() == 1 && got[0].type == VgMsg::Bye && std::string(got[0].data.begin(), got[0].data.end()) == "closed", "BYE with its reason");
    got.clear();
    LCHECK(!vgParseRtcp(c.data(), c.size() - 4, got) && got.empty(), "a cut RTCP packet is refused whole");
    std::vector<uint8_t> v1 = c;
    v1[0] = 0x41;
    LCHECK(!vgParseRtcp(v1.data(), v1.size(), got), "not version 2 is refused");
    const VgKv kv = vgKvParse(vgKvText({{"name", "Club\nTV"}, {"slot", "3"}, {"x", ""}}));
    LCHECK(vgKvStr(kv, "name") == "Club TV" && vgKvNum(kv, "slot") == 3 && vgKvNum(kv, "missing", 7) == 7 && vgKvNum(kv, "x", 5) == 5, "key=value text");
    LCHECK(vgKvParse("junk\n=nokey\nok=1").size() == 1, "lines without a key are skipped");
}

void testTxInfo() {
    VgTxInfo t;
    t.channelBps = 10e6;
    LCHECK(vgUsableBps(t, 1) > 9.6e6 && vgUsableBps(t, 1) < 9.8e6, "one generator: 97 %% of the channel");
    t.slots = {1, 2};
    LCHECK(std::abs(vgUsableBps(t, 2) - 4.85e6) < 1, "two in the multiplex share it");
    LCHECK(vgUsableBps(t, 5) > 9.6e6, "a generator not in the multiplex is told the whole channel");
    t.running = true; t.digitalTv = true; t.system = "DVB-T"; t.params = "8 MHz, 64-QAM";
    double u = 0;
    const VgTxInfo b = vgTxInfoFromKv(vgKvParse(vgKvText(vgTxInfoKv(t, 1))), u);
    LCHECK(b.running && b.digitalTv && b.system == "DVB-T" && b.params == "8 MHz, 64-QAM" && b.channelBps == 10e6 && b.slots.size() == 2 && std::abs(u - 4.85e6) < 1,
           "TxInfo through key=value (usable %.0f)", u);
    VgTxInfo none;
    LCHECK(vgUsableBps(none, 1) == 0, "no channel: no usable rate");
}

// local: over the pipe (Windows) / Unix socket; else over UDP
void testServerClient(bool local) {
    printf("  link over %s\n", local ? "the local pipe" : "UDP");
    VgLinkServer srv;
    std::string err;
    const std::string pipe = vgTestPipeName();
    LCHECK(srv.start(!local, 0, false, err, pipe), "the server listens (%s)", err.c_str());
    const int port = srv.port();
    LCHECK(srv.listeningLocal() && srv.localName() == pipe, "it always has the local pipe (%s)", srv.localName().c_str());
    LCHECK(local ? port == 0 : port > 0, "a UDP port only when asked (%d)", port);
    auto open = [&](VgLinkClient& c, const std::string& name, int slot, const std::string& tok) {
        if (local) c.startLocal(pipe, name, slot, tok);
        else c.start("127.0.0.1", port, name, slot, tok);
    };
    if (local) {   // one OnAir per pipe
        VgLinkServer other;
        std::string e2;
        LCHECK(!other.start(false, 0, false, e2, pipe) && e2.find("another OnAir") != std::string::npos, "a second OnAir cannot take the pipe (%s)", e2.c_str());
    }
    std::mutex byeMu;   // onBye runs on the link's thread
    std::string byeToken;
    srv.onBye = [&](const std::string& t) { std::lock_guard<std::mutex> lk(byeMu); byeToken = t; };

    VgLinkClient a, b;
    open(a, "Club camera", 1, "tok-a");
    LCHECK(waitFor([&] { return a.connected() && a.slot() == 1; }), "the first generator gets slot 1 (%s)", a.status().c_str());
    open(b, "Second", 1, "");
    LCHECK(waitFor([&] { return b.connected() && b.slot() == 2; }), "slot 1 is taken: the next free one (%d)", b.slot());
    a.setStatus(false, {"300 H.264", "301 H.265"});
    LCHECK(waitFor([&] { auto p = srv.peers(); return p.size() == 2 && !p[0].easy && p[0].programmes.size() == 2; }), "Status reaches OnAir");
    {
        auto p = srv.peers();
        LCHECK(p.size() == 2 && p[0].name == "Club camera" && p[0].host == (local ? "this computer" : "127.0.0.1") && p[0].local == local && p[0].token == "tok-a" && p[1].slot == 2, "peers: name, host, token, slot");
    }
    // the stream
    std::vector<uint8_t> pk(188 * 50, 0xFF);
    for (size_t i = 0; i < 50; i++) { pk[i * 188] = 0x47; pk[i * 188 + 1] = 0x01; pk[i * 188 + 2] = 0x00; pk[i * 188 + 3] = 0x10; }
    a.startStream(2e6);
    LCHECK(waitFor([] { return generatorRate(1) == 2e6; }), "Start sets the hub's rate (%.0f)", generatorRate(1));
    LCHECK(a.sendTs(pk.data(), 50), "packets sent");
    LCHECK(waitFor([] { return generatorQueued(1) == 50; }), "the packets wait in slot 1's queue (%zu)", generatorQueued(1));
    a.setRate(3e6);
    LCHECK(waitFor([] { return generatorRate(1) == 3e6; }), "Rate changes it");
    a.startStream(3e6);   // a new start empties the queue (#494)
    LCHECK(waitFor([] { return generatorQueued(1) == 0; }), "a Start empties the queue");
    LCHECK(waitFor([&] { auto p = srv.peers(); return !p.empty() && p[0].rateBps == 3e6; }), "the peer shows it sending");
    a.stopStream();
    LCHECK(waitFor([] { return generatorRate(1) == 0; }), "Stop: rate 0");
    // the transmitter's info
    VgTxInfo t;
    t.running = true; t.digitalTv = true; t.channelBps = 8e6; t.system = "DVB-T2"; t.slots = {1, 2};
    srv.setTxInfo(t);
    LCHECK(waitFor([&] { double u = 0; VgTxInfo x = a.txInfo(u); return x.running && x.system == "DVB-T2" && std::abs(u - 3.88e6) < 1; }), "TxInfo reaches the generators");
    LCHECK(waitFor([&] { double u = 0; b.txInfo(u); return std::abs(u - 3.88e6) < 1; }), "each one gets its share");
    // Raise / Quit / Bye
    LCHECK(srv.raise("tok-a") && waitFor([&] { return a.takeRaise(); }) && !b.takeRaise(), "Raise goes to the generator with that token only");
    LCHECK(!srv.raise("") && !srv.raise("other"), "no generator with an empty or unknown token");
    LCHECK(srv.quit("tok-a") && waitFor([&] { return a.takeQuit(); }), "Quit by token");
    a.bye();
    LCHECK(waitFor([&] { std::lock_guard<std::mutex> lk(byeMu); return byeToken == "tok-a"; }), "Bye tells OnAir which one was closed");
    // a generator leaving: its rate goes, its slot is free again
    b.startStream(1e6);
    LCHECK(waitFor([] { return generatorRate(2) == 1e6; }), "b sends");
    b.stop();
    LCHECK(waitFor([&] { return srv.peers().size() == 1 && generatorRate(2) == 0; }), "b left: rate 0, one peer");
    // eight at most
    std::vector<std::unique_ptr<VgLinkClient>> many;
    for (int i = 0; i < kMaxInstances; i++) { many.push_back(std::make_unique<VgLinkClient>()); open(*many.back(), "G" + std::to_string(i), 1, ""); }
    LCHECK(waitFor([&] { return srv.peers().size() == (size_t)kMaxInstances; }), "eight generators connected (%zu)", srv.peers().size());
    LCHECK(waitFor([&] { int refused = 0; for (auto& m : many) if (m->status().find("refused") != std::string::npos) refused++; return refused == 1; }), "the ninth is refused");
    many.clear();
    // lost datagrams are counted from the sequence numbers (a gap made by hand)
    {
        VgLinkClient c;
        open(c, "Gap", 4, "");
        LCHECK(waitFor([&] { return c.connected(); }), "a fourth generator links");
        c.startStream(1e6);
        LCHECK(waitFor([&] { return generatorRate(c.slot()) == 1e6; }), "it sends");
        LCHECK(c.sendTs(pk.data(), 14), "two datagrams");
        LCHECK(waitFor([&] { for (auto& p : srv.peers()) if (p.name == "Gap") return p.packets == 14; return false; }), "14 packets arrive");
        for (auto& p : srv.peers()) if (p.name == "Gap") LCHECK(p.lost == 0 && p.late == 0, "none lost (%llu)", (unsigned long long)p.lost);
        c.stop();
    }
    // the server goes away and comes back on the same port: the generator reconnects
    a.startStream(1.5e6);
    srv.stop();
    LCHECK(waitFor([&] { return !a.connected(); }), "the generator sees OnAir gone");
    LCHECK(generatorRate(1) == 0, "OnAir's slots are empty after stop");
    LCHECK(srv.start(!local, port, false, err, pipe), "the server listens again (%s)", err.c_str());
    LCHECK(waitFor([&] { return a.connected(); }, 6), "the generator reconnects");
    LCHECK(waitFor([] { return generatorRate(1) == 1.5e6; }), "and starts its stream again (rate %.0f)", generatorRate(1));
    a.stop();
    srv.stop();
}

} // namespace

int videogenLinkTests() {
    testFraming();
    testTxInfo();
    testServerClient(true);
    testServerClient(false);
    return gFail;
}
