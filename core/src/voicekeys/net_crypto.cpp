// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AES-128 helpers, CMAC and CCM* (net_crypto.h).
#include "dect2/voicekeys/net_crypto.h"
#include "../tv/iptv_aes.h"
#include <cstring>

namespace dect2::voicekeys {

void aes128Encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) { iptv::Aes128 a(key); a.encryptBlock(in, out); }

void aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out) {
    iptv::Aes128 a(key);
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        uint8_t t[16];
        a.decryptBlock(in + i, t);
        for (int k = 0; k < 16; k++) out[i + (size_t)k] = t[k] ^ prev[k];
        std::memcpy(prev, in + i, 16);
    }
}

static void dbl(const uint8_t in[16], uint8_t out[16]) {
    const int carry = in[0] >> 7;
    for (int i = 0; i < 15; i++) out[i] = (uint8_t)(in[i] << 1 | in[i + 1] >> 7);
    out[15] = (uint8_t)((in[15] << 1) ^ (carry ? 0x87 : 0));
}

void cmac128(const uint8_t key[16], const uint8_t* d, size_t n, uint8_t out[16]) {
    iptv::Aes128 a(key);
    uint8_t l[16] = {}, k1[16], k2[16];
    { uint8_t z[16] = {}; a.encryptBlock(z, l); }
    dbl(l, k1); dbl(k1, k2);
    const size_t blocks = n == 0 ? 1 : (n + 15) / 16;
    const bool full = n != 0 && n % 16 == 0;
    uint8_t x[16] = {};
    for (size_t b = 0; b + 1 < blocks; b++) {
        for (int i = 0; i < 16; i++) x[i] ^= d[b * 16 + (size_t)i];
        a.encryptBlock(x, x);
    }
    uint8_t last[16] = {};
    const size_t rem = n - (blocks - 1) * 16;
    if (rem) std::memcpy(last, d + (blocks - 1) * 16, rem);
    if (full) for (int i = 0; i < 16; i++) last[i] ^= k1[i];
    else { last[rem] = 0x80; for (int i = 0; i < 16; i++) last[i] ^= k2[i]; }
    for (int i = 0; i < 16; i++) x[i] ^= last[i];
    a.encryptBlock(x, out);
}

namespace {
void blockB0(uint8_t b0[16], const uint8_t nonce[13], size_t aadLen, size_t n, int micLen) {
    b0[0] = (uint8_t)(((aadLen > 0) ? 0x40 : 0) | ((micLen ? (micLen - 2) / 2 : 0) << 3) | 1);   // L' = 1
    std::memcpy(b0 + 1, nonce, 13);
    b0[14] = (uint8_t)(n >> 8); b0[15] = (uint8_t)n;
}
void cbcUpdate(iptv::Aes128& a, uint8_t x[16], const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i += 16) {
        for (size_t k = 0; k < 16; k++) if (i + k < n) x[k] ^= d[i + k];
        a.encryptBlock(x, x);
    }
}
void mic(iptv::Aes128& a, const uint8_t nonce[13], const uint8_t* aad, size_t aadLen, const uint8_t* m, size_t n, int micLen, uint8_t out[16]) {
    uint8_t b0[16]; blockB0(b0, nonce, aadLen, n, micLen);
    uint8_t x[16];
    a.encryptBlock(b0, x);
    if (aadLen) {
        std::vector<uint8_t> h;
        h.push_back((uint8_t)(aadLen >> 8)); h.push_back((uint8_t)aadLen);
        h.insert(h.end(), aad, aad + aadLen);
        cbcUpdate(a, x, h.data(), h.size());
    }
    if (n) cbcUpdate(a, x, m, n);
    std::memcpy(out, x, 16);
}
void ctr(iptv::Aes128& a, const uint8_t nonce[13], uint16_t cnt, uint8_t out[16]) {
    uint8_t c[16];
    c[0] = 1;
    std::memcpy(c + 1, nonce, 13);
    c[14] = (uint8_t)(cnt >> 8); c[15] = (uint8_t)cnt;
    a.encryptBlock(c, out);
}
} // namespace

bool ccmStarDecrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aadLen, const uint8_t* cm, size_t n, int micLen, std::vector<uint8_t>& plain) {
    iptv::Aes128 a(key);
    plain.assign(n, 0);
    uint8_t ks[16];
    for (size_t i = 0; i < n; i++) {
        if (i % 16 == 0) ctr(a, nonce, (uint16_t)(i / 16 + 1), ks);
        plain[i] = cm[i] ^ ks[i % 16];
    }
    if (micLen == 0) return true;
    uint8_t m[16], s0[16];
    mic(a, nonce, aad, aadLen, plain.data(), n, micLen, m);
    ctr(a, nonce, 0, s0);
    for (int i = 0; i < micLen; i++) if ((m[i] ^ s0[i]) != cm[n + (size_t)i]) { plain.clear(); return false; }
    return true;
}

std::vector<uint8_t> ccmStarEncrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, size_t aadLen, const uint8_t* plain, size_t n, int micLen) {
    iptv::Aes128 a(key);
    std::vector<uint8_t> out(n + (size_t)micLen);
    uint8_t ks[16];
    for (size_t i = 0; i < n; i++) {
        if (i % 16 == 0) ctr(a, nonce, (uint16_t)(i / 16 + 1), ks);
        out[i] = plain[i] ^ ks[i % 16];
    }
    if (micLen) {
        uint8_t m[16], s0[16];
        mic(a, nonce, aad, aadLen, plain, n, micLen, m);
        ctr(a, nonce, 0, s0);
        for (int i = 0; i < micLen; i++) out[n + (size_t)i] = m[i] ^ s0[i];
    }
    return out;
}

} // namespace dect2::voicekeys
