// AES helpers against published vectors (RFC 4493, RFC 3610 packet vector), then each protocol's user-key decode on simulated frames with a wrong key that must be rejected.
#include "dect2/voicekeys/net_crypto.h"
#include "dect2/wisun/wisun_proto.h"
#include "dect2/wmbus/wmbus_proto.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zwave/zwave_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::voicekeys;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> hx(const char* s) { std::vector<uint8_t> v; for (size_t i = 0; s[i] && s[i + 1]; i += 2) { char b[3] = {s[i], s[i + 1], 0}; v.push_back((uint8_t)strtol(b, nullptr, 16)); } return v; }

static void vectors() {
    const auto k = hx("2b7e151628aed2a6abf7158809cf4f3c");
    uint8_t mac[16];
    cmac128(k.data(), nullptr, 0, mac);
    CHECK(!memcmp(mac, hx("bb1d6929e95937287fa37d129b756746").data(), 16));
    const auto m40 = hx("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411");
    cmac128(k.data(), m40.data(), m40.size(), mac);
    CHECK(!memcmp(mac, hx("dfa66747de9ae63030ca32611497c827").data(), 16));
    // RFC 3610 packet vector 1 has M = 8, L = 2 and a 13-byte nonce
    const auto kc = hx("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF"), n = hx("00000003020100A0A1A2A3A4A5");
    const auto aad = hx("0001020304050607"), pt = hx("08090A0B0C0D0E0F101112131415161718191A1B1C1D1E");
    const auto expect = hx("588C979A61C663D2F066D0C2C0F989806D5F6B61DAC38417E8D12CFDF926E0");
    auto ct = ccmStarEncrypt(kc.data(), n.data(), aad.data(), aad.size(), pt.data(), pt.size(), 8);
    CHECK(ct == expect);
    std::vector<uint8_t> out;
    CHECK(ccmStarDecrypt(kc.data(), n.data(), aad.data(), aad.size(), expect.data(), pt.size(), 8, out) && out == pt);
    auto bad = expect; bad[0] ^= 1;
    CHECK(!ccmStarDecrypt(kc.data(), n.data(), aad.data(), aad.size(), bad.data(), pt.size(), 8, out));
}

static void zigbeeTest() {
    uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0x10 + i);
    // MAC header (data, 16-bit addresses, PAN compress)
    std::vector<uint8_t> f = {0x41, 0x88, 0x05, 0x34, 0x12, 0xFF, 0xFF, 0x01, 0x00};
    // NWK header: fc data, proto 2, secured, dest 0x0002 src 0x0001, radius, seq
    std::vector<uint8_t> nwk = {0x08, 0x02, 0x02, 0x00, 0x01, 0x00, 0x1E, 0x07};
    nwk[1] |= 0x02;                                 // security bit (bit 9)
    const size_t auxAt = nwk.size();
    nwk.push_back(0x28);                            // sc: extended nonce, key id network, level 0 on air
    nwk.insert(nwk.end(), {0x44, 0x33, 0x22, 0x11});
    const uint8_t ext[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    nwk.insert(nwk.end(), ext, ext + 8);
    nwk.push_back(0x00);                            // key sequence
    std::vector<uint8_t> aad = nwk;
    aad[auxAt] = (uint8_t)((nwk[auxAt] & ~7) | 5);
    uint8_t nonce[13]; memcpy(nonce, ext, 8); memcpy(nonce + 8, nwk.data() + auxAt + 1, 4); nonce[12] = aad[auxAt];
    // APS data: unicast, dest ep 1, cluster 0x0006, profile 0x0104, src ep 1, counter
    const std::vector<uint8_t> aps = {0x40, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x05, 0x01, 0x00, 0x01};
    const auto ct = ccmStarEncrypt(key, nonce, aad.data(), aad.size(), aps.data(), aps.size(), 4);
    f.insert(f.end(), nwk.begin(), nwk.end());
    f.insert(f.end(), ct.begin(), ct.end());
    f.push_back(0); f.push_back(0);                 // the FCS, as parse() expects
    zigbee::Frame z = zigbee::parse(f.data(), f.size());
    CHECK(z.ok && z.nwkSecured && z.encrypted);
    std::vector<uint8_t> plain;
    uint8_t wrong[16] = {};
    { zigbee::Frame z2 = z; CHECK(!zigbee::decryptNwk(f.data(), f.size(), z2, wrong, plain)); CHECK(z2.encrypted); }
    CHECK(zigbee::decryptNwk(f.data(), f.size(), z, key, plain));
    CHECK(z.decrypted && !z.encrypted && z.hasAps && z.apsCluster == 6 && z.apsProfile == 0x0104);
    CHECK(z.info.find("Home Automation") != std::string::npos);
}

static void wisunTest() {
    uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0xA0 + i);
    wisun::Frame f;
    f.encrypted = true; f.secLevel = 6; f.srcMode = 3; f.srcExt = 0x0011223344556677ULL; f.frameCounter = 0x01020304;
    std::vector<uint8_t> hdr = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    f.payloadAt = hdr.size();
    uint8_t nonce[13] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 1, 2, 3, 4, 6};
    const std::vector<uint8_t> pl = {9, 8, 7, 6, 5};
    const auto ct = ccmStarEncrypt(key, nonce, hdr.data(), hdr.size(), pl.data(), pl.size(), 8);
    std::vector<uint8_t> m = hdr; m.insert(m.end(), ct.begin(), ct.end());
    std::vector<uint8_t> out;
    CHECK(wisun::decryptFrame(m.data(), m.size(), f, key, out) && out == pl);
    uint8_t wrong[16] = {1};
    CHECK(!wisun::decryptFrame(m.data(), m.size(), f, wrong, out));
}

static void zwaveS0() {
    uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 3);
    uint8_t aa[16], ab[16], ke[16], ka[16];
    memset(aa, 0xAA, 16); memset(ab, 0x55, 16);
    aes128Encrypt(key, aa, ke); aes128Encrypt(key, ab, ka);
    zwave::S0Session s;
    zwave::Frame rep; rep.src = 5; rep.dst = 1;
    rep.payload = {0x98, 0x80, 0x77, 2, 3, 4, 5, 6, 7, 8};
    s.observe(rep);
    const uint8_t sender[8] = {1, 1, 2, 3, 5, 8, 13, 21};
    uint8_t iv[16]; memcpy(iv, sender, 8); memcpy(iv + 8, rep.payload.data() + 2, 8);
    const std::vector<uint8_t> plain = {0x00, 0x25, 0x03, 0xFF};
    std::vector<uint8_t> ct(plain.size());
    uint8_t ks[16]; memcpy(ks, iv, 16);
    for (size_t i = 0; i < plain.size(); i++) { if (i % 16 == 0) aes128Encrypt(ke, ks, ks); ct[i] = plain[i] ^ ks[i % 16]; }
    std::vector<uint8_t> auth(iv, iv + 16);
    auth.push_back(0x81); auth.push_back(1); auth.push_back(5); auth.push_back((uint8_t)ct.size());
    auth.insert(auth.end(), ct.begin(), ct.end());
    while (auth.size() % 16) auth.push_back(0);
    uint8_t mac[16] = {};
    for (size_t i = 0; i < auth.size(); i += 16) { for (int k = 0; k < 16; k++) mac[k] ^= auth[i + (size_t)k]; aes128Encrypt(ka, mac, mac); }
    zwave::Frame f; f.src = 1; f.dst = 5;
    f.payload = {0x98, 0x81};
    f.payload.insert(f.payload.end(), sender, sender + 8);
    f.payload.insert(f.payload.end(), ct.begin(), ct.end());
    f.payload.push_back(0x77);
    f.payload.insert(f.payload.end(), mac, mac + 8);
    std::vector<uint8_t> out;
    CHECK(s.decrypt(f, key, out) && out == std::vector<uint8_t>({0x25, 0x03, 0xFF}));
    uint8_t wrong[16] = {9};
    CHECK(!s.decrypt(f, wrong, out));
    zwave::S0Session none;
    CHECK(!none.decrypt(f, key, out));   // the nonce was not seen
}

static void wmbusTest() {
    uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0x40 + i);
    // L C M2 A(id4 V T) CI(7A) ACC STATUS CFG(2) then 16 bytes encrypted: 2F 2F, volume DIF 04 VIF 13 value, padding
    std::vector<uint8_t> body = {0x00, 0x44, 0x4D, 0x4C, 0x10, 0x20, 0x30, 0x40, 0x07, 0x07, 0x7A, 0x55, 0x00, 0x10, 0x05};
    body[0] = 0;   // L is filled in below
    std::vector<uint8_t> plain = {0x2F, 0x2F, 0x04, 0x13, 0x10, 0x27, 0x00, 0x00, 0x2F, 0x2F, 0x2F, 0x2F, 0x2F, 0x2F, 0x2F, 0x2F};
    uint8_t iv[16] = {0x4D, 0x4C, 0x10, 0x20, 0x30, 0x40, 0x07, 0x07, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    // CBC encrypt by hand: E(iv ^ p)
    uint8_t x[16]; for (int i = 0; i < 16; i++) x[i] = iv[i] ^ plain[(size_t)i];
    aes128Encrypt(key, x, x);
    body.insert(body.end(), x, x + 16);
    body[0] = (uint8_t)(body.size() - 1);
    auto air = wmbus::buildFrame(std::vector<uint8_t>(body.begin() + 1, body.end()), false);
    wmbus::Frame f;
    CHECK(wmbus::parseFrame(air.data(), air.size(), false, f));
    CHECK(f.encrypted && f.config == 0x0510);
    uint8_t wrong[16] = {7};
    { wmbus::Frame g = f; CHECK(!wmbus::decryptFrame(g, wrong) && g.encrypted); }
    CHECK(wmbus::decryptFrame(f, key));
    CHECK(f.decrypted && !f.encrypted && !f.records.empty());
    if (!f.records.empty()) CHECK(f.records[0].quantity == "Volume");
}

int main() { vectors(); zigbeeTest(); wisunTest(); zwaveS0(); wmbusTest(); return testSummary("net_crypto"); }
