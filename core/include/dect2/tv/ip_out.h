// IP output of the broadcast data modes: the IP datagrams of DVB-H / IP datacasting (MPE and MPE-FEC sections in a transport stream)
// and of DVB-C2 GSE PLPs, written to a pcap file (libpcap, link type RAW: the IP packets as they are) and/or forwarded over UDP.
// Both are off by default. The forward sends the payload of each UDP datagram (after IPv4 reassembly) to one address, so a player can
// listen there (an RTP or MPEG-TS multicast service of the broadcast plays as udp://@:port); port 0 keeps each datagram's own
// destination port. With `wholePackets` the whole IP packet is sent as the UDP payload instead (any protocol, for an analyser).
// Only what was received in the clear goes out: nothing is decrypted.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

struct IpOutConfig {
    bool pcap = false;
    std::string pcapPath;              // the file (created or truncated when the output is switched on)
    bool udp = false;
    std::string udpHost = "127.0.0.1"; // a dotted IPv4 address (unicast, multicast or broadcast)
    int udpPort = 5004;                // 0: each datagram's own destination port
    bool wholePackets = false;         // forward whole IP packets instead of UDP payloads
    bool operator==(const IpOutConfig& o) const {
        return pcap == o.pcap && pcapPath == o.pcapPath && udp == o.udp && udpHost == o.udpHost && udpPort == o.udpPort && wholePackets == o.wholePackets;
    }
};

struct IpOutStats {
    uint64_t packets = 0;        // IP packets offered
    uint64_t invalid = 0;        // not a valid IPv4 / IPv6 packet: dropped
    uint64_t written = 0;        // to the pcap file
    uint64_t bytesWritten = 0;
    uint64_t forwarded = 0;      // UDP datagrams sent
    uint64_t notUdp = 0;         // payload forward: packets that are not UDP (not forwarded)
    uint64_t sendErrors = 0;
};

class IpOut {
public:
    IpOut();
    ~IpOut();
    // false when the file cannot be opened or the address is not valid (error() says why); the parts that could be set up are on
    bool configure(const IpOutConfig& c);
    IpOutConfig config() const;
    bool active() const;                                   // something is switched on
    // one IP packet (IPv4 or IPv6); thread-safe; `unixSeconds` 0 = now
    void packet(const uint8_t* ip, size_t n, double unixSeconds = 0);
    IpOutStats stats() const;
    std::string error() const;
    void close();

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
