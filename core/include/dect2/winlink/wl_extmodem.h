// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// External modems for Winlink and peer-to-peer links (3.9, docs/HFMODEMS-3.9.md).
//
// OnAir does not contain, ship or reimplement ARDOP (ardopcf), VARA or Mercury. Each is a separate program that does the radio part.
// OnAir connects to it over TCP, the way Winlink Express and Pat do, and carries the B2F session over the modem's data port:
//
//   ARDOP    ardopcf host interface: command port (default 8515), data port = command port + 1. Commands end in CR. The data port
//            carries frames: 2-byte big-endian length, a 3-letter tag ("ARQ", "FEC", "IDF", "ERR"), then the bytes. What OnAir
//            sends has the length and no tag.
//   VARA     VARA's "Native TNC Commands" (EA5HVK): command port 8300, data port 8301, CR-terminated text. The data port is raw bytes.
//   Mercury  Rhizomatica Mercury, "VARA-compatible TCP TNC interface" (docs/TNC.md): 8300 / 8301, CR-terminated, raw data port.
//
// The commands used come only from those published descriptions. Everything here was tested against mock modems built from the same
// descriptions (tests/winlink/test_winlink_extmodem.cpp): no real ARDOP, VARA or Mercury program was used.
#pragma once
#include "dect2/winlink/wl_link.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {
namespace winlink {

enum class ModemType { Ardop = 0, VaraHF = 1, VaraFM = 2, Mercury = 3 };
const char* modemTypeName(ModemType t);          // "ARDOP", "VARA HF", "VARA FM", "Mercury"

// One entry of the modem list: where the modem program listens
struct ModemProfile {
    std::string name;                // shown in the list; Gateway::modem refers to it
    ModemType type = ModemType::Ardop;
    std::string host = "127.0.0.1";
    int cmdPort = 8515;
    int dataPort = 8516;
    std::string bw;                  // "" = leave the modem's own setting. ARDOP: 500MAX, 1000FORCE ...; VARA HF / Mercury: 500, 2300, 2750
    int connectTimeoutSec = 0;       // 0: 120 s (Mercury 330 s: it calls for up to 300 s)
    std::string pttNote, audioNote;  // notes for the operator only: the modem program owns the radio and the sound card
    int tcpAttempts = 2;             // tries to open the two ports before reporting "modem not running"
};
// ports and timeout the type normally uses (ARDOP 8515 / 8516, VARA and Mercury 8300 / 8301)
ModemProfile defaultModemProfile(ModemType t);
// the BW values the type documents (the field stays editable): ARDOP 200MAX .. 2000FORCE, VARA HF and Mercury 500 / 2300 / 2750, VARA FM none
std::vector<std::string> modemBwChoices(ModemType t);
bool modemBwValid(ModemType t, const std::string& bw);
std::string modemBwCommand(ModemType t, const std::string& bw);   // "ARQBW 500MAX", "BW2300"; empty when bw is empty or the type has no BW command

struct ModemHelp {
    std::string steps;       // setup steps (plain text, one per line)
    std::string installUrl;  // where the user gets the program
    std::string licence;     // a short licence note: shown as a warning, never blocks
    std::string source;      // the document the interface was taken from
};
ModemHelp modemHelp(ModemType t);

// one profile per line: name|type|host|cmdPort|dataPort|bw|timeoutSec|pttNote|audioNote
std::string saveModemProfiles(const std::vector<ModemProfile>& p);
std::vector<ModemProfile> loadModemProfiles(const std::string& text);

// "Test connection": opens the command port and the data port, asks VERSION, sends nothing over the air.
struct ModemTestResult {
    bool cmdOpen = false, dataOpen = false, answered = false;
    std::string version;     // the modem's reply to VERSION, when there was one
    std::string message;     // one sentence for the user
};
ModemTestResult testModemProfile(const ModemProfile& p, int timeoutMs = 3000);

struct ExtModemConfig {
    ModemProfile profile;
    std::string mycall;
    std::string peer;                    // empty while listening
    std::vector<std::string> via;        // VARA FM digipeaters
    bool p2p = false;                    // VARA HF: P2P SESSION instead of WINLINK SESSION
    int arqRepeats = 5;                  // ARDOP: repeats of the connect request (2 - 15)
    double idleWatchdogSec = 150;        // VARA sends IAMALIVE every 60 s: nothing heard for this long means the modem is gone
    double disconnectWaitSec = 60;       // DISCONNECT without an answer for this long: ABORT
};

class ExtModemLink : public ByteLink {
public:
    explicit ExtModemLink(const ExtModemConfig& c);
    ~ExtModemLink() override;
    void connect(double now) override;
    void listen() override;
    void write(const uint8_t* d, size_t n) override;
    void disconnect(double now) override;
    void abort() override;
    void tick(double now) override;
    size_t pendingTx() const override;
    std::string remoteCall() const override { return remote_; }
    std::string detail() const override;
    // what the modem reported
    int bufferBytes() const { return buffer_; }
    bool busy() const { return busy_; }
    bool ptt() const { return ptt_; }
    double snr() const { return snr_; }                       // dB, NaN-free: valid() tells
    bool snrKnown() const { return snrKnown_; }
    const std::string& bandwidth() const { return bw_; }      // as CONNECTED reported it
    const std::string& version() const { return version_; }
    const std::string& bitrate() const { return bitrate_; }
    bool pending() const { return pending_; }                 // a connect request was heard (PENDING)
private:
    void start();
    void run();
    void sendCmd(const std::string& s, bool critical = false);
    void sendInit();
    void handleLine(const std::string& l, double now);
    void deliverData(const uint8_t* d, size_t n);
    void flushTx();
    void closeSockets();
    void failNow(const std::string& why);
    ExtModemConfig c_;
    bool arqStyle() const { return c_.profile.type == ModemType::Ardop; }
    // thread side
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<std::string> lines_;
    std::vector<uint8_t> data_;
    int thState_ = 0;                // 0 not started, 1 connecting, 2 up, 3 closed by the modem, 4 failed
    std::string thErr_;
    long long cs_ = -1, ds_ = -1;    // sockets
    std::string lineBuf_;
    std::vector<uint8_t> frameBuf_;
    // main side
    bool wantListen_ = false, initDone_ = false, started_ = false;
    std::vector<uint8_t> txq_;
    std::vector<uint8_t> rxHold_;
    std::vector<std::string> sentCmds_;   // VARA / Mercury answer OK or WRONG in order
    std::string remote_, bw_, version_, bitrate_;
    int buffer_ = 0;
    bool busy_ = false, ptt_ = false, pending_ = false, snrKnown_ = false, discSent_ = false, sawLinkUp_ = false;
    double snr_ = 0;
    double now_ = 0, callAt_ = -1, heardAt_ = 0, discAt_ = -1;
};

} // namespace winlink
} // namespace dect2
