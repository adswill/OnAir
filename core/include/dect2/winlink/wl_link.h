// Reliable byte streams for Winlink on top of OnAir's radio modems:
//
//  Ax25Link   AX.25 version 2.0 connected mode (modulo 8): SABM / UA to connect, I frames with N(S) / N(R) in a window of k frames,
//             RR / REJ acknowledgements, T1 retries with go-back-N, DISC / UA (or DM) to end. The frames go out through sendFrame
//             (the app renders them with the 1200 baud AFSK or 9600 baud G3RUH packet modem) and come in through onFrame (frames
//             decoded by the packet receiver).
//  ArdopLink  An ARQ session made of ARDOP frame types: ConReq500M / ConAck500 to connect, 4FSK.500.100 data frames (64 bytes, even /
//             odd toggle) answered by DataACK / DataNAK, IDLE when the sending side (ISS) has nothing to say, BREAK for the receiving
//             side (IRS) to take over, DISC / END to finish. This is a simplified ARQ built from the frame layer OnAir has
//             (ardop_proto.h): it talks to another OnAir, and to the local mock gateway of the tests; working with ardopcf or
//             ARDOP_Win over the air is not verified.
//
// Both are driven from one thread: frames in through onFrame(), time through tick(now). Nothing here touches a radio.
#pragma once
#include "dect2/packet/packet_ax25.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace winlink {

enum class LinkState { Idle, Listening, Connecting, Connected, Disconnecting, Closed, Failed };
const char* linkStateName(LinkState s);

class ByteLink {
public:
    virtual ~ByteLink() = default;
    virtual void connect(double now) = 0;                     // calling side
    virtual void listen() = 0;                                // answering side (tests, peer to peer)
    virtual void write(const uint8_t* d, size_t n) = 0;       // queued, sent when the link allows
    virtual void disconnect(double now) = 0;                  // after the queued data has been acknowledged
    virtual void abort() = 0;                                 // at once, no frame sent
    virtual void tick(double now) = 0;
    virtual size_t pendingTx() const = 0;                     // bytes not yet acknowledged
    LinkState state() const { return st_; }
    const std::string& error() const { return err_; }
    std::function<void(const uint8_t* d, size_t n)> onData;   // bytes received in order
    std::function<void(LinkState)> onState;
    std::function<void(const std::string&)> log;
    uint64_t framesSent = 0, framesReceived = 0, retries = 0;
protected:
    void setState(LinkState s, const std::string& why = std::string());
    LinkState st_ = LinkState::Idle;
    std::string err_;
};

// ---------------------------------------------------------------- AX.25

struct Ax25LinkConfig {
    std::string mycall;          // CALL or CALL-SSID
    std::string peer;            // the gateway; empty while listening (taken from the SABM)
    std::vector<std::string> via;   // digipeaters
    int paclen = 128;            // bytes per I frame
    int window = 4;              // k, at most 7
    double t1Sec = 6;            // retry timer (FRACK); about 3 s at 9600 baud, 6 s at 1200 baud
    int n2 = 10;                 // retries before giving up
};

class Ax25Link : public ByteLink {
public:
    explicit Ax25Link(const Ax25LinkConfig& c);
    std::function<void(const ax25::Frame&)> sendFrame;        // to the modem
    void onFrame(const ax25::Frame& f, double now);           // from the receiver (frames for other stations are ignored)
    void connect(double now) override;
    void listen() override;
    void write(const uint8_t* d, size_t n) override;
    void disconnect(double now) override;
    void abort() override;
    void tick(double now) override;
    size_t pendingTx() const override;
    const Ax25LinkConfig& config() const { return c_; }
private:
    void sendU(uint8_t ctl, bool command);
    void sendS(uint8_t type, bool command, bool pf);
    void sendI(int ns, bool poll);
    void pump(double now);
    void ackTo(int nr, double now);
    bool forMe(const ax25::Frame& f) const;
    Ax25LinkConfig c_;
    ax25::Address me_, peer_;
    std::vector<ax25::Address> via_;
    std::deque<uint8_t> txq_;
    std::vector<uint8_t> win_[8];
    int vs_ = 0, vr_ = 0, va_ = 0, nbuf_ = 0;   // nbuf_: frames held from V(A) on (sent, not yet acknowledged)
    int tries_ = 0;
    double t1At_ = -1;
    bool ackDue_ = false, rejSent_ = false, discWanted_ = false;
    double now_ = 0;
};

// ---------------------------------------------------------------- ARDOP

struct ArdopLinkFrame {
    int code = 0;
    uint8_t sid = 0xFF;
    std::vector<uint8_t> body;       // ardop_proto.h make... (what follows the two type bytes); for data frames the 4FSK block
};

struct ArdopLinkConfig {
    std::string mycall, peer;        // peer empty while listening
    int leaderMs = 240;
    double ackTimeoutSec = 9;        // a data frame (about 4 s at 100 baud) plus the answer
    double idleRepeatSec = 4;        // the ISS repeats IDLE this often while nobody has anything to send
    int retries = 10;
};

// The received frame as the ARDOP receiver reports it
struct ArdopRxFrameIn {
    int code = 0;
    int sid = -1;
    bool bodyOk = false;
    std::string from, to;
    std::vector<uint8_t> payload;    // data frames: the payload bytes that passed the CRC
};

uint8_t ardopSessionId(const std::string& caller, const std::string& target);

class ArdopLink : public ByteLink {
public:
    explicit ArdopLink(const ArdopLinkConfig& c);
    std::function<void(const ArdopLinkFrame&)> sendFrame;
    void onFrame(const ArdopRxFrameIn& f, double now);
    void connect(double now) override;
    void listen() override;
    void write(const uint8_t* d, size_t n) override;
    void disconnect(double now) override;
    void abort() override;
    void tick(double now) override;
    size_t pendingTx() const override { return txq_.size() + cur_.size(); }
    bool isIss() const { return iss_; }
    uint8_t sid() const { return sid_; }
private:
    void send(int code, std::vector<uint8_t> body = {});
    void sendData(double now);
    void becomeIss(double now);
    ArdopLinkConfig c_;
    uint8_t sid_ = 0xFF;
    bool iss_ = false;
    std::deque<uint8_t> txq_;
    std::vector<uint8_t> cur_;       // the data frame waiting for its ACK
    bool curOdd_ = false, curIsIdle_ = false;
    int lastRxOdd_ = -1;
    bool breakPending_ = false;
    int tries_ = 0;
    double timerAt_ = -1;
    bool discWanted_ = false;
};

} // namespace winlink
} // namespace dect2
