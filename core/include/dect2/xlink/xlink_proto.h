// Frame parsers of the extra-links family (family 600): frames already out of their PHY are turned into a row for the screens. Receive only; payloads that are encrypted
// stay encrypted, nothing is decrypted here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dect2::xlink {

enum Proto { WfbNg = 1, OpenHd = 2, Npr = 4, Winmor = 8, Mioty = 16 };
const char* protoName(int proto);

struct WfbFrame {
    bool ok = false;
    int proto = 0;              // WfbNg or OpenHd
    uint32_t channelId = 0;     // wfb-ng: (link id << 8) + radio port
    int linkId = 0, port = 0;
    bool session = false;       // wfb-ng: a session key packet (encrypted, its contents are not readable)
    uint64_t block = 0;         // wfb-ng: FEC block number
    int fragment = 0;           // wfb-ng: fragment index inside the FEC block
    int openHdPeer = 0;         // OpenHD: 1 air, 2 ground
    uint64_t nonce = 0;         // OpenHD: the packet nonce carried in the MAC address fields
    size_t payload = 0;         // bytes after the 802.11 header (ciphertext or encoded data)
};

// mpdu without FCS. Recognises the 802.11 data frames wfb-ng and OpenHD inject in monitor mode; anything else gives ok = false.
WfbFrame parseWfb(const uint8_t* mpdu, size_t n);

} // namespace dect2::xlink
