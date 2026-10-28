// AES-128-CBC decryption with PKCS#7 padding, for the plain HLS segment encryption (#EXT-X-KEY:METHOD=AES-128, RFC 8216 section 4.3.2.4).
// That is ordinary transport encryption with a key the playlist hands out, not DRM; Widevine / FairPlay / PlayReady / SAMPLE-AES are refused elsewhere.
// Written from FIPS-197; small and table free on purpose (a few MB/s are needed, not hundreds).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dect2::iptv {

class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]) { init(); expand(key); }
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk_[10][i];
        for (int r = 9; r >= 0; r--) {
            invShiftRows(s);
            for (int i = 0; i < 16; i++) s[i] = isb()[s[i]];
            for (int i = 0; i < 16; i++) s[i] ^= rk_[r][i];
            if (r > 0) invMix(s);
        }
        memcpy(out, s, 16);
    }
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const {   // used by the tests
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk_[0][i];
        for (int r = 1; r <= 10; r++) {
            for (int i = 0; i < 16; i++) s[i] = sb()[s[i]];
            shiftRows(s);
            if (r < 10) mix(s);
            for (int i = 0; i < 16; i++) s[i] ^= rk_[r][i];
        }
        memcpy(out, s, 16);
    }
private:
    uint8_t rk_[11][16];
    static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
    struct Tabs { uint8_t sb[256], isb[256]; };
    static const Tabs& tabs() {
        static const Tabs t = [] {
            Tabs t{};
            uint8_t p = 1, q = 1;
            do {   // generate the S-box from the multiplicative inverse (the usual loop from the AES literature)
                p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
                q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4); if (q & 0x80) q ^= 0x09;
                const uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
                t.sb[p] = (uint8_t)(x ^ 0x63);
            } while (p != 1);
            t.sb[0] = 0x63;
            for (int i = 0; i < 256; i++) t.isb[t.sb[i]] = (uint8_t)i;
            return t;
        }();
        return t;
    }
    static const uint8_t* sb() { return tabs().sb; }
    static const uint8_t* isb() { return tabs().isb; }
    static void init() { (void)tabs(); }
    void expand(const uint8_t* key) {
        uint8_t w[176];
        memcpy(w, key, 16);
        uint8_t rc = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
            if (i % 16 == 0) { const uint8_t a = t[0]; t[0] = (uint8_t)(sb()[t[1]] ^ rc); t[1] = sb()[t[2]]; t[2] = sb()[t[3]]; t[3] = sb()[a]; rc = xt(rc); }
            for (int k = 0; k < 4; k++) w[i + k] = (uint8_t)(w[i - 16 + k] ^ t[k]);
        }
        for (int r = 0; r < 11; r++) memcpy(rk_[r], w + 16 * r, 16);
    }
    static void shiftRows(uint8_t* s) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
        memcpy(s, t, 16);
    }
    static void invShiftRows(uint8_t* s) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
        memcpy(s, t, 16);
    }
    static void mix(uint8_t* s) {
        for (int c = 0; c < 4; c++) {
            uint8_t* a = s + 4 * c; const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = (uint8_t)(xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3);
            a[1] = (uint8_t)(a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3);
            a[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3));
            a[3] = (uint8_t)((xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3));
        }
    }
    static void invMix(uint8_t* s) {
        for (int c = 0; c < 4; c++) {
            uint8_t* a = s + 4 * c; const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = (uint8_t)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
            a[1] = (uint8_t)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
            a[2] = (uint8_t)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
            a[3] = (uint8_t)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
        }
    }
};

// CBC decrypt in place of a copy; strips the PKCS#7 padding. False when the length is not a multiple of 16 or the padding is wrong.
inline bool aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], std::vector<uint8_t>& data) {
    if (data.empty() || data.size() % 16) return false;
    Aes128 a(key);
    uint8_t prev[16], cur[16], out[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i < data.size(); i += 16) {
        memcpy(cur, &data[i], 16);
        a.decryptBlock(cur, out);
        for (int k = 0; k < 16; k++) data[i + k] = (uint8_t)(out[k] ^ prev[k]);
        memcpy(prev, cur, 16);
    }
    const uint8_t pad = data.back();
    if (pad == 0 || pad > 16 || pad > data.size()) return false;
    for (size_t k = 0; k < pad; k++) if (data[data.size() - 1 - k] != pad) return false;
    data.resize(data.size() - pad);
    return true;
}

} // namespace dect2::iptv
