// Rig control for transmitting: the watched CAT link (re-open with back-off, never while keyed), tuning for TX with read-back and split, PTT by
// CAT / VOX and a failed release, CatRigIo through the ham gate (licence x Arm x band), a rig that drops mid-burst, finding the rig, audio pairing,
// error hints and the rig profile in the settings. Fake CAT and fake sound card only; the Hamlib part uses the dummy rig (no radio).
#include "dect2/ham/ham.h"
#include "dect2/source/rig_control.h"
#include "dect2/source/source.h"
#include <cmath>
#include "../common/test_util.h"
using namespace dect2;

namespace {
struct FakeCat : RigCat {
    bool answers = true, openOk = true, pttFail = false, releaseFail = false, splitOk = true;
    double hz = 14.074e6, splitHz = 0, offsetHz = 0;   // offsetHz: the rig lands this far off what it is told
    int opens = 0, closes = 0, pttOn = 0, pttOff = 0, reads = 0;
    std::string mode;
    HamlibConn last;
    bool open(const HamlibConn& c, std::string& err) override { opens++; last = c; if (!openOk) { err = "Hamlib open /dev/x: IO error"; return false; } return true; }
    void close() override { closes++; }
    bool setFreq(double f, std::string& err) override { if (!answers) { err = "Hamlib set frequency: Communication timed out"; return false; } hz = f + offsetHz; return true; }
    bool getFreq(double& f, std::string& err) override { reads++; if (!answers) { err = "Hamlib read frequency: Communication timed out"; return false; } f = hz; return true; }
    bool setMode(const std::string& m, std::string& err) override { if (m == "PKTUSB" && mode == "nodata") { err = "Feature not available"; return false; } mode = m; return true; }
    bool setPtt(bool on, std::string& err) override {
        if (on) { pttOn++; if (pttFail) { err = "Command rejected by the rig"; return false; } }
        else { pttOff++; if (releaseFail) { err = "Communication timed out"; return false; } }
        return true;
    }
    bool setSplit(bool, double f, std::string& err) override { if (!splitOk) { err = "Feature not available"; return false; } splitHz = f; return true; }
    bool getSplitFreq(double& f, std::string&) override { f = splitHz; return true; }
};
struct FakeAudio : RigAudio {
    long frames = 0; bool started = false;
    bool start(const std::string&, int, std::string&) override { started = true; return true; }
    int write(const float*, int n) override { frames += n; return n; }
    int buffered() override { return 0; }
    void flush() override {}
    void stop() override { started = false; }
};
HamlibRig rig7300() { HamlibRig r; r.model = 3073; r.path = "/dev/cu.usbserial-1420"; r.baud = 115200; return r; }
ham::HamTxCheck gate(double dial = 14.074e6) {
    ham::HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; g.audioHz = 1500; return g;
}
}  // namespace

int main() {
    std::string err;
    // ---- the watched link ----
    {
        auto* c = new FakeCat; RigKeeper k{std::unique_ptr<RigCat>(c)};
        RigKeeper empty{std::make_unique<FakeCat>()};
        CHECK(!empty.connect(err) && err.find("choose the rig") != std::string::npos, "no rig chosen: %s", err.c_str());
        HamlibRig r = rig7300(); r.pttType = 2; r.pttPath = "/dev/cu.ptt";
        k.setProfile(r);
        CHECK(k.connect(err) && k.state() == RigKeeper::Up && k.dialHz() == 14.074e6, "connect: %s", err.c_str());
        CHECK(c->last.model == 3073 && c->last.baud == 115200 && c->last.pttType == 2 && c->last.pttPath == "/dev/cu.ptt", "the connection carries the PTT settings");
        k.tick(0); k.tick(1);
        const int reads = c->reads;
        k.tick(2.5);
        CHECK(c->reads == reads + 1, "pinged every 2 s");
        c->answers = false;
        k.tick(5);
        CHECK(k.state() == RigKeeper::Down && k.status().find("does not answer") != std::string::npos, "rig gone: %s", k.status().c_str());
        c->openOk = false;
        const int o0 = c->opens;
        k.tick(5.5);  CHECK(c->opens == o0, "no re-open before 1 s");
        k.tick(6.1);  CHECK(c->opens == o0 + 1, "re-open after 1 s");
        k.tick(7.5);  CHECK(c->opens == o0 + 1, "then after 2 s more");
        k.tick(8.2);  CHECK(c->opens == o0 + 2, "second re-open");
        c->openOk = true; c->answers = true;
        k.tick(13);   CHECK(k.state() == RigKeeper::Up && k.status().empty(), "back up: %s", k.status().c_str());
        k.disconnect();
        CHECK(k.state() == RigKeeper::Off, "disconnected");
    }
    // ---- tuning for TX ----
    {
        auto* c = new FakeCat; RigKeeper k{std::unique_ptr<RigCat>(c)};
        k.setProfile(rig7300());
        CHECK(!k.tuneForTx(14.074e6, "PKTUSB", err), "not connected: refused");
        k.connect(err);
        CHECK(k.tuneForTx(7.074e6, "PKTUSB", err) && c->hz == 7.074e6 && c->mode == "PKTUSB", "tuned: %s", err.c_str());
        c->offsetHz = 500;
        CHECK(!k.tuneForTx(14.074e6, "PKTUSB", err) && err.find("not at") != std::string::npos, "read-back mismatch refused: %s", err.c_str());
        c->offsetHz = 0; c->mode = "nodata";
        CHECK(k.tuneForTx(14.074e6, "PKTUSB", err) && c->mode == "USB", "no data mode: falls back to USB");
        HamlibRig s = rig7300(); s.split = true;
        k.setProfile(s);
        c->hz = 14.070e6;
        CHECK(k.tuneForTx(14.0745e6, "PKTUSB", err) && c->splitHz == 14.0745e6 && c->hz == 14.070e6, "split: the TX VFO moves, the RX VFO stays");
    }
    // ---- PTT ----
    {
        auto* c = new FakeCat; RigKeeper k{std::unique_ptr<RigCat>(c)};
        HamlibRig v = rig7300(); v.pttType = 3;
        k.setProfile(v); k.connect(err);
        CHECK(k.ptt(true, err) && c->pttOn == 0 && k.keyed(), "VOX: no PTT command");
        k.ptt(false, err);
        k.setProfile(rig7300());
        c->pttFail = true;
        CHECK(!k.ptt(true, err) && c->pttOff == 1 && !k.keyed() && err.find("refused") != std::string::npos, "PTT on failed: released again (%s)", err.c_str());
        c->pttFail = false;
        CHECK(k.ptt(true, err) && k.keyed(), "PTT on");
        const int o = c->opens;
        k.tick(100);
        CHECK(c->reads >= 1 && c->opens == o, "keyed: no re-open under the transmitter");
        c->releaseFail = true;
        CHECK(!k.ptt(false, err) && !k.keyed() && k.state() == RigKeeper::Down && err.find("by hand") != std::string::npos, "release failed: port closed, told (%s)", err.c_str());
    }
    // ---- CatRigIo through the gate ----
    {
        int keyed = 0;
        for (int m = 0; m < 8; m++) {
            auto* c = new FakeCat; RigKeeper k{std::unique_ptr<RigCat>(c)}; k.setProfile(rig7300());
            auto* a = new FakeAudio;
            CatRigIo io(k, std::unique_ptr<RigAudio>(a));
            ham::HamSlotTx tx(io);
            ham::HamTxCheck g = gate(m & 4 ? 14.074e6 : 13.9e6);
            g.licenceConfirmed = m & 1; g.armed = m & 2;
            const bool ok = tx.send(g, 2, "CQ LY1ABC KO24", 1500, 0.5, err);
            const bool arm = (m & 2) != 0;   // licence and band are warnings now: only Arm keys
            CHECK(ok == arm && (c->pttOn > 0) == arm && (a->frames > 0) == arm && (c->opens > 0) == arm,
                  "CAT licence %d arm %d band %d: sent %d keyed %d", m & 1, (m >> 1) & 1, (m >> 2) & 1, ok, c->pttOn);
            if (m == 7) CHECK(c->hz == 14.074e6 && c->mode == "PKTUSB" && c->pttOff == 1, "tuned to the checked frequency, released");
            keyed += c->pttOn;
        }
        CHECK(keyed == 4, "a key-up in the 4 armed combinations of 8");
        auto* c = new FakeCat; RigKeeper k{std::unique_ptr<RigCat>(c)}; k.setProfile(rig7300()); k.connect(err);
        CatRigIo io(k, std::make_unique<FakeAudio>());
        CHECK(!io.ptt(true, err) && c->pttOn == 0, "CatRigIo: no PTT without the gate (%s)", err.c_str());
        c->offsetHz = 1000;
        ham::HamSlotTx tx(io);
        CHECK(!tx.send(gate(), 2, "CQ LY1ABC KO24", 1500, 0.5, err) && c->pttOn == 0, "the rig is not where it was told: nothing keyed (%s)", err.c_str());
    }
    {   // the rig stops answering mid-burst: hard stop, PTT release tried, the link marked down
        auto* c = new FakeCat; RigKeeper k{std::unique_ptr<RigCat>(c)}; k.setProfile(rig7300());
        struct DropAudio : FakeAudio { FakeCat* c = nullptr; int n = 0; int write(const float*, int f) override { if (++n == 5) c->answers = false; frames += f; return f; } };
        auto* a = new DropAudio; a->c = c;
        CatRigIo io(k, std::unique_ptr<RigAudio>(a));
        io.setCheckInterval(0);   // read the rig at every block (the default is every 2 s)
        ham::HamSlotTx tx(io);
        CHECK(!tx.send(gate(), 2, "CQ LY1ABC KO24", 1500, 0.5, err) && err.find("stopped answering") != std::string::npos, "rig gone mid-burst: stopped (%s)", err.c_str());
        CHECK(c->pttOn == 1 && c->pttOff >= 1 && k.state() == RigKeeper::Down && !k.keyed() && a->n <= 6, "PTT off tried, link down, %d blocks played", a->n);
        CHECK(!tx.send(gate(), 2, "CQ LY1ABC KO24", 1500, 0.5, err) && c->pttOn == 1, "the next burst is refused before PTT (%s)", err.c_str());
    }
    // ---- finding the rig ----
    {
        const std::vector<RigModelInfo> all = {
            {3073, "Icom", "IC-7300", 3, false, 1200, 115200}, {3085, "Icom", "IC-705", 3, false, 300, 115200}, {1035, "Yaesu", "FT-991", 3, false, 4800, 38400},
            {2028, "Kenwood", "TS-480", 3, false, 1200, 115200}, {2044, "Elecraft", "KX3", 3, false, 4800, 38400}, {3088, "Xiegu", "G90", 3, false, 4800, 19200},
            {2, "Hamlib", "NET rigctl", 3, true}, {4, "FLRig", "FLRig", 3, true}};
        SerialPortInfo p; p.path = "/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_03001234-if00-port0";
        auto h = rigHintsForPort(p);
        CHECK(!h.empty() && h[0].model == "IC-7300", "IC-7300 by name: %s", h.empty() ? "-" : h[0].model.c_str());
        p.path = "/dev/cu.usbmodem1101"; p.display = "QDX Transceiver";
        h = rigHintsForPort(p);
        bool ts480 = false; for (auto& x : h) ts480 |= x.model == "TS-480";
        CHECK(!h.empty() && h[0].model == "QDX" && ts480, "QDX: its own model, then TS-480");
        p.display = "Silicon Labs Dual CP2105 USB to UART Bridge"; p.path = "COM5";
        h = rigHintsForPort(p);
        CHECK(!h.empty() && h[0].mfg == "Yaesu", "CP2105: Yaesu first");
        p.display = "USB-SERIAL CH340"; h = rigHintsForPort(p);
        CHECK(!h.empty() && h[0].mfg == "Xiegu", "CH340: Xiegu");
        p.display = "Some modem"; p.path = "/dev/cu.Bluetooth"; CHECK(rigHintsForPort(p).empty(), "no hint from an unrelated name");
        p.path = "/dev/serial/by-id/usb-Silicon_Labs_CP2102_IC-7300"; p.display = "";
        const auto plan = rigDetectPlan(all, rigHintsForPort(p), 24);
        CHECK(!plan.empty() && plan[0] == std::make_pair(3073, 115200) && plan.size() <= 24, "plan starts with the IC-7300 at 115200");
        for (const auto& [m, b] : plan) CHECK(findRigModel(all, m) >= 0, "only known models (%d)", m);
        int calls = 0;
        auto res = detectRig(plan, [&](int m, int b) { calls++; return m == 1035 && b == 38400; }, all);
        CHECK(res.found && res.model == 1035 && res.baud == 38400 && res.note.find("share CAT") != std::string::npos && calls == res.tries, "found the FT-991: %s", res.note.c_str());
        res = detectRig(plan, [](int, int) { return false; }, all);
        CHECK(!res.found && res.tries == (int)plan.size() && res.note.find("no rig") != std::string::npos, "nothing answers");
        const auto net = networkRigPresets(all);
        CHECK(net.size() == 2 && net[0].model == 2 && net[0].path == "localhost:4532" && net[1].model == 4 && net[1].path == "localhost:12345", "rigctld and FlRig presets");
        HamlibRig r = rig7300();
        const std::vector<std::string> outs = {"MacBook Speakers", "USB Audio CODEC", "BlackHole 2ch"};
        CHECK(pairRigAudio(r, all, outs) == 1, "IC-7300: the USB Audio CODEC");
        r.model = 3085;
        CHECK(pairRigAudio(r, all, {"Speakers", "IC-705 Audio", "USB Audio CODEC"}) == 1, "IC-705: its own named device");
        CHECK(pairRigAudio(r, all, {"Speakers", "HDMI"}) == -1, "nothing that looks like a rig");
    }
    // ---- hints and the profile ----
    {
        CHECK(hamlibErrorHint("Hamlib open /dev/cu.x: Communication timed out").find("baud rate") != std::string::npos, "timeout hint");
        CHECK(hamlibErrorHint("Hamlib open localhost:4532: Connection refused").find("rigctld") != std::string::npos, "network hint");
        CHECK(hamlibErrorHint("Hamlib open COM3: IO error").find("Device Manager") != std::string::npos, "port hint");
        CHECK(hamlibErrorHint("").empty() && hamlibErrorHint("odd").rfind("odd", 0) == 0, "unknown errors kept");
        CHECK(std::string(rigPttName(0)) == "CAT" && std::string(rigPttName(1)) == "RTS" && std::string(rigPttName(2)) == "DTR" && std::string(rigPttName(3)) == "VOX", "PTT names");
        HamlibRig a = rig7300(); a.label = "shack"; a.pttType = 1; a.pttPath = "/dev/cu.ptt,1"; a.dataMode = "PKTUSB"; a.audioOutId = "USB Audio CODEC"; a.split = true;
        HamlibRig b = rig7300();
        const auto back = hamlibRigsFromString(hamlibRigsToString({a, b}));
        CHECK(back.size() == 2 && back[0] == a && back[1] == b, "the TX profile round-trips");
        CHECK(hamlibRigsToString({b}) == "3073,/dev/cu.usbserial-1420,,,115200", "a receive-only rig keeps the short form: %s", hamlibRigsToString({b}).c_str());
        const auto old = hamlibRigsFromString("3073,/dev/cu.x,IC,dev,19200");
        CHECK(old.size() == 1 && old[0].pttType == 0 && !old[0].split && old[0].baud == 19200, "an older entry reads with defaults");
    }
    // ---- the Hamlib transmit sink warns outside the amateur bands and keys (dummy rig, no radio) ----
    if (hamlibSupported() && hamlibLibraryReady()) {   // a run-time Hamlib build without the library (Windows CI) skips it
        HamlibRig r; r.model = 1; r.path = "dummy";
        auto devs = hamlibRigDevices({r});
        auto s = makeTxSink(devs[0]);
        TxTune t; t.centerHz = 100e6; t.sampleRate = 48000;
        CHECK(s && s->open(t, err), "100 MHz: a warning on stderr, the CAT rig is keyed (%s)", err.c_str());
        if (s) s->close();
        t.centerHz = 14.074e6;
        CHECK(s && s->open(t, err), "14.074 MHz on the dummy rig: %s", err.c_str());
        if (s) s->close();
    }
    return testSummary("rig_control");
}
