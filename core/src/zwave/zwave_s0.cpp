// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave S0 (Security 0) with a user-imported network key: Ke / Ka derived from the network key with AES-ECB of 0xAA and 0x55 blocks, payload AES-OFB with
// IV = sender nonce + receiver nonce, MAC an AES CBC chain over IV, command, source, destination, length and ciphertext (first 8 bytes). Layout from the Grizzly S0 documentation
// and the arXiv Z-Wave security papers; S2 needs the session key handshake and is not decoded.
#include "dect2/zwave/zwave_proto.h"
#include "dect2/voicekeys/net_crypto.h"
#include <cstring>

namespace dect2 {
namespace zwave {

void S0Session::observe(const Frame& f) {
    if (f.payload.size() < 10 || f.payload[0] != 0x98 || f.payload[1] != 0x80) return;
    for (auto& r : rep_) if (r.src == f.src && r.dst == f.dst) { std::memcpy(r.nonce, f.payload.data() + 2, 8); return; }
    Rep r; r.src = f.src; r.dst = f.dst; std::memcpy(r.nonce, f.payload.data() + 2, 8);
    if (rep_.size() >= 64) rep_.erase(rep_.begin());
    rep_.push_back(r);
}

bool S0Session::decrypt(const Frame& f, const uint8_t key[16], std::vector<uint8_t>& plain) const {
    const auto& p = f.payload;
    if (p.size() < 2 + 8 + 1 + 1 + 8 || p[0] != 0x98 || (p[1] != 0x81 && p[1] != 0xC1)) return false;
    const size_t cipherLen = p.size() - 2 - 8 - 1 - 8;
    const uint8_t nonceId = p[p.size() - 9];
    const uint8_t* reported = nullptr;
    // the receiver of this frame reported the nonce: the report is from f.dst to f.src
    for (const auto& r : rep_) if (r.src == f.dst && r.dst == f.src && r.nonce[0] == nonceId) { reported = r.nonce; break; }
    if (!reported) return false;
    uint8_t aa[16], ab[16], ke[16], ka[16];
    std::memset(aa, 0xAA, 16); std::memset(ab, 0x55, 16);
    voicekeys::aes128Encrypt(key, aa, ke);
    voicekeys::aes128Encrypt(key, ab, ka);
    uint8_t iv[16];
    std::memcpy(iv, p.data() + 2, 8); std::memcpy(iv + 8, reported, 8);
    // MAC
    std::vector<uint8_t> auth;
    auth.insert(auth.end(), iv, iv + 16);
    auth.push_back(p[1]);
    auth.push_back((uint8_t)f.src);
    auth.push_back((uint8_t)f.dst);
    auth.push_back((uint8_t)cipherLen);
    auth.insert(auth.end(), p.begin() + 10, p.begin() + 10 + (long)cipherLen);
    while (auth.size() % 16) auth.push_back(0);
    uint8_t mac[16] = {};
    for (size_t i = 0; i < auth.size(); i += 16) {
        for (int k = 0; k < 16; k++) mac[k] ^= auth[i + (size_t)k];
        voicekeys::aes128Encrypt(ka, mac, mac);
    }
    if (std::memcmp(mac, p.data() + p.size() - 8, 8) != 0) return false;
    uint8_t ks[16];
    std::memcpy(ks, iv, 16);
    plain.clear();
    for (size_t i = 0; i < cipherLen; i++) {
        if (i % 16 == 0) voicekeys::aes128Encrypt(ke, ks, ks);
        plain.push_back(p[10 + i] ^ ks[i % 16]);
    }
    if (plain.empty()) return false;
    plain.erase(plain.begin());            // the sequence byte
    return true;
}

} // namespace zwave
} // namespace dect2
