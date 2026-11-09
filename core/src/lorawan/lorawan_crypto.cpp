// LoRaWAN user-key decryption and beacons (lorawan_crypto.h).
#include "dect2/lorawan/lorawan_crypto.h"
#include "dect2/voicekeys/net_crypto.h"
#include <cstring>

namespace dect2 {
namespace lorawan {

static void block(uint8_t b[16], uint8_t head, bool up, uint32_t addr, uint32_t fcnt, uint8_t last) {
    std::memset(b, 0, 16);
    b[0] = head; b[5] = up ? 0 : 1;
    for (int i = 0; i < 4; i++) { b[6 + i] = (uint8_t)(addr >> (8 * i)); b[10 + i] = (uint8_t)(fcnt >> (8 * i)); }
    b[15] = last;
}

uint32_t dataMic(const uint8_t* p, size_t n, const uint8_t key[16], bool up, uint32_t addr, uint32_t fcnt) {
    std::vector<uint8_t> m(16 + n);
    block(m.data(), 0x49, up, addr, fcnt, (uint8_t)n);
    if (n) std::memcpy(m.data() + 16, p, n);
    uint8_t mac[16];
    voicekeys::cmac128(key, m.data(), m.size(), mac);
    return (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24;
}

std::vector<uint8_t> decryptFrm(const uint8_t* frm, size_t n, const uint8_t key[16], bool up, uint32_t addr, uint32_t fcnt) {
    std::vector<uint8_t> o(n);
    uint8_t a[16], s[16];
    for (size_t i = 0; i < n; i++) {
        if (i % 16 == 0) { block(a, 0x01, up, addr, fcnt, (uint8_t)(i / 16 + 1)); voicekeys::aes128Encrypt(key, a, s); }
        o[i] = frm[i] ^ s[i % 16];
    }
    return o;
}

uint16_t beaconCrc(const uint8_t* p, size_t n) {   // x^16+x^12+x^5+1, init 0, MSB first: the variant that reproduces the examples of 1.0.2 section 15.2 (0xC87E, 0x55DE)
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

Beacon parseBeacon(const uint8_t* p, size_t n) {
    Beacon b;
    if (n != 17) return b;
    int32_t la, lo;
    const uint16_t gw = (uint16_t)(p[15] | p[16] << 8);
    const uint16_t g2 = beaconCrc(p + 8, 7);
    if (gw != g2) return b;
    const uint16_t oldC = beaconCrc(p, 7), newC = beaconCrc(p, 6);
    if ((oldC & 0xFF) == p[7]) {            // 1.0.2: NetID 3, Time 4, CRC-8 (the low byte of the CRC-16)
        b.netId = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
        b.timeSec = (uint32_t)p[3] | (uint32_t)p[4] << 8 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 24;
        b.crc1Ok = true;
    } else if (newC == (uint16_t)(p[6] | p[7] << 8)) {
        b.newFormat = true;
        b.timeSec = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
        b.crc1Ok = true;
    } else return b;
    b.ok = true;
    const int gi = 8;
    b.infoDesc = p[gi];
    if (b.infoDesc <= 2) {
        la = (int32_t)((uint32_t)p[gi + 1] | (uint32_t)p[gi + 2] << 8 | (uint32_t)p[gi + 3] << 16);
        if (la & 0x800000) la -= 1 << 24;
        lo = (int32_t)((uint32_t)p[gi + 4] | (uint32_t)p[gi + 5] << 8 | (uint32_t)p[gi + 6] << 16);
        if (lo & 0x800000) lo -= 1 << 24;
        b.lat = la * 90.0 / 8388608.0;
        b.lon = lo * 180.0 / 8388608.0;
        b.hasGps = true;
    }
    return b;
}

} // namespace lorawan
} // namespace dect2
