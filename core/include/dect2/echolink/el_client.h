// EchoLink client: directory log-in and station list (TCP), one connection (QSO) to a station, link, repeater or conference (UDP RTP/RTCP),
// GSM 06.10 audio both ways and text chat. The protocol is described in el_proto.h.
// The password is the user's: it is kept in the settings (a credential key, see backup.h isSecretPrefKey: never in a plain backup), given to
// the client in memory, sent only inside the log-in command to the directory server the user configured, and never put in a log line,
// a status text or an error message (loginCommandRedacted is what the log shows).
// Session is the connection logic alone (no sockets, the caller's clock) so the tests can drive it; Client adds the sockets and a thread.
#pragma once
#include "dect2/echolink/el_proto.h"
#include "dect2/echolink/gsm610.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {
namespace echolink {

enum class QsoState { Idle, Connecting, Connected };
const char* qsoStateName(QsoState s);   // "not connected", "connecting", "connected"

struct RxPacket { uint16_t seq; int nframes; uint8_t frames[kFramesPerPacket][kGsmFrameBytes]; };

class Session {
public:
    // ctrl: the control port (else the audio port); the packet goes to ip:port
    using SendFn = std::function<void(bool ctrl, const std::string& ip, int port, const std::vector<uint8_t>& pkt)>;
    void setSend(SendFn f) { send_ = std::move(f); }
    void setLocal(const std::string& call, const std::string& name, const std::string& info);
    void setAcceptIncoming(bool on) { accept_ = on; }

    // Start a connection: SDES to ip at its audio port + 1, repeated every kKeepAliveMs, given up after kConnectRetries
    bool connect(const std::string& ip, int remoteAudioPort, int64_t nowMs);
    void disconnect();                                   // BYE, back to Idle
    void onCtrl(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs);
    void onAudio(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs);
    void tick(int64_t nowMs);                            // keep-alive, connect retries, time-out
    bool sendAudio(const uint8_t frames[kFramesPerPacket][kGsmFrameBytes]);
    bool sendChat(const std::string& message);

    QsoState state() const { return state_; }
    const std::string& peerCall() const { return peerCall_; }
    const std::string& peerName() const { return peerName_; }
    const std::string& peerIp() const { return peerIp_; }
    const std::string& peerInfo() const { return peerInfo_; }   // the station's info text (a conference: who is connected)
    bool rxActive(int64_t nowMs) const { return state_ == QsoState::Connected && lastAudioMs_ >= 0 && nowMs - lastAudioMs_ < 300; }
    // what arrived since the last call
    std::vector<RxPacket> takeAudio();
    std::vector<std::string> takeChat();
    std::vector<std::string> takeEvents();               // "connected to LY1ABC", "LY1ABC disconnected", ...
    uint64_t packetsIn() const { return pktIn_; }
    uint64_t packetsOut() const { return pktOut_; }
    uint64_t packetsLost() const { return lost_; }       // gaps in the peer's sequence numbers
    uint64_t packetsLate() const { return late_; }       // duplicates and out-of-order packets (dropped)

private:
    void sendSdes();
    void sendBye(const std::string& ip, int ctrlPort);
    void setConnected(int64_t nowMs);
    void toIdle(const std::string& why);
    SendFn send_;
    std::string call_, name_, info_;
    bool accept_ = true;
    QsoState state_ = QsoState::Idle;
    std::string peerIp_, peerCall_, peerName_, peerInfo_;
    int peerCtrl_ = 0, peerAudio_ = 0;
    int64_t lastSdesOut_ = 0, lastHeard_ = 0, lastAudioMs_ = -1;
    int retries_ = 0;
    uint16_t txSeq_ = 0;
    bool haveRxSeq_ = false;
    uint16_t rxSeq_ = 0;
    uint64_t pktIn_ = 0, pktOut_ = 0, lost_ = 0, late_ = 0;
    std::vector<RxPacket> rx_;
    std::vector<std::string> chat_, events_;
};

struct ClientConfig {
    std::string call, password, name, location;
    std::string info;                                    // the text a connecting station sees ("" = "Running OnAir")
    std::vector<std::string> servers{"servers.echolink.org"};
    int directoryPort = kDirectoryPort;
    int audioPort = kDefaultAudioPort;                   // local UDP ports: audio and audio + 1 (control)
    std::string bindAddress = "0.0.0.0";
    bool acceptIncoming = true;
    int directoryTimeoutMs = 10000;
};

struct ClientStatus {
    bool portsOpen = false;
    bool loggedIn = false;
    bool busy = false;                                   // a directory operation is running
    std::string directory;                               // "logged in", "incorrect password", "cannot reach ...": never holds the password
    QsoState qso = QsoState::Idle;
    std::string peerCall, peerName, peerIp, peerInfo;
    bool rx = false, tx = false;
    uint64_t packetsIn = 0, packetsOut = 0, lost = 0;
    int listAgeSec = -1;                                 // seconds since the station list arrived (-1 = none yet)
};

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void configure(const ClientConfig& c);               // takes effect for the next operation; ports on the next open()
    bool open(std::string& err);                         // binds the UDP ports and starts the network thread
    void close();                                        // BYE, log-out (when logged in), ports closed
    bool isOpen() const { return open_; }

    // directory, run on a worker; results in status() / stations()
    void login();                                        // status Online, refreshed every 5 minutes while logged in
    void logout();
    void refreshList();
    std::vector<Station> stations() const;

    bool connectTo(const std::string& ip, int remoteAudioPort = kDefaultAudioPort);
    bool connectCall(const std::string& call);           // looks the callsign up in the station list
    void disconnect();
    bool sendChat(const std::string& message);

    // audio at 8 kHz. setTransmit(true) sends what pushTx() gives, 4 frames per packet; false flushes the rest (padded with silence).
    void setTransmit(bool on);
    bool transmitting() const { return tx_; }
    void pushTx(const int16_t* pcm, int n);
    int pullRx(int16_t* out, int n);                     // decoded audio from the peer; fewer than n when not enough arrived
    int rxBuffered() const;

    ClientStatus status() const;
    std::vector<std::string> takeChat();
    std::vector<std::string> takeLog();                  // events and directory results, password-free
    int localAudioPort() const { return boundAudio_; }

private:
    void run();
    void dirJob(int kind);
    bool dirExchange(const std::string& cmd, bool list, std::string& reply, std::string& err);
    void log(const std::string& s);
    void flushTx(bool pad);

    mutable std::mutex mu_;
    ClientConfig cfg_;
    Session sess_;
    GsmEncoder enc_;
    GsmDecoder dec_;
    std::deque<int16_t> txq_, rxq_;
    std::vector<Station> stations_;
    int64_t listAt_ = -1;
    std::vector<std::string> log_, chat_;
    std::string dirText_;
    bool loggedIn_ = false;
    int64_t lastLogin_ = 0;
    std::atomic<bool> open_{false}, stop_{false}, tx_{false}, dirBusy_{false};
    int boundAudio_ = 0;
    long long sa_ = -1, sc_ = -1;                       // sockets (sock_t fits)
    std::thread net_, dir_;
};

int64_t monoMs();

} // namespace echolink
} // namespace dect2
