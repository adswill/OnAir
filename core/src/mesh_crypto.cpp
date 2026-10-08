// AES, SHA-256, HMAC-SHA256, SHA-512 and the text helpers; Ed25519 is in mesh_crypto_ed25519.cpp.
#include "dect2/mesh_crypto.h"
#include "mesh_crypto_tables.h"
#include <cstring>

namespace dect2 {
namespace meshcrypto {

namespace {

uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}
const uint8_t* invSbox() {
    static uint8_t t[256];
    static bool init = [] { for (int i = 0; i < 256; i++) t[kAesSbox[i]] = (uint8_t)i; return true; }();
    (void)init;
    return t;
}

inline uint32_t ror32(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }
inline uint64_t ror64(uint64_t x, int s) { return (x >> s) | (x << (64 - s)); }

} // namespace

// FIPS 197 5.2: key expansion into round keys kept as bytes in state order
bool Aes::setKey(const uint8_t* key, size_t len) {
    if (len != 16 && len != 32) return false;
    const int nk = (int)len / 4;
    rounds_ = nk + 6;
    const int words = 4 * (rounds_ + 1);
    std::memcpy(rk_, key, len);
    uint8_t rcon = 1;
    for (int i = nk; i < words; i++) {
        uint8_t t[4];
        std::memcpy(t, &rk_[4 * (i - 1)], 4);
        if (i % nk == 0) {
            const uint8_t f = t[0];
            t[0] = (uint8_t)(kAesSbox[t[1]] ^ rcon);
            t[1] = kAesSbox[t[2]];
            t[2] = kAesSbox[t[3]];
            t[3] = kAesSbox[f];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = kAesSbox[t[k]];
        }
        for (int k = 0; k < 4; k++) rk_[4 * i + k] = (uint8_t)(rk_[4 * (i - nk) + k] ^ t[k]);
    }
    return true;
}

void Aes::encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk_[i]);
    for (int r = 1; r <= rounds_; r++) {
        for (int i = 0; i < 16; i++) s[i] = kAesSbox[s[i]];
        for (int c = 0; c < 4; c++)                       // ShiftRows: row k moves left by k
            for (int k = 0; k < 4; k++) t[k + 4 * c] = s[k + 4 * ((c + k) & 3)];
        if (r < rounds_) {
            for (int c = 0; c < 4; c++) {                 // MixColumns
                const uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c]     = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                s[4 * c + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                s[4 * c + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        } else {
            std::memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk_[16 * r + i];
    }
    std::memcpy(out, s, 16);
}

void Aes::decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const uint8_t* inv = invSbox();
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk_[16 * rounds_ + i]);
    for (int r = rounds_ - 1; r >= 0; r--) {
        for (int c = 0; c < 4; c++)                       // InvShiftRows: row k moves right by k
            for (int k = 0; k < 4; k++) t[k + 4 * ((c + k) & 3)] = s[k + 4 * c];
        for (int i = 0; i < 16; i++) t[i] = (uint8_t)(inv[t[i]] ^ rk_[16 * r + i]);
        if (r > 0) {
            for (int c = 0; c < 4; c++) {                 // InvMixColumns
                const uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c]     = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                s[4 * c + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                s[4 * c + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                s[4 * c + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        } else {
            std::memcpy(s, t, 16);
        }
    }
    std::memcpy(out, s, 16);
}

std::vector<uint8_t> aesEcbEncrypt(const uint8_t* key, size_t keyLen, const uint8_t* in, size_t n) {
    std::vector<uint8_t> out;
    Aes a;
    if (!a.setKey(key, keyLen)) return out;
    out.resize((n + 15) / 16 * 16);
    for (size_t o = 0; o < out.size(); o += 16) {
        uint8_t blk[16] = {};
        std::memcpy(blk, in + o, n - o < 16 ? n - o : 16);
        a.encryptBlock(blk, &out[o]);
    }
    return out;
}

std::vector<uint8_t> aesEcbDecrypt(const uint8_t* key, size_t keyLen, const uint8_t* in, size_t n) {
    std::vector<uint8_t> out;
    Aes a;
    if (!a.setKey(key, keyLen)) return out;
    out.resize(n / 16 * 16);
    for (size_t o = 0; o < out.size(); o += 16) a.decryptBlock(in + o, &out[o]);
    return out;
}

bool aesCtr(const uint8_t* key, size_t keyLen, const uint8_t iv[16], uint8_t* data, size_t n) {
    Aes a;
    if (!a.setKey(key, keyLen)) return false;
    uint8_t ctr[16], ks[16];
    std::memcpy(ctr, iv, 16);
    for (size_t o = 0; o < n; o += 16) {
        a.encryptBlock(ctr, ks);
        const size_t m = n - o < 16 ? n - o : 16;
        for (size_t i = 0; i < m; i++) data[o + i] ^= ks[i];
        for (int i = 15; i >= 0; i--) if (++ctr[i] != 0) break;
    }
    return true;
}

// ---- SHA-256 (FIPS 180-4 6.2) ----
void Sha256::reset() {
    for (int i = 0; i < 8; i++) h_[i] = kSha256H0[i];
    total_ = 0;
    fill_ = 0;
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        const uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const uint8_t* p, size_t n) {
    total_ += n;
    while (n) {
        const size_t m = 64 - fill_ < n ? 64 - fill_ : n;
        std::memcpy(buf_ + fill_, p, m);
        fill_ += m; p += m; n -= m;
        if (fill_ == 64) { block(buf_); fill_ = 0; }
    }
}

void Sha256::final(uint8_t out[32]) {
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (fill_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h_[i] >> (24 - 8 * k));
    reset();
}

void sha256(const uint8_t* p, size_t n, uint8_t out[32]) {
    Sha256 s;
    s.update(p, n);
    s.final(out);
}

// RFC 2104 / RFC 4231
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t n, uint8_t out[32]) {
    uint8_t k[64] = {};
    if (keyLen > 64) sha256(key, keyLen, k);
    else if (keyLen) std::memcpy(k, key, keyLen);
    uint8_t pad[64], inner[32];
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    Sha256 s;
    s.update(pad, 64);
    s.update(msg, n);
    s.final(inner);
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    s.update(pad, 64);
    s.update(inner, 32);
    s.final(out);
}

// ---- SHA-512 (FIPS 180-4 6.4), one shot ----
void sha512(const uint8_t* p, size_t n, uint8_t out[64]) {
    uint64_t h[8];
    for (int i = 0; i < 8; i++) h[i] = kSha512H0[i];
    std::vector<uint8_t> m(p, p + n);
    m.push_back(0x80);
    while (m.size() % 128 != 112) m.push_back(0);
    for (int i = 0; i < 8; i++) m.push_back(0);                    // high 64 bits of the 128-bit length
    const uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m.push_back((uint8_t)(bits >> (56 - 8 * i)));
    for (size_t o = 0; o < m.size(); o += 128) {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            uint64_t v = 0;
            for (int k = 0; k < 8; k++) v = (v << 8) | m[o + 8 * i + k];
            w[i] = v;
        }
        for (int i = 16; i < 80; i++) {
            const uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            const uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
            const uint64_t ch = (e & f) ^ (~e & g);
            const uint64_t t1 = hh + S1 + ch + kSha512K[i] + w[i];
            const uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
            const uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) out[8 * i + k] = (uint8_t)(h[i] >> (56 - 8 * k));
}

// ---- text helpers ----
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool base64Decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    size_t pad = 0;
    for (char ch : s) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
        if (ch == '=') { pad++; continue; }
        if (pad) return false;                                    // data after padding
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;                  // '-' and '_' are the URL-safe spelling
        else if (ch == '/' || ch == '_') v = 63;
        else return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); acc &= (1u << bits) - 1; }
    }
    return true;
}

std::string base64Encode(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t)p[i] << 16) | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        s += kB64[(v >> 18) & 63];
        s += kB64[(v >> 12) & 63];
        s += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        s += i + 2 < n ? kB64[v & 63] : '=';
    }
    return s;
}

bool hexDecode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    int hi = -1;
    for (char ch : s) {
        if (ch == ' ' || ch == ':' || ch == '\n') continue;
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else return false;
        if (hi < 0) hi = v;
        else { out.push_back((uint8_t)(hi * 16 + v)); hi = -1; }
    }
    return hi < 0;
}

std::string hexEncode(const uint8_t* p, size_t n, bool upper) {
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

} // namespace meshcrypto
} // namespace dect2
