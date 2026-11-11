// Hamlib rig control against Hamlib's own dummy rig (model 1), in process and through rigctld (model 2, netrigctl): open and close, both
// directions of frequency and mode, polling, a rig that disappears and comes back, repeated and concurrent start/stop, mode names, and a
// check that receiving never keys the transmitter. rigctld missing: the network tests are skipped with a note.
#include "dect2/audio/audioin.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/hamlib_rig.h"
#include "dect2/common/ring.h"
#include "dect2/source/source.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace dect2;
using Clock = std::chrono::steady_clock;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, m); fails++; } } while (0)

static double secs(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class F> static bool waitFor(F f, double timeoutS) {
    const auto t0 = Clock::now();
    while (secs(t0) < timeoutS) { if (f()) return true; sleepMs(40); }
    return f();
}

static void testPure() {
    int m = 0; std::string p;
    CHECK(parseHamlibSerial(hamlibRigSerial(1025, "/dev/ttyUSB0"), m, p) && m == 1025 && p == "/dev/ttyUSB0", "serial parse");
    HamlibRig r; r.model = 3079; r.path = "/dev/ttyUSB0"; r.label = "shack"; r.audioId = "mic-id"; r.baud = 19200;
    const auto back = hamlibRigsFromString(hamlibRigsToString({r}));
    CHECK(back.size() == 1 && back[0] == r, "hamlib rig round-trip");
    const auto devs = hamlibRigDevices({r});
    CHECK(devs.size() == 1 && devs[0].kind == DeviceInfo::Hamlib && devs[0].serial == hamlibRigSerial(3079, "/dev/ttyUSB0", 19200), "device entry");
    CHECK(hamlibRigsFromString("0,/dev/x;5,;,/dev/y").empty(), "bad entries dropped");
}

#ifdef DECT2_HAVE_HAMLIB

static void testModes() {
    const char* good[] = {"USB", "LSB", "AM", "FM", "WFM", "CW", "CWR", "RTTY", "PKTUSB", "PKTLSB", "PKTFM", "usb", "Wfm"};
    for (const char* g : good) CHECK(hamlibModeKnown(g), g);
    CHECK(!hamlibModeKnown("") && !hamlibModeKnown("NOPE") && !hamlibModeKnown("US"), "unknown modes rejected");
    CHECK(hamlibCanonicalMode("wfm") == "WFM" && hamlibCanonicalMode("pktusb") == "PKTUSB" && hamlibCanonicalMode("zzz").empty(), "canonical names");
}

// in-process dummy rig: every call and every misuse
static void testDummy() {
    HamlibLink l;
    std::string e;
    double hz = 0;
    CHECK(!l.isOpen(), "closed at birth");
    CHECK(!l.setFreq(1e6, e) && !e.empty(), "set freq on closed link fails with a message");
    CHECK(!l.getFreq(hz, e), "get freq on closed link fails");
    l.close(); l.close();   // closing a never-opened link twice is harmless
    HamlibConn bad; bad.model = 99999999;
    e.clear();
    CHECK(!l.open(bad, e) && !e.empty(), "unknown model fails with a message");

    HamlibConn c; c.model = 1; c.path = "dummy";
    for (int round = 0; round < 3; round++) {   // open, use, close, open again
        e.clear();
        CHECK(l.open(c, e), "open dummy");
        CHECK(l.isOpen(), "isOpen after open");
        CHECK(l.open(c, e) && l.isOpen(), "a second open while open re-opens cleanly");
        // frequency units are hertz, from 100 kHz to the microwave range, including a fractional kHz
        const double fs[] = {100e3, 9.5e6, 14.074e6, 87.5e6, 145.8e6, 522e6, 1.2967e9, 5.8e9, 7.0741234e6};
        for (double f : fs) {
            CHECK(l.setFreq(f, e), "set freq");
            double g = 0;
            CHECK(l.getFreq(g, e) && std::fabs(g - f) < 1.0, "freq reads back in hertz");
        }
        // every mode OnAir offers
        const char* ms[] = {"USB", "LSB", "AM", "FM", "WFM", "CW", "CWR", "RTTY", "PKTUSB", "PKTLSB", "PKTFM"};
        for (const char* m : ms) {
            std::string back; int pb = 0;
            CHECK(l.setMode(m, 0, e), m);
            const bool okm = l.getMode(back, pb, e);
            CHECK(okm && hamlibSameMode(back, m), "mode reads back (some under an alias, e.g. PKTFM as FM-D)");
            if (!(okm && hamlibSameMode(back, m)))printf("  %s -> '%s' pb %d (%s)\n", m, back.c_str(), pb, e.c_str());
            CHECK(pb >= 0, "passband not negative");
        }
        std::string back; int pb = 0;
        CHECK(l.setMode("USB", 2400, e) && l.getMode(back, pb, e) && pb == 2400, "explicit passband");
        CHECK(!l.setMode("NOPE", 0, e) && e.find("NOPE") != std::string::npos, "unknown mode refused by name");
        CHECK(!l.info().empty() || true, "info does not crash");
        l.close();
        CHECK(!l.isOpen() && !l.setFreq(1e6, e), "closed again");
    }
    // other models' open without a rig: a serial model on a port that does not exist fails cleanly, quickly
    HamlibConn r8; r8.model = 3079; r8.path = "/dev/cu.no-such-port"; r8.baud = 19200;
    const auto t0 = Clock::now();
    CHECK(!l.open(r8, e) && !e.empty() && secs(t0) < 5, "missing serial port: error, no hang");
    HamlibConn net; net.model = 2; net.path = "127.0.0.1:1";   // nothing listens
    const auto t1 = Clock::now();
    CHECK(!l.open(net, e) && !e.empty() && secs(t1) < 8, "refused network rig: error, no hang");
    CHECK(!l.isOpen(), "still closed");

    // concurrent use plus a close from another thread: no crash, no deadlock
    HamlibLink sh;
    CHECK(sh.open(c, e), "reopen for stress");
    std::atomic<bool> go{true};
    std::thread a([&] { std::string ee; double v; while (go) { sh.setFreq(7e6, ee); sh.getFreq(v, ee); } });
    std::thread b([&] { std::string ee; std::string m; int p; while (go) { sh.setMode("USB", 0, ee); sh.getMode(m, p, ee); } });
    sleepMs(150);
    sh.close();
    sleepMs(50);
    go = false;
    a.join(); b.join();
    CHECK(!sh.isOpen(), "closed under load");
}

static DeviceInfo rigDev(int model, const std::string& path, int baud = 0) {
    HamlibRig r; r.model = model; r.path = path; r.baud = baud;
    return hamlibRigDevices({r})[0];
}

static TuneSettings tune(double hz, const char* mode) {
    TuneSettings t;
    t.centerHz = hz;
    t.radio["hamlib_mode"] = mode;
    return t;
}

// source on the in-process dummy: start, retune, stop, many times; stop while another thread retunes
static void testSourceDummy() {
    IqRing ring(1u << 16);
    const DeviceInfo d = rigDev(1, "dummy");
    for (int i = 0; i < 12; i++) {
        auto s = makeHamlibSource(d);
        std::string e;
        CHECK(s->start(tune(7.074e6 + i * 1000, "USB"), ring, e), "dummy source starts"); if (!e.empty()) printf("  err: %s\n", e.c_str());
        CHECK(hamlibRigState().active && hamlibRigState().connected, "state active and connected");
        CHECK(s->retune(tune(14.2e6, "LSB"), e), "retune");
        CHECK(std::fabs(hamlibRigState().hz - 14.2e6) < 1.0 && hamlibRigState().mode == "LSB", "state follows the app");
        s->stop();
        CHECK(!hamlibRigState().active, "state inactive after stop");
        s->stop();   // twice
    }
    // second open after close, same object
    {
        auto s = makeHamlibSource(d);
        std::string e;
        CHECK(s->start(tune(10e6, "AM"), ring, e), "start 1");
        s->stop();
        CHECK(s->start(tune(11e6, "FM"), ring, e), "start 2 after stop");
        s->stop();
    }
    // stop racing with retune
    for (int i = 0; i < 6; i++) {
        auto s = makeHamlibSource(d);
        std::string e;
        CHECK(s->start(tune(7e6, "USB"), ring, e), "start for race");
        std::atomic<bool> go{true};
        std::thread t([&] { std::string ee; int k = 0; while (go) s->retune(tune(7e6 + (k++ % 50) * 1000, k % 2 ? "USB" : "LSB"), ee); });
        sleepMs(120);
        go = false;
        t.join();
        const auto t0 = Clock::now();
        s->stop();
        CHECK(secs(t0) < 3, "stop is quick");
        s.reset();
    }
    // a bad id and a serial rig that is not there
    {
        auto s = makeHamlibSource(rigDev(3079, "/dev/cu.no-such-port"));
        std::string e;
        CHECK(!s->start(tune(1e6, "AM"), ring, e) && !e.empty(), "missing port: start fails with a message");
        DeviceInfo bad = d; bad.serial = "garbage";
        auto s2 = makeHamlibSource(bad);
        CHECK(!s2->start(tune(1e6, "AM"), ring, e), "bad id fails");
    }
}

#if !defined(_WIN32)

struct Rigctld {
    pid_t pid = -1;
    int port = 0;
    std::string log;
    bool start(int p, const std::string& logPath) {
        port = p; log = logPath;
        const char* bin = nullptr;
        for (const char* c : {"/opt/homebrew/bin/rigctld", "/usr/local/bin/rigctld", "/usr/bin/rigctld"}) if (access(c, X_OK) == 0) { bin = c; break; }
        if (!bin) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        const std::string ps = std::to_string(p);
        char* argv[] = {(char*)bin, (char*)"-m", (char*)"1", (char*)"-t", (char*)ps.c_str(), (char*)"-T", (char*)"127.0.0.1", (char*)"-vvvv", nullptr};
        const int r = posix_spawn(&pid, bin, &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        if (r != 0) { pid = -1; return false; }
        // wait for the port to accept
        for (int i = 0; i < 100; i++) {
            const int s = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)p); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            const bool ok = connect(s, (sockaddr*)&a, sizeof a) == 0;
            ::close(s);
            if (ok) return true;
            sleepMs(50);
        }
        return false;
    }
    void kill9() { if (pid > 0) { kill(pid, SIGKILL); int st; waitpid(pid, &st, 0); pid = -1; } }
    ~Rigctld() { kill9(); }
};

// ask rigctld a raw question ("t" = PTT state), as another program would
static std::string rawAsk(int port, const char* cmd) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { ::close(s); return "?"; }
    timeval tv{2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string c = std::string(cmd) + "\n";
    (void)!write(s, c.data(), c.size());
    char b[256]; const ssize_t n = read(s, b, sizeof b - 1);
    ::close(s);
    return n > 0 ? std::string(b, (size_t)n) : "?";
}

static std::string slurp(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

static void testNetwork(const std::string& tmp) {
    const int port = 46000 + (int)(getpid() % 2000);
    const std::string addr = "127.0.0.1:" + std::to_string(port);
    Rigctld rc;
    if (!rc.start(port, tmp + "/rigctld.log")) { printf("note: rigctld not available, network tests skipped\n"); return; }
    std::string e;

    // the link itself, over the network
    HamlibLink l;
    HamlibConn c; c.model = 2; c.path = addr;
    CHECK(l.open(c, e), "open netrigctl");
    double hz = 0;
    CHECK(l.setFreq(145.5e6, e) && l.getFreq(hz, e) && std::fabs(hz - 145.5e6) < 1, "net: freq round trip");
    CHECK(rawAsk(port, "f").find("145500000") == 0, "rigctld itself reports what OnAir set (hertz)");
    std::string m; int pb;
    CHECK(l.setMode("WFM", 0, e) && l.getMode(m, pb, e) && m == "WFM", "net: mode round trip");
    CHECK(rawAsk(port, "m").find("WFM") == 0, "rigctld itself reports the mode");
    l.close();
    CHECK(l.open(c, e) && l.isOpen(), "net: reopen after close");
    l.close();

    // the source: rig -> app and app -> rig
    IqRing ring(1u << 16);
    HamlibRig r; r.model = 2; r.path = addr;
    const DeviceInfo d = hamlibRigDevices({r})[0];
    auto s = makeHamlibSource(d);
    CHECK(s->start(tune(7.1e6, "LSB"), ring, e), "net source starts");
    CHECK(rawAsk(port, "f").find("7100000") == 0 && rawAsk(port, "m").find("LSB") == 0, "app -> rig at start");
    HamlibLink knob;   // somebody turns the dial on the rig
    for (int tries = 0; tries < 3 && !knob.open(c, e); tries++) sleepMs(300);   // rigctld occasionally refuses one quick connection
    CHECK(knob.isOpen(), "second client");
    if (!knob.isOpen()) printf("  second client: %s\n", e.c_str());
    const uint64_t seq0 = hamlibRigState().seq;
    CHECK(knob.setFreq(7.2e6, e), "dial turned on the rig");
    CHECK(waitFor([&] { return std::fabs(hamlibRigState().hz - 7.2e6) < 1 && hamlibRigState().seq > seq0; }, 3), "rig -> app: dial change shows in the state");
    const uint64_t seq1 = hamlibRigState().seq;
    CHECK(knob.setMode("AM", 0, e), "mode turned on the rig");
    CHECK(waitFor([&] { return hamlibRigState().mode == "AM" && hamlibRigState().seq > seq1; }, 3), "rig -> app: mode change shows in the state");
    // the app's own retune is not reported back as a rig change
    const uint64_t seq2 = hamlibRigState().seq;
    CHECK(s->retune(tune(7.3e6, "AM"), e), "app retune");
    sleepMs(1200);   // several polls
    CHECK(hamlibRigState().seq == seq2, "an app retune does not echo as a rig change");
    CHECK(std::fabs(hamlibRigState().hz - 7.3e6) < 1, "state holds the app's frequency");
    CHECK(rawAsk(port, "f").find("7300000") == 0, "app -> rig: retune reached the rig");
    // the app sets a mode: the rig changes too
    CHECK(s->retune(tune(7.3e6, "USB"), e) && rawAsk(port, "m").find("USB") == 0, "app -> rig: mode");

    // the rig disappears
    rc.kill9();
    CHECK(waitFor([&] { return !hamlibRigState().connected && !hamlibRigState().status.empty(); }, 6), "rigctld killed: state shows the link down with a message");
    const auto t0 = Clock::now();
    std::string re;
    s->retune(tune(7.4e6, "USB"), re);   // the answer does not matter, only that it returns and does not crash
    CHECK(secs(t0) < 5, "retune on a dead rig returns quickly");
    sleepMs(1500);
    // it comes back
    CHECK(rc.start(port, tmp + "/rigctld2.log"), "rigctld restarted");
    CHECK(waitFor([&] { return hamlibRigState().connected; }, 15), "reconnect works without restarting the receiver");
    CHECK(waitFor([&] { return rawAsk(port, "f").find("7400000") == 0; }, 3), "a retune made while the link was down is applied after reconnect");
    const auto t1 = Clock::now();
    s->stop();
    CHECK(secs(t1) < 3, "stop after all that is quick");
    CHECK(!hamlibRigState().active, "inactive after stop");

    // killed rigctld at start: clear message
    rc.kill9();
    auto s2 = makeHamlibSource(d);
    const auto t2 = Clock::now();
    CHECK(!s2->start(tune(7e6, "USB"), ring, e) && !e.empty() && secs(t2) < 8, "start against a dead rigctld: message, no hang");
    // stop while the link is down and the poll loop is in its reconnect loop
    CHECK(rc.start(port, tmp + "/rigctld3.log"), "restart again");
    auto s3 = makeHamlibSource(d);
    CHECK(s3->start(tune(7e6, "USB"), ring, e), "start 3");
    rc.kill9();
    sleepMs(1500);
    const auto t3 = Clock::now();
    s3->stop();
    CHECK(secs(t3) < 3, "stop while reconnecting is quick");

    // PTT: nothing above (open, tune, poll, kill, reconnect, stop) may have keyed the rig
    const std::string all = slurp(tmp + "/rigctld.log") + slurp(tmp + "/rigctld2.log") + slurp(tmp + "/rigctld3.log");
    CHECK(all.size() > 100, "rigctld wrote a protocol log");
    CHECK(all.find("set_ptt") == std::string::npos && all.find("rig_set_ptt") == std::string::npos && all.find("PTT_ON") == std::string::npos,
          "receiving never sent a PTT command (rigctld log)");
    CHECK(all.find("set_freq") != std::string::npos, "the log does show the frequency commands (the check above is meaningful)");
}

static void testTxIsSeparate(const std::string& tmp) {
    // the transmit sink is the only code that keys: it does so on open() (an operator action in the TX window), and unkeys on close()
    const int port = 48000 + (int)(getpid() % 1500);
    Rigctld rc;
    if (!rc.start(port, tmp + "/rigctld_tx.log")) return;
    HamlibRig r; r.model = 2; r.path = "127.0.0.1:" + std::to_string(port);
    const DeviceInfo d = hamlibRigDevices({r})[0];
    CHECK(d.canTransmit, "hamlib entry advertises transmit (the TX window decides)");
    // the RX source never creates a TX sink, and makeTxSink needs canTransmit
    auto tx = makeTxSink(d);
    CHECK(tx != nullptr, "tx sink exists for the TX window");
    TxTune t; t.centerHz = 145.5e6; t.sampleRate = 48000;
    std::string e;
    CHECK(rawAsk(port, "t").find("0") == 0, "not keyed before");
    CHECK(tx->open(t, e), "tx open");
    CHECK(rawAsk(port, "t").find("1") == 0, "keyed only after TxSink::open");
    tx->close();
    CHECK(rawAsk(port, "t").find("0") == 0, "unkeyed after close");
    tx->close();   // twice
    // a TX open that fails before keying never sends PTT off either way
    rc.kill9();
    auto tx2 = makeTxSink(d);
    CHECK(!tx2->open(t, e), "tx open on dead rig fails");
}

#endif // !_WIN32

#endif // DECT2_HAVE_HAMLIB

// #358: the rig's sound card typed as its name ("USB Audio CODEC", "Sound card: USB Audio CODEC", "Default input") finds the device id
static void testSoundCardName() {
    const std::vector<AudioInputDevice> devs = {{"", "Default input"}, {"ca:AppleUSBAudioEngine:Burr-Brown:USB Audio CODEC:1", "USB Audio CODEC"}, {"ca:BlackHole2ch_UID", "BlackHole 2ch"}};
    std::string id = "x";
    CHECK(resolveAudioInputId("USB Audio CODEC", devs, id) && id == devs[1].id, "name");
    id = "x"; CHECK(resolveAudioInputId("  sound card: usb audio codec ", devs, id) && id == devs[1].id, "source-list name");
    id = "x"; CHECK(resolveAudioInputId("Default input", devs, id) && id.empty(), "default input");
    id = "x"; CHECK(resolveAudioInputId("default", devs, id) && id.empty(), "default");
    id = "x"; CHECK(resolveAudioInputId("ca:BlackHole2ch_UID", devs, id) && id == "ca:BlackHole2ch_UID", "an id stays");
    id = "keep"; CHECK(!resolveAudioInputId("Scarlett 2i2", devs, id) && id == "keep", "an unknown card is not replaced");
}

int main() {
    testPure();
    testSoundCardName();
#ifdef DECT2_HAVE_HAMLIB
    testModes();
    testDummy();
    testSourceDummy();
#if !defined(_WIN32)
    const char* td = getenv("TMPDIR");
    std::string tmp = td && *td ? td : "/tmp";
    while (tmp.size() > 1 && tmp.back() == '/') tmp.pop_back();
    testNetwork(tmp);
    testTxIsSeparate(tmp);
#endif
#else
    printf("note: built without Hamlib, rig tests skipped\n");
#endif
    if (!fails) printf("hamlib rig: all ok\n");
    return fails ? 1 : 0;
}
