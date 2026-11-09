// wfb-ng frame layout: svpcom/wfb-ng src/wifibroadcast.hpp (GPL-3). OpenHD: OpenHD/wifibroadcast Ieee80211Header.hpp (GPL-3). Layouts read and re-implemented.
#include "dect2/xlink/xlink_proto.h"
#include <cstring>

namespace dect2::xlink {

const char* protoName(int proto) {
    switch (proto) {
    case WfbNg: return "wfb-ng";
    case OpenHd: return "OpenHD";
    case Npr: return "NPR-70";
    default: return "?";
    }
}

WfbFrame parseWfb(const uint8_t* p, size_t n) {
    WfbFrame f;
    if (n < 24 + 9) return f;
    if (p[0] != 0x08 || (p[1] & 0x03) != 0x01) return f;   // a data frame, to the distribution system, as these links inject it
    static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (memcmp(p + 4, kBroadcast, 6) != 0) return f;
    const uint8_t* a2 = p + 10;
    const uint8_t* a3 = p + 16;
    const uint8_t* q = p + 24;
    const size_t m = n - 24;
    f.payload = m;
    if (a2[0] == 0x57 && a2[1] == 0x42 && memcmp(a2, a3, 6) == 0) {
        f.channelId = (uint32_t)a2[2] << 24 | (uint32_t)a2[3] << 16 | (uint32_t)a2[4] << 8 | a2[5];
        f.linkId = (int)(f.channelId >> 8);
        f.port = (int)(f.channelId & 0xFF);
        f.proto = WfbNg;
        if (q[0] == 1 && m >= 9 + 16) {
            uint64_t nonce = 0;
            for (int i = 0; i < 8; i++) nonce = nonce << 8 | q[1 + i];
            f.block = nonce >> 8;
            f.fragment = (int)(nonce & 0xFF);
            f.ok = true;
        } else if (q[0] == 2 && m >= 25 + 16) {
            f.session = true;
            f.ok = true;
        }
        return f;
    }
    // OpenHD: a one-byte air / ground id and the radio port frame the nonce in two MAC address fields
    if ((a2[0] == 1 || a2[0] == 2) && a3[0] == a2[0] && a2[5] == a3[5] && memcmp(a2 + 1, a3 + 1, 4) != 0) {
        f.proto = OpenHd;
        f.openHdPeer = a2[0];
        f.port = a2[5];
        uint64_t nonce = 0;
        for (int i = 3; i >= 0; i--) nonce = nonce << 8 | a3[1 + i];
        for (int i = 3; i >= 0; i--) nonce = nonce << 8 | a2[1 + i];
        f.nonce = nonce;
        f.ok = true;
    }
    return f;
}

} // namespace dect2::xlink
