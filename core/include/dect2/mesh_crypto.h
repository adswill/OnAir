// Crypto for the Meshtastic and MeshCore packet layers, written for this project: AES-128/256 (ECB, CTR), SHA-256,
// HMAC-SHA256, SHA-512 and Ed25519 (MeshCore adverts are signed). Checked against FIPS 197, SP 800-38A, RFC 3686,
// FIPS 180-4, RFC 4231 and RFC 8032 vectors in tests/test_mesh_crypto_kat.cpp. Not hardened against timing attacks:
// it only handles public channel keys and test keys.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace meshcrypto {

// AES with a 16 or 32 byte key (FIPS 197)
class Aes {
public:
    bool setKey(const uint8_t* key, size_t len);                 // false: len is not 16 or 32
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const;
private:
    uint8_t rk_[15 * 16] = {};
    int rounds_ = 0;
};

// ECB: in/out may be the same buffer. Encrypt zero-pads the last partial block (as MeshCore does); decrypt ignores a partial tail.
std::vector<uint8_t> aesEcbEncrypt(const uint8_t* key, size_t keyLen, const uint8_t* in, size_t n);
std::vector<uint8_t> aesEcbDecrypt(const uint8_t* key, size_t keyLen, const uint8_t* in, size_t n);
// CTR (SP 800-38A F.5): the 16 byte counter block is incremented as a big-endian number. Encrypt and decrypt are the same.
bool aesCtr(const uint8_t* key, size_t keyLen, const uint8_t iv[16], uint8_t* data, size_t n);

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const uint8_t* p, size_t n);
    void final(uint8_t out[32]);
private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t total_ = 0;
    size_t fill_ = 0;
};
void sha256(const uint8_t* p, size_t n, uint8_t out[32]);
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t n, uint8_t out[32]);

void sha512(const uint8_t* p, size_t n, uint8_t out[64]);

// Ed25519 (RFC 8032, pure). The seed is the 32 byte private key.
void ed25519PublicKey(const uint8_t seed[32], uint8_t pub[32]);
void ed25519Sign(const uint8_t seed[32], const uint8_t* msg, size_t n, uint8_t sig[64]);
bool ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pub[32]);

// text helpers
bool base64Decode(const std::string& s, std::vector<uint8_t>& out);   // standard alphabet, padding optional, spaces ignored
std::string base64Encode(const uint8_t* p, size_t n);
bool hexDecode(const std::string& s, std::vector<uint8_t>& out);      // spaces and ':' ignored
std::string hexEncode(const uint8_t* p, size_t n, bool upper = false);

} // namespace meshcrypto
} // namespace dect2
