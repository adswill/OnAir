// Winlink email station: the mailbox folders on disk, the gateway list (entered by hand: OnAir never looks gateways up on the
// internet), the optional Telnet link to the Winlink CMS (off unless the user turns it on), and WinlinkStation, which runs one B2F
// session over one link: AX.25 packet 1200 / 9600 baud, ARDOP, or Telnet.
#pragma once
#include "dect2/winlink/b2f.h"
#include "dect2/winlink/b2msg.h"
#include "dect2/winlink/wl_link.h"
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {
namespace winlink {

// ---------------------------------------------------------------- mailbox

enum class Folder { Inbox, Outbox, Sent, Archive };
const char* folderName(Folder f);   // "inbox", "outbox", "sent", "archive"

struct MailEntry {
    std::string mid, subject, from, date;
    std::vector<std::string> to;
    size_t bytes = 0;
    int files = 0;
};

// One message per file: <dir>/<folder>/<MID>.b2f (the B2 bytes, the same form Pat keeps)
class Mailbox {
public:
    explicit Mailbox(std::string dir = std::string());
    void setDir(const std::string& dir);
    const std::string& dir() const { return dir_; }
    bool save(Folder f, const B2Message& m, std::string* err = nullptr);
    bool load(Folder f, const std::string& mid, B2Message& m) const;
    std::vector<MailEntry> list(Folder f) const;       // newest first
    bool move(const std::string& mid, Folder from, Folder to);
    bool remove(Folder f, const std::string& mid);
    bool has(const std::string& mid) const;            // in any folder (a gateway offering it again gets "-")
private:
    std::string path(Folder f, const std::string& mid) const;
    std::string dir_;
};

// ---------------------------------------------------------------- gateways

enum class Transport { Packet1200 = 0, Packet9600 = 1, Ardop = 2, Telnet = 3 };
const char* transportName(Transport t);

struct Gateway {
    std::string call;             // the gateway's call sign (RMS), e.g. "LY2XYZ-10"
    Transport transport = Transport::Packet1200;
    double mhz = 0;               // dial frequency (ARDOP: upper sideband dial; packet: the channel)
    std::string via;              // packet digipeaters, comma separated
    std::string host;             // Telnet: host name
    int port = 8772;              // Telnet: port
    std::string note;
};
// one gateway per line: call|transport|mhz|via|host|port|note
std::string saveGateways(const std::vector<Gateway>& g);
std::vector<Gateway> loadGateways(const std::string& text);

// ---------------------------------------------------------------- Telnet

// TCP to the Winlink CMS (or any B2F Telnet host): the "Callsign :" / "Password :" login, then the bytes as they come.
// The connection and the reading run on a worker thread; tick() hands the received bytes over on the caller's thread.
class TelnetLink : public ByteLink {
public:
    TelnetLink(std::string host, int port, std::string mycall, std::string loginPassword = "CMSTelnet");
    ~TelnetLink() override;
    void connect(double now) override;
    void listen() override {}
    void write(const uint8_t* d, size_t n) override;
    void disconnect(double now) override;
    void abort() override;
    void tick(double now) override;
    size_t pendingTx() const override { return 0; }
private:
    void run();
    std::string host_, mycall_, loginPw_;
    int port_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<uint8_t> rx_;
    std::string thErr_;
    int thState_ = 0;             // 0 connecting, 1 logging in, 2 connected, 3 closed, 4 failed
    long long sock_ = -1;
    std::string loginBuf_;
};

// ---------------------------------------------------------------- the station

struct StationConfig {
    std::string mycall, locator, password;   // password: the Winlink account's, for the secure login (kept by the app, never logged)
    Gateway gateway;
    bool telnetEnabled = false;               // Settings: "Allow Telnet to the CMS" (off by default)
    int paclen = 128, window = 4;
    double t1Sec = 0;                          // 0: from the speed (6 s at 1200 baud, 3 s at 9600 baud)
};

class WinlinkStation {
public:
    explicit WinlinkStation(Mailbox& mb);
    ~WinlinkStation();
    // false with err: no call sign, Telnet not allowed, a session already running
    bool start(const StationConfig& c, double now, std::string& err);
    void stop(double now);                   // ends the session politely (disconnect); a second stop aborts
    void tick(double now);
    bool running() const;
    // frames from the receivers (the app's frame taps; call on the tick thread)
    void rxAx25(const ax25::Frame& f, double now);
    void rxArdop(const ArdopRxFrameIn& f, double now);
    // frames to transmit, collected since the last call
    std::vector<ax25::Frame> takeAx25();
    std::vector<ArdopLinkFrame> takeArdop();
    // status
    std::string status() const;
    std::vector<std::string> takeLog();
    int received() const { return session_ ? session_->messagesReceived() : lastRx_; }
    int sent() const { return session_ ? session_->messagesSent() : lastTx_; }
    bool lastOk() const { return lastOk_; }
    const std::string& lastError() const { return lastErr_; }
    const B2fSession* session() const { return session_.get(); }
    const ByteLink* link() const { return link_.get(); }
private:
    void finish(double now);
    Mailbox& mb_;
    StationConfig c_;
    std::unique_ptr<ByteLink> link_;
    Ax25Link* ax_ = nullptr;
    ArdopLink* ar_ = nullptr;
    std::unique_ptr<B2fSession> session_;
    std::vector<ax25::Frame> outAx_;
    std::vector<ArdopLinkFrame> outAr_;
    std::vector<std::string> log_;
    bool stopping_ = false, sessionEnded_ = false;
    int lastRx_ = 0, lastTx_ = 0;
    bool lastOk_ = false;
    std::string lastErr_;
    double startedAt_ = 0;
};

} // namespace winlink
} // namespace dect2
