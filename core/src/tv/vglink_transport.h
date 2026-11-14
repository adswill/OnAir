// What carries the video generator link's datagrams (vglink.h, docs/VIDEOGEN-LINK.md): internal to vglink.cpp.
//
//  - local (the default, #458): no ports at all. Windows: the named pipe \\.\pipe\onair-videogen-<user> in message mode (one pipe
//    message = one RTP or RTCP packet); Linux / macOS: a Unix-domain datagram socket in the user's runtime folder (the POSIX form of
//    a named pipe that keeps datagrams whole; a FIFO would be a byte stream). RTP and RTCP share it (RFC 5761: told apart by the
//    packet type).
//  - network (OnAir's checkbox): RTP on a UDP port and RTCP on the next one, as RFC 3550.
// Nothing waits on a slow reader: a datagram that does not fit is dropped, as UDP would.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::vgen::link {

// one datagram from a generator: `from` is where the answer goes (and who sent it), `host` what OnAir shows ("" on this computer)
using OnDatagram = std::function<void(const uint8_t* d, size_t n, const std::string& from, const std::string& host)>;

// OnAir's side: its own thread(s) call `cb` for every datagram, from any generator
class ServerTransport {
public:
    virtual ~ServerTransport() = default;
    virtual bool send(const std::string& to, const uint8_t* d, size_t n) = 0;   // thread safe; false: not sent
    virtual void stop() = 0;                                                     // joins the threads; no callback after it
    virtual int port() const { return 0; }                                       // network: the RTP port
};
// RTP on `port` (even; 0: a free pair), RTCP on port + 1, on 127.0.0.1 or every card
std::unique_ptr<ServerTransport> listenNetwork(int port, bool anyAddress, OnDatagram cb, std::string& err);
// the local pipe / socket `name` ("": localName()); false when another OnAir has it
std::unique_ptr<ServerTransport> listenLocal(const std::string& name, OnDatagram cb, std::string& err);
std::string localName();   // this user's: \\.\pipe\onair-videogen-<user>, or <runtime dir>/onair-videogen.sock

// the generator's side: one OnAir
class ClientTransport {
public:
    virtual ~ClientTransport() = default;
    virtual bool open(std::string& why) = 0;   // resolve / connect; false (with why): try again later
    virtual bool isOpen() const = 0;
    virtual void close() = 0;
    // never waits long; false: not sent (full, or OnAir gone: then isOpen() turns false)
    virtual bool sendRtp(const uint8_t* d, size_t n) = 0;
    virtual bool sendRtcp(const uint8_t* d, size_t n) = 0;
    // one datagram from OnAir within ms: its length, or -1 (none)
    virtual long long recv(std::vector<uint8_t>& buf, int ms) = 0;
    virtual std::string where() const = 0;     // "this computer" or "host:port"
};
std::unique_ptr<ClientTransport> networkClient(const std::string& host, int port);
std::unique_ptr<ClientTransport> localClient(const std::string& name);   // "": localName()

} // namespace dect2::vgen::link
