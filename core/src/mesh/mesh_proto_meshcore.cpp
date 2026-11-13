// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MeshCore packets. Sources: meshcore-dev/MeshCore src/Packet.h and Packet.cpp (header byte, transport codes, path length byte
// with hash size in the top two bits, readFrom/writeTo), src/Mesh.cpp (ADVERT signature message, GRP_TXT/GRP_DATA handling,
// createAdvert, createGroupDatagram, TRACE layout), src/Utils.cpp (encrypt: AES-128-ECB zero padded; MAC = first 2 bytes of
// HMAC-SHA256 over the ciphertext with the 32 byte secret), src/helpers/BaseChatMesh.cpp (group text layout, channel hash =
// SHA-256 of the secret), src/helpers/AdvertDataHelpers.{h,cpp} (advert app data), docs/packet_format.md.
#include "mesh_proto_internal.h"
#include "dect2/mesh/mesh_crypto.h"
#include <cmath>
#include <cstring>
#include "dect2/common/endian.h"

namespace dect2 {

using namespace meshcrypto;

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putLe32(v, x); }

} // namespace

// Utils::MACThenDecrypt: HMAC key is the full 32 byte secret, AES key its first 16 bytes
bool meshcoreMacThenDecrypt(const MeshCoreChannel& c, const uint8_t* src, size_t n, std::vector<uint8_t>& plain) {
    if (n <= 2) return false;
    const size_t enc = n - 2;
    if (enc == 0 || enc % 16 != 0) return false;                       // Utils::MACThenDecrypt
    uint8_t mac[32];
    hmacSha256(c.secret, 32, src + 2, enc, mac);
    if (std::memcmp(mac, src, 2) != 0) return false;
    plain = aesEcbDecrypt(c.secret, 16, src + 2, enc);
    return !plain.empty();
}


// ---- builders ----

bool meshcoreNodeKeys(const MeshCoreNode& n, uint8_t seed[32], uint8_t pub[32]) {
    uint8_t in[8] = {'m', 'c', 't', 'e', 's', 't', n.seed, 0};
    sha256(in, 7, seed);
    ed25519PublicKey(seed, pub);
    return true;
}

std::vector<uint8_t> meshcoreAdvert(const MeshCoreNode& nd, uint32_t unixTime) {
    uint8_t seed[32], pub[32];
    meshcoreNodeKeys(nd, seed, pub);
    // AdvertDataBuilder::encodeTo
    std::vector<uint8_t> app;
    app.push_back((uint8_t)((nd.repeater ? 2 : 1) | 0x10 | (nd.name.empty() ? 0 : 0x80)));
    put32(app, (uint32_t)(int32_t)(nd.lat * 1e6));
    put32(app, (uint32_t)(int32_t)(nd.lon * 1e6));
    const size_t room = 32 - app.size();
    app.insert(app.end(), nd.name.begin(), nd.name.begin() + (nd.name.size() < room ? nd.name.size() : room));
    std::vector<uint8_t> msg(pub, pub + 32);
    put32(msg, unixTime);
    msg.insert(msg.end(), app.begin(), app.end());
    uint8_t sig[64];
    ed25519Sign(seed, msg.data(), msg.size(), sig);
    std::vector<uint8_t> out;
    out.push_back((uint8_t)((4 << 2) | 1));                // ADVERT, flood
    out.push_back(0);                                      // path length 0
    out.insert(out.end(), pub, pub + 32);
    put32(out, unixTime);
    out.insert(out.end(), sig, sig + 64);
    out.insert(out.end(), app.begin(), app.end());
    return out;
}

std::vector<uint8_t> meshcoreGroupText(const uint8_t secret16[16], const MeshCoreNode& from, uint32_t unixTime, const std::string& text,
                                       const std::vector<uint8_t>& path) {
    std::vector<uint8_t> plain;
    put32(plain, unixTime);
    plain.push_back(0);                                    // TXT_TYPE_PLAIN, attempt 0
    const std::string line = from.name + ": " + text;
    // BaseChatMesh::sendGroupMessage; keep the packet under 184 payload bytes (1 hash + 2 MAC + whole blocks)
    const size_t maxPlain = (184 - 3) / 16 * 16;
    plain.insert(plain.end(), line.begin(), line.end());
    if (plain.size() > maxPlain) plain.resize(maxPlain);
    uint8_t key32[32] = {};
    std::memcpy(key32, secret16, 16);
    const std::vector<uint8_t> ct = aesEcbEncrypt(key32, 16, plain.data(), plain.size());
    uint8_t mac[32];
    hmacSha256(key32, 32, ct.data(), ct.size(), mac);
    uint8_t h[32];
    sha256(secret16, 16, h);
    std::vector<uint8_t> out;
    out.push_back((uint8_t)((5 << 2) | 1));                // GRP_TXT, flood
    out.push_back((uint8_t)(path.size() & 63));
    out.insert(out.end(), path.begin(), path.begin() + (path.size() < 63 ? path.size() : 63));
    out.push_back(h[0]);
    out.push_back(mac[0]);
    out.push_back(mac[1]);
    out.insert(out.end(), ct.begin(), ct.end());
    return out;
}

std::vector<uint8_t> meshcorePublicText(const MeshCoreNode& from, uint32_t unixTime, const std::string& text, const std::vector<uint8_t>& path) {
    return meshcoreGroupText(meshcorePublicSecret(), from, unixTime, text, path);
}

std::vector<uint8_t> meshcoreAck(uint32_t crc, const std::vector<uint8_t>& path) {
    std::vector<uint8_t> out;
    out.push_back((uint8_t)((3 << 2) | 1));
    out.push_back((uint8_t)(path.size() & 63));
    out.insert(out.end(), path.begin(), path.begin() + (path.size() < 63 ? path.size() : 63));
    put32(out, crc);
    return out;
}

} // namespace dect2
