// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Standard cipher primitives for user-supplied digital-voice keys, written for this project from public specifications only.
// Nothing here searches for, recovers or brute-forces a key: every function takes a key the user already holds and either produces a
// keystream or decrypts one block. These are the building blocks the per-protocol glue in voice_decrypt.h uses.
//
//   RC4       : Rivest's stream cipher (the "alleged RC4" / ARCFOUR as described publicly). Used by P25 ADP and DMR enhanced-privacy-style.
//   DES       : FIPS 46-3, single DES, one 64-bit block (56 effective key bits). Used by P25 DES-OFB and the NXDN/standard DES option.
//   OFB       : SP 800-38A output feedback, turns a block cipher into a keystream generator (P25 DES-OFB and AES-256-OFB).
//   LFSR15    : the 15-bit scrambler (x^15 + x^14 + 1) seeded by the user's key, as the NXDN and dPMR scramblers are publicly described.
//
// AES itself is reused from dect2::meshcrypto::Aes (FIPS 197), so there is one audited AES in the build.
// Not hardened against timing attacks: these only process a small amount of voice with a key the user chose to load.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace voicekeys {

// ---- RC4 / ARCFOUR (public description) -------------------------------------------------------------
// keystream of n bytes from a key of keyLen bytes. Decryption is XOR of the keystream with the ciphertext.
class Rc4 {
public:
    void setKey(const uint8_t* key, size_t keyLen);
    void discard(size_t n);                       // drop n keystream bytes (some profiles drop a prefix)
    uint8_t next();                               // one keystream byte
    void keystream(uint8_t* out, size_t n);
private:
    uint8_t s_[256] = {};
    int i_ = 0, j_ = 0;
};

// ---- single DES (FIPS 46-3) -------------------------------------------------------------------------
class Des {
public:
    bool setKey(const uint8_t key[8]);            // the 8 key bytes (parity bits ignored); false only if key is null
    void encryptBlock(const uint8_t in[8], uint8_t out[8]) const;
    void decryptBlock(const uint8_t in[8], uint8_t out[8]) const;
private:
    uint64_t sub_[16] = {};                       // the 16 round subkeys (48 bits each)
    bool ready_ = false;
};

// ---- OFB keystream (SP 800-38A) ---------------------------------------------------------------------
// Generate n bytes of OFB keystream from a block cipher. BlockFn encrypts one block in place-ish: out = E(in).
// blockBytes is 8 (DES) or 16 (AES). iv is blockBytes long. These wrappers avoid duplicating the feedback loop.
void desOfbKeystream(const Des& des, const uint8_t* iv, uint8_t* out, size_t n);
void aesOfbKeystream(const uint8_t* key, size_t keyLen, const uint8_t iv[16], uint8_t* out, size_t n);   // keyLen 16 or 32

// ---- scrambler (15-bit LFSR) ------------------------------------------------------------------------
// The scrambler used by NXDN and dPMR as publicly described: a 15-bit LFSR (polynomial x^15 + x^14 + 1) loaded with a non-zero
// 15-bit seed (the user's key), one output bit per shift. keystreamBits fills n bits (one per byte, value 0/1).
class Lfsr15 {
public:
    void setSeed(uint16_t seed15);                // low 15 bits; a zero seed is forced to 1 so it never locks up
    uint8_t nextBit();
    void keystreamBits(uint8_t* bits, size_t n);
private:
    uint16_t st_ = 1;
};

// XOR a keystream (one bit per byte, 0/1) into a bit buffer in place (both one bit per byte).
void xorBits(uint8_t* data, const uint8_t* ks, size_t n);
// XOR a byte keystream into data in place.
void xorBytes(uint8_t* data, const uint8_t* ks, size_t n);

} // namespace voicekeys
} // namespace dect2
