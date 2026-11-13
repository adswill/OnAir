// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat: packets, CRC, keys, AES-256-GCM and messages, see esubghz_proto.h
#include "dect2/esubghz/esubghz_proto.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cstring>
#include <random>

namespace dect2 {
namespace esubghz {

uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}

std::vector<uint8_t> packetOctets(const uint8_t* payload, size_t n, int preambleOctets) {
    std::vector<uint8_t> o((size_t)preambleOctets, 0xAA);
    o.push_back(kSync0);
    o.push_back(kSync1);
    const size_t body = o.size();
    o.push_back((uint8_t)n);
    o.insert(o.end(), payload, payload + n);
    const uint16_t c = crc16(o.data() + body, n + 1);
    o.push_back((uint8_t)(c >> 8));
    o.push_back((uint8_t)c);
    return o;
}

std::vector<std::vector<uint8_t>> splitMessage(const std::vector<uint8_t>& msg) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = 0; i < msg.size(); i += kMaxChunk) out.emplace_back(msg.begin() + (long)i, msg.begin() + (long)std::min(msg.size(), i + kMaxChunk));
    return out;
}

Key keyFromPassword(const std::string& password) {
    Key k;
    k.on = true;
    meshcrypto::sha256((const uint8_t*)password.data(), password.size(), k.k);
    return k;
}

bool keyFromHex(const std::string& hex, Key& out) {
    std::string h;
    for (char c : hex) if (c != ' ' && c != ':' && c != '-' && c != '\t') h += c;
    std::vector<uint8_t> b;
    if (h.size() != 2 * sizeof out.k || !meshcrypto::hexDecode(h, b) || b.size() != sizeof out.k) return false;
    out.on = true;
    std::copy_n(b.begin(), sizeof out.k, out.k);
    return true;
}

std::string keyHex(const Key& k) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 32; i++) {
        if (i) s += ' ';
        s += d[k.k[i] >> 4];
        s += d[k.k[i] & 15];
    }
    return s;
}

// ---------------------------------------------------------------- AES-256-GCM

namespace {

struct Block { uint64_t hi = 0, lo = 0; };

Block load(const uint8_t* p) {
    Block b;
    for (int i = 0; i < 8; i++) { b.hi = (b.hi << 8) | p[i]; b.lo = (b.lo << 8) | p[8 + i]; }
    return b;
}
void store(Block b, uint8_t* p) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)b.hi; p[8 + i] = (uint8_t)b.lo; b.hi >>= 8; b.lo >>= 8; }
}

// multiplication in GF(2^128) with the GCM bit order (SP 800-38D algorithm 1)
Block gmul(Block x, Block y) {
    Block z, v = y;
    for (int i = 0; i < 128; i++) {
        const bool bit = i < 64 ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
        if (bit) { z.hi ^= v.hi; z.lo ^= v.lo; }
        const bool lsb = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        if (lsb) v.hi ^= 0xE100000000000000ull;
    }
    return z;
}

struct Ghash {
    Block h, s;
    void absorb(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i += 16) {
            uint8_t blk[16] = {};   // a short last block is padded with zeros
            std::copy_n(p + i, std::min(sizeof blk, n - i), blk);
            const Block b = load(blk);
            s.hi ^= b.hi; s.lo ^= b.lo;
            s = gmul(s, h);
        }
    }
    void lengths(size_t aadLen, size_t n) {
        s.hi ^= (uint64_t)aadLen * 8; s.lo ^= (uint64_t)n * 8;
        s = gmul(s, h);
    }
};

void gcmCore(const meshcrypto::Aes& aes, const uint8_t iv[12], const uint8_t* in, size_t n, uint8_t* out) {
    uint8_t ctr[16], ks[16];
    static_assert(sizeof ctr == kIvOctets + 4, "the IV and a 32-bit counter fill the block");
    std::copy_n(iv, kIvOctets, ctr);
    uint32_t c = 2;   // J0 = IV || 1 makes the tag; the data starts at inc32(J0)
    for (size_t i = 0; i < n; i += 16) {
        ctr[12] = (uint8_t)(c >> 24); ctr[13] = (uint8_t)(c >> 16); ctr[14] = (uint8_t)(c >> 8); ctr[15] = (uint8_t)c;
        aes.encryptBlock(ctr, ks);
        for (size_t j = 0; j < 16 && i + j < n; j++) out[i + j] = in[i + j] ^ ks[j];
        c++;
    }
}

void gcmTag(const meshcrypto::Aes& aes, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, const uint8_t* c, size_t n, uint8_t tag[16]) {
    uint8_t zero[16] = {}, hb[16], j0[16];
    aes.encryptBlock(zero, hb);
    Ghash g;
    g.h = load(hb);
    g.absorb(aad, aadLen);
    g.absorb(c, n);
    g.lengths(aadLen, n);
    static_assert(sizeof j0 == kIvOctets + 4, "the IV and a 32-bit counter fill the block");
    std::copy_n(iv, kIvOctets, j0);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    uint8_t ek[16], sb[16];
    aes.encryptBlock(j0, ek);
    store(g.s, sb);
    for (int i = 0; i < 16; i++) tag[i] = ek[i] ^ sb[i];
}

} // namespace

void gcmEncrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t n, uint8_t* out, uint8_t tag[16]) {
    meshcrypto::Aes aes;
    aes.setKey(key, 32);
    gcmCore(aes, iv, in, n, out);
    gcmTag(aes, iv, aad, aadLen, out, n, tag);
}

bool gcmDecrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t n, uint8_t* out, const uint8_t tag[16]) {
    meshcrypto::Aes aes;
    aes.setKey(key, 32);
    uint8_t t[16];
    gcmTag(aes, iv, aad, aadLen, in, n, t);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(t[i] ^ tag[i]);
    if (diff) return false;
    gcmCore(aes, iv, in, n, out);
    return true;
}

// ---------------------------------------------------------------- messages

std::vector<uint8_t> encodePlain(const std::string& line) {
    std::vector<uint8_t> m(line.begin(), line.end());
    m.push_back('\r');
    m.push_back('\n');
    return m;
}

std::vector<uint8_t> encodeEncrypted(const Key& k, uint64_t runId, uint32_t counter, const uint8_t iv[12], const std::string& line) {
    static_assert(kOverhead == kHeaderOctets + kIvOctets + kTagOctets, "header, IV and tag come before the text");
    std::vector<uint8_t> m(kOverhead + line.size());
    for (size_t i = 0; i < 8; i++) m[i] = (uint8_t)(runId >> (8 * i));   // the Flipper copies its native (little-endian) uint64_t
    m[8] = (uint8_t)(counter >> 24); m[9] = (uint8_t)(counter >> 16); m[10] = (uint8_t)(counter >> 8); m[11] = (uint8_t)counter;
    std::copy_n(iv, kIvOctets, m.begin() + kHeaderOctets);
    gcmEncrypt(k.k, iv, m.data(), kHeaderOctets, (const uint8_t*)line.data(), line.size(), m.data() + kOverhead, m.data() + kHeaderOctets + kIvOctets);
    return m;
}

// the length of the UTF-8 sequence that starts at p[i] (1 for ASCII), 0 when it is not valid
static size_t utf8Len(const uint8_t* p, size_t n, size_t i) {
    const uint8_t c = p[i];
    if (c < 0x80) return 1;
    const size_t more = c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : 0;
    if (more == 0 || i + more >= n) return 0;
    for (size_t j = 1; j <= more; j++) if ((p[i + j] & 0xC0) != 0x80) return 0;
    return more + 1;
}

bool isText(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        const uint8_t c = p[i];
        if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') return false;
        if (c == 0x7F) return false;
        const size_t l = utf8Len(p, n, i);
        if (!l) return false;
        i += l;
    }
    return true;
}

static std::string trimEol(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

Message decodeMessage(const Key* key, const uint8_t* p, size_t n) {
    Message m;
    if (!p || n == 0) return m;
    if (key && key->on && n >= kOverhead + 1) {
        std::vector<uint8_t> out(n - kOverhead);
        if (gcmDecrypt(key->k, p + kHeaderOctets, p, kHeaderOctets, p + kOverhead, out.size(), out.data(), p + kHeaderOctets + kIvOctets)) {
            m.ok = true;
            m.encrypted = true;
            for (size_t i = 0; i < 8; i++) m.runId |= (uint64_t)p[i] << (8 * i);
            m.counter = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
            m.text = trimEol(std::string(out.begin(), out.end()));
            return m;
        }
    }
    if (isText(p, n)) {
        m.ok = true;
        m.text = trimEol(std::string((const char*)p, n));
        return m;
    }
    m.looksEncrypted = n >= kOverhead + 1;
    return m;
}

bool ReplayGuard::accept(uint64_t runId, uint32_t counter) {
    auto it = last_.find(runId);
    if (it != last_.end() && it->second >= counter) return false;
    last_[runId] = counter;
    return true;
}

Sender::Sender() {
    std::random_device rd;
    runId_ = ((uint64_t)rd() << 32) ^ (uint64_t)rd();
}

std::vector<uint8_t> Sender::encode(const Key* key, const std::string& line) {
    if (!key || !key->on) return encodePlain(line);
    std::random_device rd;
    uint8_t iv[kIvOctets];
    for (size_t i = 0; i < kIvOctets; i += 4) { const uint32_t r = rd(); for (size_t j = 0; j < 4; j++) iv[i + j] = (uint8_t)(r >> (8 * j)); }
    return encodeEncrypted(*key, runId_, counter_++, iv, line);
}

std::string displayText(const std::string& s) {
    std::string o;
    const uint8_t* p = (const uint8_t*)s.data();
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = p[i];
        const size_t l = utf8Len(p, s.size(), i);
        if (l == 1) o += c == '\t' ? ' ' : c < 0x20 || c == 0x7F ? '?' : (char)c;
        else if (l > 1) o.append(s, i, l);
        else o += '?';
        i += l ? l : 1;
    }
    return o;
}

} // namespace esubghz
} // namespace dect2
