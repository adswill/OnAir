// Crypto helpers for the user-key decoders of Zigbee, Wi-SUN, Z-Wave S0, wM-Bus and LoRaWAN: AES-128 block helpers, CMAC (RFC 4493) and AES-CCM* (IEEE 802.15.4 Annex B / Zigbee).
// Only ever used with a key the user imported through the key store (voice_keys.h). Nothing here searches for keys.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::voicekeys {

void aes128Encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out);   // n a multiple of 16
void cmac128(const uint8_t key[16], const uint8_t* d, size_t n, uint8_t out[16]);

// AES-CCM* with a 13-byte nonce (L = 2), the MIC of micLen bytes (0, 4, 8 or 16), aad authenticated, cipherLen bytes of ciphertext followed by the MIC at cipherAndMic.
// Returns false when the MIC does not match (plain is then empty).
bool ccmStarDecrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aadLen, const uint8_t* cipherAndMic, size_t cipherLen, int micLen, std::vector<uint8_t>& plain);
// The inverse (test signals).
std::vector<uint8_t> ccmStarEncrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aadLen, const uint8_t* plain, size_t n, int micLen);

} // namespace dect2::voicekeys
