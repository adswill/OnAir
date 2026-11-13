// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW frame layer (see espnow_proto.h).
#include "dect2/espnow/espnow_proto.h"
#include "dect2/voicekeys/net_crypto.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2::espnow {

namespace {
uint32_t oui3(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

// The CCMP nonce (flags, A2, PN5 .. PN0) and the additional authenticated data (masked frame control, A1, A2, A3, masked sequence control)
// of a frame with a three-address header (clause 12.5.3.3.3 / 12.5.3.3.4)
void ccmpParts(const uint8_t* h, uint64_t pn, bool mgmtFlag, uint8_t nonce[13], uint8_t aad[22]) {
    const int type = h[0] >> 2 & 3;
    nonce[0] = (uint8_t)(type == 0 && mgmtFlag ? 0x10 : 0x00);
    std::memcpy(nonce + 1, h + 10, 6);
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (8 * (5 - i)));
    aad[0] = type == 2 ? (uint8_t)(h[0] & 0x8F) : h[0];             // data frames: subtype bits 4 - 6 masked
    aad[1] = (uint8_t)((h[1] & ~(0x08 | 0x10 | 0x20)) | 0x40);      // retry, power management, more data masked; protected set
    std::memcpy(aad + 2, h + 4, 18);                                  // A1, A2, A3
    aad[20] = (uint8_t)(h[22] & 0x0F); aad[21] = 0;                   // the fragment number only
}

// The ESP-NOW content of a frame body that starts at the category
void parseBody(const uint8_t* b, size_t n, Frame& f) {
    if (n < 8 || b[0] != kCategoryVendor || oui3(b + 1) != kEspressifOui) return;
    f.random = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
    size_t i = 8;
    while (i + 2 <= n) {
        const uint8_t id = b[i], len = b[i + 1];
        if (i + 2 + len > n) { f.truncated = true; break; }
        if (id == kElementVendor && len >= 5 && b[i + 5] == kTypeEspNow) {
            if (f.elements == 0) { f.version = b[i + 6] & 0x0F; f.elemOui = oui3(b + i + 2); }
            f.moreData = (b[i + 6] >> 4 & 1) != 0;
            f.body.insert(f.body.end(), b + i + 7, b + i + 2 + len);
            f.elements++;
        }
        i += 2 + (size_t)len;
    }
    f.espnow = f.elements > 0;
}
} // namespace

bool ccmpDecrypt(const uint8_t key[16], const uint8_t* f, size_t n, bool mgmtFlag, std::vector<uint8_t>& plain) {
    plain.clear();
    if (n < 24 + 8 + 8 || !(f[27] & 0x20)) return false;   // header, CCMP header (ExtIV set), MIC
    const uint64_t pn = (uint64_t)f[24] | (uint64_t)f[25] << 8 | (uint64_t)f[28] << 16 | (uint64_t)f[29] << 24 | (uint64_t)f[30] << 32 | (uint64_t)f[31] << 40;
    uint8_t nonce[13], aad[22];
    ccmpParts(f, pn, mgmtFlag, nonce, aad);
    return voicekeys::ccmStarDecrypt(key, nonce, aad, sizeof aad, f + 32, n - 32 - 8, 8, plain);
}

std::vector<uint8_t> ccmpEncrypt(const uint8_t key[16], const uint8_t hdr[24], const uint8_t* body, size_t n, uint64_t pn, bool mgmtFlag) {
    std::vector<uint8_t> out(hdr, hdr + 24);
    out[1] |= 0x40;
    uint8_t nonce[13], aad[22];
    ccmpParts(out.data(), pn, mgmtFlag, nonce, aad);
    const uint8_t ch[8] = {(uint8_t)pn, (uint8_t)(pn >> 8), 0, 0x20, (uint8_t)(pn >> 16), (uint8_t)(pn >> 24), (uint8_t)(pn >> 32), (uint8_t)(pn >> 40)};
    out.insert(out.end(), ch, ch + 8);
    const std::vector<uint8_t> c = voicekeys::ccmStarEncrypt(key, nonce, aad, sizeof aad, body, n, 8);
    out.insert(out.end(), c.begin(), c.end());
    return out;
}

bool parse(const uint8_t* p, size_t n, Frame& f, const std::vector<std::vector<uint8_t>>& keys) {
    f = Frame();
    if (n < 24) return false;
    const int type = p[0] >> 2 & 3, sub = p[0] >> 4;
    if (type != 0 || (sub != 13 && sub != 14)) return false;
    f.action = true;
    std::memcpy(f.da, p + 4, 6); std::memcpy(f.sa, p + 10, 6); std::memcpy(f.a3, p + 16, 6);
    f.broadcast = std::all_of(f.da, f.da + 6, [](uint8_t v) { return v == 0xFF; });
    f.seq = (uint16_t)((p[22] | p[23] << 8) >> 4);
    f.isProtected = (p[1] & 0x40) != 0;
    if (!f.isProtected) { parseBody(p + 24, n - 24, f); return true; }
    if (n < 24 + 8 + 8) return true;
    f.pn = (uint64_t)p[24] | (uint64_t)p[25] << 8 | (uint64_t)p[28] << 16 | (uint64_t)p[29] << 24 | (uint64_t)p[30] << 32 | (uint64_t)p[31] << 40;
    f.keyId = p[27] >> 6;
    for (const auto& k : keys) {
        if (k.size() != 16) continue;
        for (bool mg : {true, false}) {
            std::vector<uint8_t> plain;
            if (!ccmpDecrypt(k.data(), p, n, mg, plain)) continue;
            f.decrypted = true;
            parseBody(plain.data(), plain.size(), f);
            return true;
        }
    }
    return true;
}

std::vector<uint8_t> makeFrame(const Build& b) {
    std::vector<uint8_t> h = {0xD0, 0x00, 0x00, 0x00};
    h.insert(h.end(), b.da, b.da + 6);
    h.insert(h.end(), b.sa, b.sa + 6);
    for (int i = 0; i < 6; i++) h.push_back(0xFF);
    const uint16_t sc = (uint16_t)(b.seq << 4);
    h.push_back((uint8_t)sc); h.push_back((uint8_t)(sc >> 8));
    std::vector<uint8_t> body = {kCategoryVendor, 0x18, 0xFE, 0x34,
                                 (uint8_t)(b.random >> 24), (uint8_t)(b.random >> 16), (uint8_t)(b.random >> 8), (uint8_t)b.random};
    size_t at = 0;
    do {
        const size_t k = std::min(kMaxBodyV1, b.body.size() - at);
        const bool more = at + k < b.body.size();
        body.push_back(kElementVendor);
        body.push_back((uint8_t)(5 + k));
        body.push_back(0x18); body.push_back(0xFE); body.push_back(0x34);
        body.push_back(kTypeEspNow);
        body.push_back((uint8_t)((b.version & 0x0F) | (more ? 0x10 : 0)));
        body.insert(body.end(), b.body.begin() + (ptrdiff_t)at, b.body.begin() + (ptrdiff_t)(at + k));
        at += k;
    } while (at < b.body.size());
    std::vector<uint8_t> f;
    if (b.encrypt) f = ccmpEncrypt(b.key, h.data(), body.data(), body.size(), b.pn, true);
    else { f = h; f.insert(f.end(), body.begin(), body.end()); }
    wifi::appendFcs(f);
    return f;
}

bool espressifOui(const uint8_t mac[6]) {
    static const uint32_t kOuis[] = {
        0x083AF2, 0x0C8B95, 0x0CB815, 0x0CDC7E, 0x10521C, 0x18FE34, 0x240AC4, 0x244CAB, 0x246F28, 0x24A160, 0x24B2DE, 0x24D7EB, 0x24DCC3,
        0x2C3AE8, 0x2CF432, 0x308398, 0x30AEA4, 0x30C6F7, 0x34865D, 0x34945B, 0x348518, 0x34AB95, 0x34B472, 0x3C6105, 0x3C71BF,
        0x40225D, 0x404CCA, 0x409151, 0x40F520, 0x441793, 0x483FDA, 0x485519, 0x48E729, 0x4C11AE, 0x4C7525, 0x4CEBD6, 0x500291, 0x543204,
        0x5443B2, 0x588C81, 0x5CCF7F, 0x600194, 0x6055F9, 0x64B708, 0x64E833, 0x68B6B3, 0x68C63A, 0x70039F, 0x70041D, 0x70B8F6, 0x744DBD,
        0x782184, 0x78E36D, 0x7C2C67, 0x7C9EBD, 0x7CDFA1, 0x807D3A, 0x840D8E, 0x84CCA8, 0x84F3EB, 0x84F703, 0x8813BF, 0x8C4B14,
        0x8CAAB5, 0x8CCE4E, 0x9097D5, 0x943CC6, 0x94B555, 0x94B97E, 0x98CDAC, 0x98F4AB, 0x9C9C1F, 0xA020A6, 0xA0764E, 0xA0B765, 0xA4CF12,
        0xA4E57C, 0xA842E3, 0xAC0BFB, 0xAC67B2, 0xACD074, 0xB0A732, 0xB0B21C, 0xB48A0A, 0xB4E62D, 0xB8D61A, 0xBCDDC2, 0xBCFF4D, 0xC049EF,
        0xC44F33, 0xC45BBE, 0xC4D8D5, 0xC4DD57, 0xC82B96, 0xC8C9A3, 0xC8F09E, 0xCC50E3, 0xCC7B5C, 0xCCDBA7, 0xD48AFC, 0xD4D4DA, 0xD4F98D,
        0xD8132A, 0xD8A01D, 0xD8BC38, 0xD8BFC0, 0xD8F15B, 0xDC4F22, 0xDC5475, 0xDCDA0C, 0xE05A1B, 0xE09806, 0xE465B8, 0xE831CD, 0xE868E7,
        0xE89F6D, 0xE8DB84, 0xEC6260, 0xEC94CB, 0xECDA3B, 0xECFABC, 0xF008D1, 0xF09E9E, 0xF4CFA2, 0xF412FA, 0xFCB467, 0xFCE8C0, 0xFCF5C4};
    const uint32_t o = oui3(mac);
    for (uint32_t k : kOuis) if (k == o) return true;
    return false;
}

std::string hexText(const uint8_t* p, size_t n, size_t maxBytes) {
    std::string s;
    char b[4];
    const size_t m = std::min(n, maxBytes);
    s.reserve(m * 3);
    for (size_t i = 0; i < m; i++) { snprintf(b, sizeof b, "%02X", p[i]); if (i) s += ' '; s += b; }
    if (m < n) s += " ...";
    return s;
}

std::string printable(const uint8_t* p, size_t n, size_t maxBytes) {
    std::string s;
    const size_t m = std::min(n, maxBytes);
    for (size_t i = 0; i < m; i++) s += (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
    if (m < n) s += "...";
    return s;
}

bool mostlyText(const uint8_t* p, size_t n) {
    if (n < 3) return false;
    size_t ok = 0;
    for (size_t i = 0; i < n; i++) ok += (p[i] >= 0x20 && p[i] < 0x7F) || p[i] == '\n' || p[i] == '\r' || p[i] == '\t' || p[i] == 0 ? 1 : 0;
    return ok * 5 >= n * 4;
}

std::string summary(const Frame& f) {
    char b[200];
    const std::string to = f.broadcast ? std::string("broadcast") : wifi::macText(f.da);
    if (f.isProtected && !f.decrypted) {
        snprintf(b, sizeof b, "encrypted (CCMP, PN %llu) to %s", (unsigned long long)f.pn, to.c_str());
        return b;
    }
    if (!f.espnow) { snprintf(b, sizeof b, "action frame, not ESP-NOW, to %s", to.c_str()); return b; }
    snprintf(b, sizeof b, "v%d, %zu bytes to %s%s%s%s", f.version, f.body.size(), to.c_str(), f.elements > 1 ? ", several elements" : "",
             f.decrypted ? ", decrypted" : "", f.moreData ? ", more data" : "");
    std::string s = b;
    if (!f.body.empty() && mostlyText(f.body.data(), f.body.size())) s += ": \"" + printable(f.body.data(), f.body.size(), 60) + "\"";
    return s;
}

} // namespace dect2::espnow
