// MeshCore packet layer: header, path, adverts (Ed25519 signature), group text on the Public channel and user channels.
// Sources: meshcore-dev/MeshCore src/Packet.h, Packet.cpp, Mesh.cpp, Utils.cpp, helpers/BaseChatMesh.cpp, helpers/AdvertDataHelpers,
// examples/companion_radio/MyMesh.cpp (PUBLIC_GROUP_PSK "izOH6cXN6mrJ5e26oRXNcg=="), docs/packet_format.md.
// The group text vector (GRP_TXT, Public channel, path ab, "Dubai Repeater: hello mesh") was computed independently with
// Python hashlib/hmac and OpenSSL 3.6.4 (aes-128-ecb) following Utils::encryptThenMAC, not with this code.
#include "dect2/mesh_proto.h"
#include "dect2/mesh_crypto.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static std::vector<uint8_t> H(const char* s) { std::vector<uint8_t> v; meshcrypto::hexDecode(s, v); return v; }
static std::string hex(const std::vector<uint8_t>& v) { return meshcrypto::hexEncode(v.data(), v.size()); }
static const MeshRadioInfo kRadio{};

int main() {
    MeshProto mp;
    MeshCoreNode rep; rep.seed = 7; rep.name = "Dubai Repeater"; rep.lat = 25.2048; rep.lon = 55.2708; rep.repeater = true;
    MeshCoreNode chat; chat.seed = 9; chat.name = "Sara"; chat.lat = 25.0805; chat.lon = -55.1403;

    { // Public channel: the key from the source, its hash, and the independent vector
        std::vector<uint8_t> k;
        meshcrypto::base64Decode("izOH6cXN6mrJ5e26oRXNcg==", k);
        CHECK(k.size() == 16 && std::memcmp(k.data(), meshcorePublicSecret(), 16) == 0, "public key bytes");
        uint8_t h[32]; meshcrypto::sha256(k.data(), 16, h);
        CHECK(h[0] == 0x11, "public channel hash %02x", h[0]);
        const auto want = H("1501ab11f920a61f33a7906d3a9dcd1751d8c3318196e47c7d5dbda404b869f4b2ed4e459842");
        const auto got = meshcorePublicText(rep, 1760000000, "hello mesh", {0xab});
        CHECK(got == want, "group text bytes %s", hex(got).c_str());
        auto r = mp.decodeMeshCore(want.data(), want.size(), kRadio);
        CHECK(r.ok && r.packet.decrypted && r.packet.type == "GRP_TXT" && r.packet.channel == "Public" && r.packet.channelHash == 0x11, "decode %s", r.packet.note.c_str());
        CHECK(r.packet.routeType == 1 && r.packet.payloadType == 5 && r.packet.payloadVersion == 0 && r.packet.pathHashCount == 1 && r.packet.path == "ab", "header fields");
        CHECK(r.messages.size() == 1 && r.messages[0].from == "Dubai Repeater" && r.messages[0].text == "hello mesh" && r.messages[0].channel == "Public" &&
              r.messages[0].hops == 1 && r.messages[0].senderTime == 1760000000u, "message");
    }
    { // advert layout (docs/packet_format.md, Mesh.cpp createAdvert) and decoding
        const auto p = meshcoreAdvert(rep, 1760000000);
        CHECK(p.size() > 102 && p[0] == 0x11 && p[1] == 0, "advert header %02x %02x", p[0], p[1]);
        uint8_t seed[32], pub[32];
        meshcoreNodeKeys(rep, seed, pub);
        CHECK(std::memcmp(&p[2], pub, 32) == 0, "public key at offset 2");
        CHECK(p[34] == 0x00 && p[35] == 0x78 && p[36] == 0xe7 && p[37] == 0x68, "timestamp 1760000000 little endian");
        CHECK(p[102] == (0x02 | 0x10 | 0x80), "app data flags %02x", p[102]);
        auto r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.decrypted && r.packet.type == "ADVERT" && r.nodes.size() == 1, "advert decode: %s", r.packet.note.c_str());
        if (r.nodes.size() == 1) {
            const auto& n = r.nodes[0];
            CHECK(n.longName == "Dubai Repeater" && n.role == "Repeater" && n.hasPosition && std::abs(n.lat - 25.2048) < 1e-6 && std::abs(n.lon - 55.2708) < 1e-6, "advert fields");
            CHECK(n.signatureChecked && n.signatureOk && n.publicKey == meshcrypto::hexEncode(pub, 32) && n.nodeId == n.publicKey.substr(0, 12), "key and signature");
            CHECK(r.packet.from == n.nodeId, "from = key prefix");
        }
        // a flipped name bit invalidates the signature: no node, like the firmware
        auto bad = p; bad[bad.size() - 1] ^= 1;
        r = mp.decodeMeshCore(bad.data(), bad.size(), kRadio);
        CHECK(r.ok && r.nodes.empty() && r.packet.note == "signature not valid", "forged advert: %s", r.packet.note.c_str());
        // chat node, west longitude negative
        const auto c = meshcoreAdvert(chat, 1760000100);
        r = mp.decodeMeshCore(c.data(), c.size(), kRadio);
        CHECK(r.nodes.size() == 1 && r.nodes[0].role == "Chat" && std::abs(r.nodes[0].lon + 55.1403) < 1e-6 && r.nodes[0].signatureOk, "chat advert");
        // truncated advert
        for (size_t n = 2; n < 102; n += 7) { r = mp.decodeMeshCore(p.data(), n, kRadio); CHECK(r.nodes.empty(), "truncated advert %zu", n); }
        // an advert with a 2 byte path hash (path length byte 0x41 = 2 hashes of 2 bytes)
        std::vector<uint8_t> w = {0x11, 0x42};
        w.insert(w.end(), {0xaa, 0xbb, 0xcc, 0xdd});
        w.insert(w.end(), p.begin() + 2, p.end());
        r = mp.decodeMeshCore(w.data(), w.size(), kRadio);
        CHECK(r.ok && r.nodes.size() == 1 && r.packet.pathHashSize == 2 && r.packet.pathHashCount == 2 && r.packet.path == "aabb,ccdd", "2 byte path hashes: %s", r.packet.path.c_str());
        // transport flood: header 0x10 carries 4 bytes of transport codes before the path length
        std::vector<uint8_t> t = {0x10, 0x01, 0x02, 0x03, 0x04, 0x00};
        t.insert(t.end(), p.begin() + 2, p.end());
        r = mp.decodeMeshCore(t.data(), t.size(), kRadio);
        CHECK(r.ok && r.nodes.size() == 1 && r.packet.routeType == 0, "transport codes skipped");
    }
    { // user channels: hashtag convention, hex and base64 secrets, a 32 byte secret
        std::vector<uint8_t> hs(32);
        meshcrypto::sha256((const uint8_t*)"#test", 5, hs.data());
        CHECK(meshcrypto::hexEncode(hs.data(), 16) == "9cd8fcf22a47333b591d96a2b848b73f", "hashtag secret (Python hashlib)");
        auto pkt = meshcoreGroupText(hs.data(), chat, 1760000200, "channel talk", {});
        auto r = mp.decodeMeshCore(pkt.data(), pkt.size(), kRadio);
        CHECK(r.ok && !r.packet.decrypted && r.packet.note.find("unknown channel") != std::string::npos, "unknown: %s", r.packet.note.c_str());
        CHECK(mp.addMeshCoreChannel("#test", ""), "add hashtag");
        r = mp.decodeMeshCore(pkt.data(), pkt.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "#test" && r.messages.size() == 1 && r.messages[0].text == "channel talk" && r.messages[0].from == "Sara" && r.messages[0].hops == 0, "hashtag channel decoded");
        const auto s16 = H("000102030405060708090a0b0c0d0e0f");
        pkt = meshcoreGroupText(s16.data(), chat, 1760000300, "hex key", {1, 2, 3});
        CHECK(mp.addMeshCoreChannel("hexchan", "00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f"), "add hex");
        r = mp.decodeMeshCore(pkt.data(), pkt.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "hexchan" && r.messages[0].hops == 3, "hex channel");
        const auto s16b = H("ffeeddccbbaa99887766554433221100");
        pkt = meshcoreGroupText(s16b.data(), chat, 1760000300, "b64 key", {});
        CHECK(mp.addMeshCoreChannel("b64chan", meshcrypto::base64Encode(s16b.data(), 16)), "add base64");
        r = mp.decodeMeshCore(pkt.data(), pkt.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "b64chan", "base64 channel");
        // 32 byte secret: hash over 32 bytes, HMAC key the 32 bytes, AES key the first 16 (Utils.cpp, BaseChatMesh::setChannel)
        std::vector<uint8_t> s32(32);
        for (int i = 0; i < 32; i++) s32[i] = (uint8_t)(0xa0 + i);
        std::vector<uint8_t> plain = H("00e1f505" "00");
        const std::string line = "Eve: long key";
        plain.insert(plain.end(), line.begin(), line.end());
        auto ct = meshcrypto::aesEcbEncrypt(s32.data(), 16, plain.data(), plain.size());
        uint8_t mac[32], hh[32];
        meshcrypto::hmacSha256(s32.data(), 32, ct.data(), ct.size(), mac);
        meshcrypto::sha256(s32.data(), 32, hh);
        std::vector<uint8_t> w = {0x15, 0x00, hh[0], mac[0], mac[1]};
        w.insert(w.end(), ct.begin(), ct.end());
        CHECK(mp.addMeshCoreChannel("big", meshcrypto::hexEncode(s32.data(), 32)), "add 32 byte");
        r = mp.decodeMeshCore(w.data(), w.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "big" && r.messages.size() == 1 && r.messages[0].from == "Eve" && r.messages[0].text == "long key", "32 byte secret");
        CHECK(!mp.addMeshCoreChannel("bad", "0011"), "short secret");
        CHECK(!mp.addMeshCoreChannel("bad", "***"), "junk secret");
        // damaged ciphertext: MAC mismatch on a known channel
        w[10] ^= 1;
        r = mp.decodeMeshCore(w.data(), w.size(), kRadio);
        CHECK(r.ok && !r.packet.decrypted && r.packet.note.find("MAC mismatch") != std::string::npos && r.messages.empty(), "MAC mismatch: %s", r.packet.note.c_str());
        mp.clearUserChannels();
        auto names = mp.channelNames(MeshProtocol::MeshCore);
        CHECK(names.size() == 1 && names[0] == "Public", "only Public remains");
    }
    { // long text is cut to fit the 184 byte payload limit, and the packet still decodes
        const std::string big(400, 'x');
        auto p = meshcorePublicText(chat, 1760000400, big, {});
        CHECK(p.size() <= 255 && p.size() - 2 <= 184, "size %zu", p.size());
        auto r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.messages.size() == 1 && r.messages[0].text.size() > 100 && r.messages[0].text.size() < 400, "cut text %zu", r.messages.empty() ? 0 : r.messages[0].text.size());
    }
    { // packets without keys: header fields only
        // TXT_MSG flood (type 2): dest hash 0x5a, src hash 0xc3, MAC, 16 bytes of ciphertext
        std::vector<uint8_t> p = {0x09, 0x00, 0x5a, 0xc3, 0x12, 0x34};
        for (int i = 0; i < 16; i++) p.push_back((uint8_t)i);
        auto r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.ok && !r.packet.decrypted && r.packet.type == "TXT_MSG" && r.packet.to == "5a" && r.packet.from == "c3" && r.packet.note == "encrypted (direct)", "txt msg");
        p[0] = 0x0a;                                       // type 2, direct
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.packet.routeType == 2 && r.packet.type == "TXT_MSG", "direct route");
        // ACK
        p = {0x0d, 0x00, 0x78, 0x56, 0x34, 0x12};
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.type == "ACK" && r.packet.detail.find("12345678") != std::string::npos, "ack: %s", r.packet.detail.c_str());
        p = meshcoreAck(0xdeadbeef, {0x01});
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.packet.type == "ACK" && r.packet.detail.find("deadbeef") != std::string::npos && r.packet.pathHashCount == 1, "ack builder");
        // REQ, ANON_REQ
        p = {0x01, 0x00, 0x5a, 0xc3, 0x00, 0x00, 0x01, 0x02};
        p[0] = (0 << 2) | 1;
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.packet.type == "REQ" && r.packet.note == "encrypted (direct)", "req");
        p = {(7 << 2) | 1, 0x00, 0x5a};
        for (int i = 0; i < 32; i++) p.push_back((uint8_t)(0x40 + i));
        p.insert(p.end(), {0x00, 0x00, 0x01});
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.packet.type == "ANON_REQ" && r.packet.to == "5a" && r.packet.from == "404142434445", "anon req %s", r.packet.from.c_str());
        // TRACE (direct) with two SNR entries in the path field (quarter dB), tag, auth, flags 0, three hashes to visit
        p = {(9 << 2) | 2, 0x02, 0x14, 0xf8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0xa1, 0xb2, 0xc3};
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.type == "TRACE" && r.packet.detail.find("3 nodes") != std::string::npos && r.packet.detail.find("5.00") != std::string::npos && r.packet.detail.find("-2.00") != std::string::npos, "trace: %s", r.packet.detail.c_str());
        // GRP_DATA on Public with a broken MAC, PATH, CONTROL, MULTIPART, RAW_CUSTOM, unknown type 12
        p = {(6 << 2) | 1, 0x00, 0x11, 0x00, 0x00, 0x01, 0x02, 0x03};
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(r.packet.type == "GRP_DATA" && r.packet.note.find("MAC mismatch") != std::string::npos, "grp data mac");
        const int types[] = {8, 10, 11, 15, 12};
        const char* names[] = {"PATH", "MULTIPART", "CONTROL", "RAW_CUSTOM", "TYPE_12"};
        for (int i = 0; i < 5; i++) {
            p = {(uint8_t)((types[i] << 2) | 1), 0x00, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05};
            r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
            CHECK(r.ok && r.packet.type == names[i], "type %s vs %s", r.packet.type.c_str(), names[i]);
        }
    }
    { // malformed
        MeshCoreNode x = chat;
        const auto good = meshcorePublicText(x, 1, "hi", {});
        for (size_t n = 0; n < good.size(); n++) { auto r = mp.decodeMeshCore(good.data(), n, kRadio); (void)r; }
        auto r = mp.decodeMeshCore(nullptr, 0, kRadio);
        CHECK(!r.ok, "null");
        std::vector<uint8_t> p = {0x15, 0xc0, 1, 2, 3};                // path length byte with the reserved hash size
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(!r.ok && r.packet.note == "bad path length byte", "reserved path size");
        p = {0x15, 0x3f, 1, 2, 3};                                     // 63 hashes do not fit in the packet
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(!r.ok, "path longer than the packet");
        p = {0x15, 0x00};
        r = mp.decodeMeshCore(p.data(), p.size(), kRadio);
        CHECK(!r.ok, "no payload");
        std::mt19937 rng(777);
        for (int i = 0; i < 200000; i++) {
            std::vector<uint8_t> q(1 + rng() % 200);
            for (auto& v : q) v = (uint8_t)rng();
            mp.decodeMeshCore(q.data(), q.size(), kRadio);
        }
    }

    if (fails) return 1;
    printf("mesh_proto_meshcore ok\n");
    return 0;
}
