// Thread (the IPv6 mesh for smart homes) on top of IEEE 802.15.4, receive only: what a sniffer can show without the network key.
//
// Thread uses the 2.4 GHz O-QPSK PHY of IEEE 802.15.4 (channels 11 - 26, zigbee_proto.h) and 6LoWPAN (RFC 4944 / RFC 6282) above the MAC.
// Almost all Thread traffic is encrypted at the link layer (AES-CCM, MAC security level 5, key identifier mode 1, a key derived from the
// network master key): the 802.15.4 header, the frame counter and key index are in the clear and shown, the 6LoWPAN / IPv6 / MLE / CoAP
// content is NOT, and is reported as encrypted. What is in the clear, and decoded here:
//   - Thread beacons (protocol id 3: network name, extended PAN ID, version),
//   - frames sent without MAC security: MLE Discovery Request / Response (the Joiner scan), MAC commands such as the data request,
//   - for those, the 6LoWPAN headers: mesh header, fragment headers, IPHC (addresses given inline, the UDP ports, the next header) and what
//     the UDP port says (MLE 19788, CoAP 5683, Thread Management 61631, Joiner 1000), the MLE command and CoAP code where unencrypted.
// Nothing is decrypted, no key is asked for or recovered.
#pragma once
#include "dect2/zigbee/zigbee_proto.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace dect2 {
namespace thread {

struct Frame {
    bool beacon = false;                    // a Thread beacon payload
    std::string networkName;
    uint64_t extPan = 0;
    int version = -1;                       // Thread version from the beacon (1 = 1.1, 2 = 1.2 ...)
    bool encrypted = false;                 // MAC security level 4 or more: the 6LoWPAN content is not readable
    bool lowpan = false;                    // a 6LoWPAN dispatch was read
    bool mesh = false, frag = false, iphc = false;
    int meshHops = -1;
    int fragSize = 0, fragTag = 0, fragOffset = -1;
    bool udp = false;
    int srcPort = -1, dstPort = -1;
    int nextHeader = -1;                    // IPv6 next header when carried inline (17 UDP, 58 ICMPv6)
    std::string service;                    // "MLE", "CoAP", "Thread Management (CoAP)", "Joiner", "" when none
    bool mle = false, mleSecured = false;
    int mleCommand = -1;
    bool coap = false;
    std::string info;                       // one line: what the frame is
};

// Reads an 802.15.4 frame the Zigbee parser already took apart (zigbee::parse) for what Thread puts in it
Frame analyse(const zigbee::Frame& f, const uint8_t* psdu, size_t n);

const char* mleCommandName(int c);
const char* serviceOfPort(int port);        // "" when the port is not one of Thread's

constexpr uint16_t kMlePort = 19788, kCoapPort = 5683, kTmfPort = 61631, kJoinerPort = 1000;

} // namespace thread
} // namespace dect2
