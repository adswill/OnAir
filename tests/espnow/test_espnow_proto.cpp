// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW frame layer (espnow_proto.h): CCMP against the IEEE 802.11-2012 M.6.4 test vector, frames built and parsed for every body size
// (v1 and v2, several elements), encrypted frames with no key / a wrong key / the right key (both nonce forms), other action frames, a
// truncated element, the Espressif OUI check, and the DSSS 2 Mbit/s PPDU builder against the 1 Mbit/s one.
#include "dect2/espnow/espnow_proto.h"
#include "dect2/wifi/wifi_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::string hex(const std::vector<uint8_t>& v) { return espnow::hexText(v.data(), v.size()); }

static void ccmpVector() {
    // IEEE Std 802.11-2012 M.6.4 (the bytes also in hostap's wlantest/test_vectors.c)
    const uint8_t tk[16] = {0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85, 0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f};
    const uint8_t frame[] = {0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44, 0x08, 0xab, 0xae, 0xa5, 0xb8,
                             0xfc, 0xba, 0x80, 0x33, 0xf8, 0xba, 0x1a, 0x55, 0xd0, 0x2f, 0x85, 0xae, 0x96, 0x7b, 0xb6, 0x2f, 0xb6, 0xcd, 0xa8, 0xeb,
                             0x7e, 0x78, 0xa0, 0x50};
    const uint64_t pn = 0xB5039776E70Cull;
    const std::vector<uint8_t> want = {0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44, 0x08, 0xab, 0xae,
                                       0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0x0c, 0xe7, 0x00, 0x20, 0x76, 0x97, 0x03, 0xb5, 0xf3, 0xd0, 0xa2, 0xfe,
                                       0x9a, 0x3d, 0xbf, 0x23, 0x42, 0xa6, 0x43, 0xe4, 0x32, 0x46, 0xe8, 0x0c, 0x3c, 0x04, 0xd0, 0x19, 0x78, 0x45,
                                       0xce, 0x0b, 0x16, 0xf9, 0x76, 0x23};
    const std::vector<uint8_t> got = espnow::ccmpEncrypt(tk, frame, frame + 24, sizeof frame - 24, pn, true);
    CHECK(got == want, "CCMP test vector:\n  got  %s\n  want %s", hex(got).c_str(), hex(want).c_str());
    std::vector<uint8_t> plain;
    CHECK(espnow::ccmpDecrypt(tk, want.data(), want.size(), true, plain) && plain == std::vector<uint8_t>(frame + 24, frame + sizeof frame), "CCMP vector decrypts");
    std::vector<uint8_t> bad = want; bad[40] ^= 1;
    CHECK(!espnow::ccmpDecrypt(tk, bad.data(), bad.size(), true, plain), "a flipped ciphertext bit fails the MIC");
}

static void roundTrips() {
    for (size_t len : {0, 1, 7, 100, 249, 250, 251, 500, 750, 1000, 1470}) {
        espnow::Build b;
        b.version = len > 250 ? 2 : 1;
        b.seq = (uint16_t)((len * 3 + 1) & 0xFFF);
        b.random = 0xA1B2C3D4u;
        for (size_t i = 0; i < len; i++) b.body.push_back((uint8_t)(i * 31 + 5));
        const std::vector<uint8_t> f = espnow::makeFrame(b);
        const size_t n = f.size() - 4;
        const uint32_t fcs = (uint32_t)f[n] | (uint32_t)f[n + 1] << 8 | (uint32_t)f[n + 2] << 16 | (uint32_t)f[n + 3] << 24;
        CHECK(wifi::crc32(f.data(), n) == fcs, "%zu: FCS", len);
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), n, p), "%zu: parsed", len);
        const size_t elems = len == 0 ? 1 : (len + 249) / 250;
        CHECK(p.action && p.espnow && !p.isProtected && p.broadcast && p.body == b.body && p.seq == b.seq && p.random == 0xA1B2C3D4u &&
              p.version == b.version && p.elements == (int)elems && !p.moreData && p.elemOui == espnow::kEspressifOui && !p.truncated,
              "%zu: fields (body %zu, seq %u, elements %d, version %d)", len, p.body.size(), (unsigned)p.seq, p.elements, p.version);
        CHECK(wifi::macText(p.sa) == "24:6F:28:11:22:33", "%zu: source", len);
        // the frame the Wi-Fi network monitor sees: a management action frame, counted only
        wifi::Frame wf;
        CHECK(wifi::parseFrame(f.data(), n, wf) && wf.type == 0 && wf.subtype == 13 && !wf.beacon, "%zu: an 802.11 action frame", len);
    }
    {   // a unicast frame
        espnow::Build b;
        const uint8_t da[6] = {0x84, 0xCC, 0xA8, 1, 2, 3};
        std::memcpy(b.da, da, 6);
        b.body = {'h', 'i'};
        const std::vector<uint8_t> f = espnow::makeFrame(b);
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), f.size() - 4, p) && !p.broadcast && wifi::macText(p.da) == "84:CC:A8:01:02:03", "unicast");
        CHECK(espnow::summary(p).find("\"hi\"") == std::string::npos, "two bytes are not called text");
    }
    for (uint16_t sq : {0, 1, 4094, 4095}) {   // the 12-bit sequence number up to where it wraps
        espnow::Build b; b.seq = sq;
        const std::vector<uint8_t> f = espnow::makeFrame(b);
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), f.size() - 4, p) && p.seq == sq, "sequence %u", (unsigned)sq);
    }
    {   // text summary
        espnow::Build b;
        const std::string s = "temp=21.5";
        b.body.assign(s.begin(), s.end());
        const std::vector<uint8_t> f = espnow::makeFrame(b);
        espnow::Frame p;
        espnow::parse(f.data(), f.size() - 4, p);
        CHECK(espnow::summary(p).find("\"temp=21.5\"") != std::string::npos, "summary %s", espnow::summary(p).c_str());
    }
}

static void encrypted() {
    uint8_t key[16], wrong[16];
    for (int i = 0; i < 16; i++) { key[i] = (uint8_t)(0x40 + i); wrong[i] = (uint8_t)(0x41 + i); }
    for (size_t len : {0, 16, 33, 250}) {
        espnow::Build b;
        const uint8_t da[6] = {0x30, 0xAE, 0xA4, 9, 9, 9};
        std::memcpy(b.da, da, 6);
        for (size_t i = 0; i < len; i++) b.body.push_back((uint8_t)(255 - i));
        b.encrypt = true; std::memcpy(b.key, key, 16); b.pn = 0x0102030405ull + len;
        const std::vector<uint8_t> f = espnow::makeFrame(b);
        const size_t n = f.size() - 4;
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), n, p) && p.action && p.isProtected && !p.decrypted && !p.espnow && p.body.empty() && p.pn == b.pn, "%zu: no key: encrypted", len);
        CHECK(espnow::summary(p).find("encrypted") == 0, "%zu: summary says encrypted", len);
        CHECK(espnow::parse(f.data(), n, p, {std::vector<uint8_t>(wrong, wrong + 16)}) && !p.decrypted && p.body.empty(), "%zu: wrong key", len);
        CHECK(espnow::parse(f.data(), n, p, {std::vector<uint8_t>(wrong, wrong + 16), std::vector<uint8_t>(key, key + 16)}) && p.decrypted && p.espnow &&
              p.body == b.body, "%zu: the right key (second in the list)", len);
        std::vector<uint8_t> bad(f.begin(), f.begin() + (ptrdiff_t)n);
        bad[24 + 8 + 3] ^= 0x80;
        CHECK(espnow::parse(bad.data(), bad.size(), p, {std::vector<uint8_t>(key, key + 16)}) && !p.decrypted, "%zu: damaged ciphertext stays encrypted", len);
    }
    {   // the other nonce form (no management flag) is tried too
        espnow::Build b;
        b.body = {1, 2, 3, 4, 5};
        const std::vector<uint8_t> clear = espnow::makeFrame(b);
        const std::vector<uint8_t> f = espnow::ccmpEncrypt(key, clear.data(), clear.data() + 24, clear.size() - 4 - 24, 77, false);
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), f.size(), p, {std::vector<uint8_t>(key, key + 16)}) && p.decrypted && p.body == b.body, "nonce without the management flag");
    }
}

static void others() {
    {   // a public action frame (category 4): action, not ESP-NOW
        espnow::Build b;
        std::vector<uint8_t> f = espnow::makeFrame(b);
        f[24] = 4;
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), f.size() - 4, p) && p.action && !p.espnow, "category 4");
        CHECK(espnow::summary(p).find("not ESP-NOW") != std::string::npos, "summary of a foreign action frame");
    }
    {   // another vendor's OUI in the action header
        espnow::Build b;
        std::vector<uint8_t> f = espnow::makeFrame(b);
        f[25] = 0x00; f[26] = 0x50; f[27] = 0xF2;
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), f.size() - 4, p) && !p.espnow, "other OUI");
    }
    {   // an element longer than the frame
        espnow::Build b;
        b.body = {1, 2, 3, 4, 5, 6, 7, 8};
        std::vector<uint8_t> f = espnow::makeFrame(b);
        f[33] = 200;   // the element's length
        espnow::Frame p;
        CHECK(espnow::parse(f.data(), f.size() - 4, p) && p.truncated && !p.espnow, "truncated element");
    }
    {   // a beacon and a short frame are not action frames
        wifi::ApSpec ap; ap.ssid = "x";
        const std::vector<uint8_t> f = wifi::makeBeacon(ap);
        espnow::Frame p;
        CHECK(!espnow::parse(f.data(), f.size() - 4, p) && !p.action, "beacon");
        CHECK(!espnow::parse(f.data(), 10, p), "10 bytes");
    }
    {   // the v2 "more data" bit
        espnow::Build b;
        b.version = 2;
        b.body.assign(300, 0x55);
        std::vector<uint8_t> f = espnow::makeFrame(b);
        espnow::Frame p;
        espnow::parse(f.data(), f.size() - 4, p);
        CHECK(p.elements == 2 && p.body.size() == 300 && !p.moreData && p.version == 2, "v2: two elements, more data cleared on the last");
        CHECK((f[32 + 6] & 0x10) != 0, "v2: more data set on the first element (its version byte)");
    }
    const uint8_t esp[6] = {0x24, 0x0A, 0xC4, 1, 2, 3}, esp2[6] = {0x18, 0xFE, 0x34, 0, 0, 1}, apple[6] = {0x00, 0x03, 0x93, 1, 2, 3};
    CHECK(espnow::espressifOui(esp) && espnow::espressifOui(esp2) && !espnow::espressifOui(apple), "Espressif OUIs");
    CHECK(espnow::mostlyText((const uint8_t*)"hello", 5) && !espnow::mostlyText((const uint8_t*)"\x01\x02\x03\x04", 4), "text test");
    CHECK(espnow::printable((const uint8_t*)"a\x01" "b", 3) == "a.b", "printable");
}

static void ppdu() {
    std::vector<uint8_t> m(40);
    for (size_t i = 0; i < m.size(); i++) m[i] = (uint8_t)(i * 13);
    const wifi::DsssPpdu one = wifi::dsssPpdu(m, 1, false);
    CHECK(one.bits == wifi::ppduBits(m) && one.qpskFrom == one.bits.size(), "1 Mbit/s: the same bits as before");
    const wifi::DsssPpdu two = wifi::dsssPpdu(m, 2, false);
    CHECK(two.bits.size() == one.bits.size() && two.qpskFrom == (size_t)(wifi::kSyncBits + 16 + 48), "2 Mbit/s long: DQPSK after the header");
    const wifi::DsssPpdu sh = wifi::dsssPpdu(m, 2, true);
    CHECK(sh.bits.size() == (size_t)(wifi::kSyncShortBits + 16 + 48) + m.size() * 8 && sh.qpskFrom == (size_t)(wifi::kSyncShortBits + 16), "short preamble layout");
}

int main() {
    ccmpVector();
    roundTrips();
    encrypted();
    others();
    ppdu();
    return testSummary("espnow proto");
}
