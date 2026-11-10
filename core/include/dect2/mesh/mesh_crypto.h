// Crypto for the Meshtastic and MeshCore packet layers, written for this project: AES-128/256 (ECB, CTR), SHA-256,
// HMAC-SHA256, SHA-512 and Ed25519 (MeshCore adverts are signed). Checked against FIPS 197, SP 800-38A, RFC 3686,
// FIPS 180-4, RFC 4231 and RFC 8032 vectors in tests/mesh/test_mesh_crypto_kat.cpp. Not hardened against timing attacks except the
// key exchange ladder; the MeshCore node (meshcore_node.h) signs adverts with the user's own key on the user's own computer.
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
// The 64 byte private key of orlp/ed25519 (MeshCore's LocalIdentity): SHA-512 of the seed, first half clamped. The public key is the
// first half times the base point (taken as stored, as ed25519_derive_pub does); signing hashes the second half with the message.
void ed25519ExpandSeed(const uint8_t seed[32], uint8_t prv[64]);
void ed25519PublicFromPrivate(const uint8_t prv[64], uint8_t pub[32]);
void ed25519SignPrivate(const uint8_t prv[64], const uint8_t pub[32], const uint8_t* msg, size_t n, uint8_t sig[64]);
// orlp ed25519_key_exchange (MeshCore's shared secret of two identities): the other Ed25519 public key turned into its Montgomery form,
// then X25519 with the private key's first half. The ladder does not branch on the key; the signing above is variable-time.
void ed25519KeyExchange(uint8_t shared[32], const uint8_t otherPub[32], const uint8_t prv[64]);

// text helpers
bool base64Decode(const std::string& s, std::vector<uint8_t>& out);   // standard alphabet, padding optional, spaces ignored
std::string base64Encode(const uint8_t* p, size_t n);
bool hexDecode(const std::string& s, std::vector<uint8_t>& out);      // spaces and ':' ignored
std::string hexEncode(const uint8_t* p, size_t n, bool upper = false);

} // namespace meshcrypto
} // namespace dect2
