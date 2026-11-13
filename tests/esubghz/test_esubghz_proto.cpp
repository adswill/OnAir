// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat protocol layer: the CC1101 CRC, packet layout, message split, keys, AES-256-GCM (the GCM specification's
// AES-256 test cases 13, 14 and 16), encrypted and plain messages, the replay guard.
#include "dect2/esubghz/esubghz_proto.h"
#include "dect2/mesh/mesh_crypto.h"
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::esubghz;
#include "../common/test_util.h"

static std::vector<uint8_t> hx(const char* s) { std::vector<uint8_t> v; meshcrypto::hexDecode(s, v); return v; }
static std::string toHex(const uint8_t* p, size_t n) { return meshcrypto::hexEncode(p, n); }

static void gcmVectors() {
    struct V { const char *k, *iv, *p, *a, *c, *t; };
    const V vs[] = {
        {"0000000000000000000000000000000000000000000000000000000000000000", "000000000000000000000000", "", "", "", "530f8afbc74536b9a963b4f1c4cb738b"},
        {"0000000000000000000000000000000000000000000000000000000000000000", "000000000000000000000000", "00000000000000000000000000000000", "",
         "cea7403d4d606b6e074ec5d3baf39d18", "d0d1c8a799996bf0265b98b5d48ab919"},
        {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
         "feedfacedeadbeeffeedfacedeadbeefabaddad2",
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
         "76fc6ece0f4e1768cddf8853bb2d551b"},
    };
    for (const V& v : vs) {
        const auto k = hx(v.k), iv = hx(v.iv), p = hx(v.p), a = hx(v.a), c = hx(v.c), t = hx(v.t);
        std::vector<uint8_t> out(p.size()), back(p.size());
        uint8_t tag[16];
        gcmEncrypt(k.data(), iv.data(), a.data(), a.size(), p.data(), p.size(), out.data(), tag);
        CHECK(toHex(out.data(), out.size()) == v.c, "ciphertext %s", toHex(out.data(), out.size()).c_str());
        CHECK(toHex(tag, 16) == v.t, "tag %s, want %s", toHex(tag, 16).c_str(), v.t);
        CHECK(gcmDecrypt(k.data(), iv.data(), a.data(), a.size(), c.data(), c.size(), back.data(), t.data()) && back == p, "decrypt");
        if (!c.empty()) {
            std::vector<uint8_t> bad = c;
            bad[0] ^= 1;
            CHECK(!gcmDecrypt(k.data(), iv.data(), a.data(), a.size(), bad.data(), bad.size(), back.data(), t.data()), "a changed ciphertext is refused");
        }
    }
}

int main() {
    // CRC-16 with polynomial 0x8005, start 0xFFFF, no reflection: the catalogue's check value of "123456789"
    CHECK(crc16((const uint8_t*)"123456789", 9) == 0xAEE7, "crc %04X", crc16((const uint8_t*)"123456789", 9));

    const uint8_t pl[] = {'h', 'i'};
    const std::vector<uint8_t> pk = packetOctets(pl, 2, 4);
    CHECK(pk.size() == 4 + 2 + 1 + 2 + 2, "packet size %zu", pk.size());
    CHECK(pk[0] == 0xAA && pk[3] == 0xAA && pk[4] == 0x46 && pk[5] == 0x4C && pk[6] == 2 && pk[7] == 'h' && pk[8] == 'i', "packet layout");
    const uint16_t c = crc16(pk.data() + 6, 3);
    CHECK(pk[9] == (c >> 8) && pk[10] == (c & 0xFF), "CRC after the payload, high octet first");

    std::vector<uint8_t> big(130, 'x');
    const auto parts = splitMessage(big);
    CHECK(parts.size() == 3 && parts[0].size() == 60 && parts[1].size() == 60 && parts[2].size() == 10, "split 60 / 60 / 10");

    // keys
    const Key pw = keyFromPassword("onair");
    uint8_t want[32];
    meshcrypto::sha256((const uint8_t*)"onair", 5, want);
    CHECK(pw.on && std::memcmp(pw.k, want, 32) == 0, "password key is SHA-256 of the password");
    Key hk;
    CHECK(keyFromHex(keyHex(pw), hk) && std::memcmp(hk.k, pw.k, 32) == 0, "hex key round trip: %s", keyHex(pw).c_str());
    CHECK(!keyFromHex("00 11", hk) && !keyFromHex(std::string(64, 'g'), hk), "bad hex refused");

    gcmVectors();

    // messages
    const std::vector<uint8_t> plain = encodePlain("Flipper: hi");
    CHECK(std::string(plain.begin(), plain.end()) == "Flipper: hi\r\n", "plain line ends in CR LF");
    Message m = decodeMessage(nullptr, plain.data(), plain.size());
    CHECK(m.ok && !m.encrypted && m.text == "Flipper: hi", "plain decoded: '%s'", m.text.c_str());

    const uint8_t iv[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::vector<uint8_t> enc = encodeEncrypted(pw, 0x0102030405060708ull, 7, iv, "OnAir: secret");
    CHECK(enc.size() == kOverhead + 13, "encrypted size %zu", enc.size());
    CHECK(enc[0] == 0x08 && enc[7] == 0x01, "run id as the Flipper's little-endian uint64");
    CHECK(enc[8] == 0 && enc[9] == 0 && enc[10] == 0 && enc[11] == 7, "counter big endian");
    CHECK(std::memcmp(enc.data() + 12, iv, 12) == 0, "IV after the header");
    m = decodeMessage(&pw, enc.data(), enc.size());
    CHECK(m.ok && m.encrypted && m.text == "OnAir: secret" && m.runId == 0x0102030405060708ull && m.counter == 7, "decrypted: '%s'", m.text.c_str());
    const Key other = keyFromPassword("other");
    m = decodeMessage(&other, enc.data(), enc.size());
    CHECK(!m.ok && m.looksEncrypted, "another key: not readable");
    m = decodeMessage(nullptr, enc.data(), enc.size());
    CHECK(!m.ok && m.looksEncrypted, "no key: not readable");
    std::vector<uint8_t> hdr = enc;
    hdr[11] = 8;   // the counter is authenticated
    m = decodeMessage(&pw, hdr.data(), hdr.size());
    CHECK(!m.ok, "a changed counter fails the tag");
    // a plain line still reads with a key set (a Flipper without encryption on the same frequency)
    m = decodeMessage(&pw, plain.data(), plain.size());
    CHECK(m.ok && !m.encrypted && m.text == "Flipper: hi", "plain with a key set");

    Sender s(42);
    const auto e1 = s.encode(&pw, "a"), e2 = s.encode(&pw, "b");
    CHECK(e1[11] == 1 && e2[11] == 2 && s.counter() == 3, "the counter starts at 1 and counts");
    CHECK(std::memcmp(e1.data() + 12, e2.data() + 12, 12) != 0, "a fresh IV per message");
    CHECK(s.encode(nullptr, "c") == encodePlain("c"), "no key: plain");

    ReplayGuard g;
    CHECK(g.accept(1, 1) && g.accept(1, 2) && !g.accept(1, 2) && !g.accept(1, 1) && g.accept(2, 1) && g.accept(1, 5), "replay guard");

    CHECK(isText((const uint8_t*)"caf\xC3\xA9 ok\r\n", 10), "UTF-8 is text");
    CHECK(!isText((const uint8_t*)"a\x01", 2) && !isText((const uint8_t*)"\xC3", 1), "control octets and a cut UTF-8 sequence are not");
    CHECK(displayText("a\x1b[1mb\xFF") == "a?[1mb?", "display: '%s'", displayText("a\x1b[1mb\xFF").c_str());
    return testSummary("esubghz proto");
}
