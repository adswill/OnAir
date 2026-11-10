// Winlink B2F (FBB compressed forwarding, version 2) session over any reliable byte stream (an AX.25 connection, an ARDOP ARQ
// connection, or the Telnet CMS link): the handshake (SID, ;FW, the ;PQ / ;PR secure login), turns of proposals ("FC EM <mid> <size>
// <compressed size> 0", "F> <checksum>"), answers ("FS +-="), the compressed blocks (SOH title offset, STX data, EOT checksum), and
// the end ("FF" no more messages, "FQ" quit).
//
// Two roles: Client (OnAir calling a gateway: the gateway speaks first, the client offers its messages first) and Gateway (the answering
// side; OnAir uses it for the local mock gateway of the tests and for peer-to-peer tests, never as a public gateway).
//
// Byte driven and non-blocking: feed received bytes with onBytes(); what to send comes out of Hooks::send. No threads, no timers (the
// link below handles timeouts).
//
// Follows the public B2F protocol notes and wl2k-go's fbb package (Copyright 2015-2016 Martin Hebnes Pedersen LA5NTA, MIT licence;
// see THIRD_PARTY.md). New C++ code.
#pragma once
#include "dect2/winlink/b2msg.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace winlink {

struct B2Proposal {
    char code = 'C';                 // 'C': B2 (LZHUF) message
    std::string type = "EM";         // "EM" encapsulated message, "CM" (CMS internal)
    std::string mid;
    int size = 0;                    // uncompressed bytes
    int csize = 0;                   // compressed bytes
    std::string title;               // the subject, sent in the SOH header
    int offset = 0;                  // resume point the other side asked for
    char answer = 0;                 // '+' accept, '-' reject (already have it), '=' defer
    std::vector<uint8_t> cdata;      // LZHUF (B2) data
};

// A proposal for a message: encoded and compressed
B2Proposal makeProposal(const B2Message& m);
// The line of a proposal ("FC EM MID 1234 567 0") and the checksum of a block of proposal lines
std::string proposalLine(const B2Proposal& p);
int proposalChecksum(const std::vector<std::string>& lines);
// Parses "FC EM MID size csize 0". False when it is not a B2 proposal.
bool parseProposalLine(const std::string& line, B2Proposal& p);

class B2fSession {
public:
    enum class Role { Client, Gateway };
    struct Config {
        Role role = Role::Client;
        std::string mycall;              // our call (with SSID if any)
        std::string targetCall;          // the gateway's call (client) or the caller (gateway, only for the greeting)
        std::string locator;             // Maidenhead locator, in the greeting line
        std::string password;            // client: the Winlink password for the secure login (empty: none sent)
        std::string appName = "OnAir", appVersion = "3.9";
        // gateway role
        std::string challenge;           // ";PQ" challenge to send (empty: none)
        std::string expectPassword;      // the password the caller must prove (empty: anything is accepted)
        std::vector<std::string> motd;   // greeting lines before the SID
    };
    struct Hooks {
        std::function<void(const uint8_t* d, size_t n)> send;               // bytes to the link
        std::function<std::vector<B2Proposal>()> outbound;                  // our turn: what we offer now (at most 5 are used)
        std::function<char(const B2Proposal& p)> inboundAnswer;             // '+', '-' or '='; default '+'
        std::function<void(const B2Message& m, const B2Proposal& p)> received;
        std::function<void(const std::string& mid, bool rejected)> sent;    // delivered (or refused as a duplicate by the other side)
        std::function<void(const std::string& line)> log;                  // protocol trace ("<" received, ">" sent)
    };
    enum class State { Idle, Handshake, OurTurn, WaitAnswer, TheirTurn, Receiving, Done, Failed };

    B2fSession(const Config& c, const Hooks& h);
    void start();                                   // client: waits for the greeting; gateway: sends the greeting
    void onBytes(const uint8_t* d, size_t n);
    void linkClosed();                              // the connection ended: Failed unless the session had ended
    State state() const { return st_; }
    bool done() const { return st_ == State::Done; }
    bool failed() const { return st_ == State::Failed; }
    const std::string& error() const { return err_; }
    const std::string& remoteSid() const { return remoteSid_; }
    const std::string& challengeSeen() const { return challengeSeen_; }
    bool loginOk() const { return loginOk_; }       // gateway: the ;PR answer matched (or none was required)
    // transfer progress of the message being received
    size_t rxBytes() const { return dataBuf_.size(); }
    size_t rxTotal() const { return curIdx_ < rxProps_.size() ? (size_t)rxProps_[curIdx_].csize : 0; }
    uint64_t bytesIn() const { return bytesIn_; }
    uint64_t bytesOut() const { return bytesOut_; }
    int messagesReceived() const { return nRx_; }
    int messagesSent() const { return nTx_; }

private:
    void sendStr(const std::string& s);
    void sendLine(const std::string& s);
    void fail(const std::string& why);
    void process();
    bool takeLine(std::string& line);
    void handshakeLine(const std::string& line);
    void sendClientHandshake();
    void ourTurn();
    void theirLine(const std::string& line);
    void answerLine(const std::string& line);
    bool receiveBinary();
    void commitSent();
    void sendBlock(const B2Proposal& p);
    void trace(char dir, const std::string& s);

    Config cfg_;
    Hooks h_;
    State st_ = State::Idle;
    std::string err_, remoteSid_, challengeSeen_;
    bool loginOk_ = false;
    std::vector<uint8_t> in_;
    size_t inPos_ = 0;
    bool remoteNoMsgs_ = false, quitSent_ = false;
    std::vector<B2Proposal> txProps_, rxProps_;
    std::vector<std::pair<std::string, bool>> pendingSent_;
    int rxSum_ = 0;
    size_t curIdx_ = 0;
    std::vector<uint8_t> dataBuf_;
    int dataSum_ = 0;
    bool haveHeader_ = false;
    uint64_t bytesIn_ = 0, bytesOut_ = 0;
    int nRx_ = 0, nTx_ = 0;
};

} // namespace winlink
} // namespace dect2
