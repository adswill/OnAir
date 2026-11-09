// Wi-SUN MAC security with a user-imported key: AES-CCM* as IEEE 802.15.4-2015 clause 9 (nonce = source EUI-64, frame counter, level; big endian).
#include "dect2/wisun/wisun_proto.h"
#include "dect2/voicekeys/net_crypto.h"

namespace dect2 {
namespace wisun {

bool decryptFrame(const uint8_t* d, size_t n, const Frame& f, const uint8_t key[16], std::vector<uint8_t>& plain) {
    if (!f.encrypted || f.secLevel < 1 || f.secLevel > 7 || f.srcMode != 3 || f.payloadAt > n) return false;
    static const int kMic[8] = {0, 4, 8, 16, 0, 4, 8, 16};
    const int mic = kMic[f.secLevel];
    if (n < f.payloadAt + (size_t)mic) return false;
    const size_t cipherLen = n - f.payloadAt - (size_t)mic;
    uint8_t nonce[13];
    for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)(f.srcExt >> (8 * (7 - i)));
    for (int i = 0; i < 4; i++) nonce[8 + i] = (uint8_t)(f.frameCounter >> (8 * (3 - i)));
    nonce[12] = (uint8_t)f.secLevel;
    if (f.secLevel < 4) return false;            // integrity only: nothing to decrypt
    return voicekeys::ccmStarDecrypt(key, nonce, d, f.payloadAt, d + f.payloadAt, cipherLen, mic, plain);
}

} // namespace wisun
} // namespace dect2
