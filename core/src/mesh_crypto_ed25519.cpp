// Ed25519 (RFC 8032 5.1) on the twisted Edwards curve of Curve25519. Field elements are 16 limbs of 16 bits in int64_t
// (same layout idea as TweetNaCl, written out here); the curve constant d, sqrt(-1) and the base point are derived at
// start from their definitions, so no long constants are typed in. Variable-time: only for public data and test keys.
#include "dect2/mesh_crypto.h"
#include <cstring>
#include <vector>

namespace dect2 {
namespace meshcrypto {

namespace {

typedef int64_t Fe[16];

void fcopy(Fe o, const Fe a) { for (int i = 0; i < 16; i++) o[i] = a[i]; }
void fset(Fe o, int64_t v) { for (int i = 0; i < 16; i++) o[i] = 0; o[0] = v; }

// carry: 2^256 = 38 (mod 2^255 - 19)
void fcar(Fe o) {
    for (int i = 0; i < 16; i++) {
        const int64_t c = o[i] >> 16;
        o[i] -= c * 65536;
        if (i < 15) o[i + 1] += c; else o[0] += 38 * c;
    }
}
void fadd(Fe o, const Fe a, const Fe b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
void fsub(Fe o, const Fe a, const Fe b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
void fmul(Fe o, const Fe a, const Fe b) {
    int64_t t[31] = {};
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    fcar(o);
    fcar(o);
}
void fsq(Fe o, const Fe a) { fmul(o, a, a); }

void fpack(uint8_t out[32], const Fe a) {
    Fe t, m;
    fcopy(t, a);
    fcar(t); fcar(t); fcar(t);
    for (int pass = 0; pass < 2; pass++) {                    // subtract p = 2^255 - 19 while the value is >= p
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int64_t borrow = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        m[15] &= 0xffff;
        if (!borrow) fcopy(t, m);
    }
    for (int i = 0; i < 16; i++) { out[2 * i] = (uint8_t)(t[i] & 0xff); out[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
void funpack(Fe o, const uint8_t in[32]) {
    for (int i = 0; i < 16; i++) o[i] = in[2 * i] + ((int64_t)in[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
bool fequal(const Fe a, const Fe b) {
    uint8_t x[32], y[32];
    fpack(x, a); fpack(y, b);
    return std::memcmp(x, y, 32) == 0;
}
int fparity(const Fe a) { uint8_t x[32]; fpack(x, a); return x[0] & 1; }

// a^(p-2)
void finv(Fe o, const Fe a) {
    Fe c;
    fcopy(c, a);
    for (int i = 253; i >= 0; i--) { fsq(c, c); if (i != 2 && i != 4) fmul(c, c, a); }
    fcopy(o, c);
}
// a^((p-5)/8)
void fpow2523(Fe o, const Fe a) {
    Fe c;
    fcopy(c, a);
    for (int i = 250; i >= 0; i--) { fsq(c, c); if (i != 1) fmul(c, c, a); }
    fcopy(o, c);
}

struct Point { Fe x, y, z, t; };

struct Consts {
    Fe d2;        // 2*d
    Fe sqrtm1;    // sqrt(-1)
    Fe dd;        // d
    Point base;
    Consts();
};

void padd(Point& r, const Point& p, const Point& q, const Fe d2) {
    Fe a, b, c, d, e, f, g, h, t1, t2;
    fsub(t1, p.y, p.x); fsub(t2, q.y, q.x); fmul(a, t1, t2);
    fadd(t1, p.y, p.x); fadd(t2, q.y, q.x); fmul(b, t1, t2);
    fmul(t1, p.t, q.t); fmul(c, t1, d2);
    fmul(t1, p.z, q.z); fadd(d, t1, t1);
    fsub(e, b, a); fsub(f, d, c); fadd(g, d, c); fadd(h, b, a);
    fmul(r.x, e, f); fmul(r.y, h, g); fmul(r.z, g, f); fmul(r.t, e, h);
}

void pencode(uint8_t out[32], const Point& p) {
    Fe zi, x, y;
    finv(zi, p.z);
    fmul(x, p.x, zi);
    fmul(y, p.y, zi);
    fpack(out, y);
    out[31] = (uint8_t)(out[31] ^ (fparity(x) << 7));
}

// false: not a point on the curve
bool pdecode(Point& r, const uint8_t in[32], const Consts& k) {
    Fe y, u, v, v3, x, t, chk, one;
    uint8_t buf[32];
    std::memcpy(buf, in, 32);
    const int sign = buf[31] >> 7;
    buf[31] &= 0x7f;
    funpack(y, buf);
    fset(one, 1);
    fsq(u, y);                       // y^2
    fmul(v, u, k.dd);
    fadd(v, v, one);                 // d*y^2 + 1
    fsub(u, u, one);                 // y^2 - 1
    fsq(v3, v); fmul(v3, v3, v);     // v^3
    fsq(t, v3); fmul(t, t, v);       // v^7
    fmul(t, t, u);                   // u*v^7
    fpow2523(t, t);
    fmul(x, t, v3); fmul(x, x, u);   // x = u*v^3*(u*v^7)^((p-5)/8)
    fsq(chk, x); fmul(chk, chk, v);  // v*x^2
    if (!fequal(chk, u)) {
        fmul(x, x, k.sqrtm1);
        fsq(chk, x); fmul(chk, chk, v);
        if (!fequal(chk, u)) return false;
    }
    if (fparity(x) != sign) { Fe z; fset(z, 0); fsub(x, z, x); }
    fcopy(r.x, x); fcopy(r.y, y); fset(r.z, 1); fmul(r.t, x, y);
    // x = 0 with the sign bit set is not a valid encoding
    Fe zero; fset(zero, 0);
    if (fequal(x, zero) && sign) return false;
    return true;
}

Consts::Consts() {
    Fe a, b, t;
    fset(a, 121665); fset(b, 121666);
    finv(b, b);
    fmul(t, a, b);                   // 121665/121666
    Fe z; fset(z, 0);
    fsub(dd, z, t);                  // d = -121665/121666
    fadd(d2, dd, dd);
    Fe two; fset(two, 2);
    fpow2523(t, two); fsq(t, t); fmul(sqrtm1, t, two);   // 2^((p-1)/4)
    // base point: y = 4/5, x even (RFC 8032 5.1)
    uint8_t enc[32];
    std::memset(enc, 0x66, 32);
    enc[0] = 0x58;
    pdecode(base, enc, *this);
}

const Consts& consts() {
    static const Consts k;
    return k;
}

// r = s*p, s little-endian bits
void pmul(Point& r, const Point& p, const uint8_t s[32], const Consts& k) {
    Point acc;
    fset(acc.x, 0); fset(acc.y, 1); fset(acc.z, 1); fset(acc.t, 0);
    for (int i = 255; i >= 0; i--) {
        Point t;
        padd(t, acc, acc, k.d2);
        acc = t;
        if ((s[i >> 3] >> (i & 7)) & 1) { padd(t, acc, p, k.d2); acc = t; }
    }
    r = acc;
}

// group order L = 2^252 + 27742317777372353535851937790883648493, little-endian
const uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

bool geL(const uint8_t r[32]) {
    for (int i = 31; i >= 0; i--) { if (r[i] != kL[i]) return r[i] > kL[i]; }
    return true;
}
void subL(uint8_t r[32]) {
    int borrow = 0;
    for (int i = 0; i < 32; i++) {
        const int v = (int)r[i] - kL[i] - borrow;
        r[i] = (uint8_t)(v & 0xff);
        borrow = v < 0;
    }
}
// 64-byte little-endian number mod L, bit by bit
void reduceL(const uint8_t in[64], uint8_t out[32]) {
    uint8_t r[32] = {};
    for (int i = 511; i >= 0; i--) {
        int carry = (in[i >> 3] >> (i & 7)) & 1;
        for (int k = 0; k < 32; k++) { const int v = (r[k] << 1) | carry; r[k] = (uint8_t)v; carry = v >> 8; }
        if (geL(r)) subL(r);
    }
    std::memcpy(out, r, 32);
}

void clampScalar(uint8_t a[32]) { a[0] &= 248; a[31] &= 127; a[31] |= 64; }

} // namespace

void ed25519PublicKey(const uint8_t seed[32], uint8_t pub[32]) {
    const Consts& k = consts();
    uint8_t h[64];
    sha512(seed, 32, h);
    clampScalar(h);
    Point p;
    pmul(p, k.base, h, k);
    pencode(pub, p);
}

void ed25519Sign(const uint8_t seed[32], const uint8_t* msg, size_t n, uint8_t sig[64]) {
    const Consts& k = consts();
    uint8_t h[64], pub[32];
    sha512(seed, 32, h);
    uint8_t a[32];
    std::memcpy(a, h, 32);
    clampScalar(a);
    Point pa;
    pmul(pa, k.base, a, k);
    pencode(pub, pa);

    std::vector<uint8_t> buf(32 + n);
    std::memcpy(buf.data(), h + 32, 32);
    if (n) std::memcpy(buf.data() + 32, msg, n);
    uint8_t rh[64], r[32];
    sha512(buf.data(), buf.size(), rh);
    reduceL(rh, r);
    Point pr;
    pmul(pr, k.base, r, k);
    pencode(sig, pr);

    buf.assign(64 + n, 0);
    std::memcpy(buf.data(), sig, 32);
    std::memcpy(buf.data() + 32, pub, 32);
    if (n) std::memcpy(buf.data() + 64, msg, n);
    uint8_t kh[64], kr[32];
    sha512(buf.data(), buf.size(), kh);
    reduceL(kh, kr);

    // S = (r + kr*a) mod L: schoolbook product in 16-bit accumulators, then reduce
    uint32_t acc[64] = {};
    for (int i = 0; i < 32; i++) for (int j = 0; j < 32; j++) acc[i + j] += (uint32_t)kr[i] * a[j];
    for (int i = 0; i < 32; i++) acc[i] += r[i];
    uint8_t prod[64];
    uint32_t carry = 0;
    for (int i = 0; i < 64; i++) { const uint32_t v = acc[i] + carry; prod[i] = (uint8_t)v; carry = v >> 8; }
    reduceL(prod, sig + 32);
}

bool ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pub[32]) {
    const Consts& k = consts();
    if (geL(sig + 32)) return false;                           // S must be < L
    Point pa;
    if (!pdecode(pa, pub, k)) return false;
    Fe z; fset(z, 0);
    fsub(pa.x, z, pa.x);                                        // -A
    fsub(pa.t, z, pa.t);

    std::vector<uint8_t> buf(64 + n);
    std::memcpy(buf.data(), sig, 32);
    std::memcpy(buf.data() + 32, pub, 32);
    if (n) std::memcpy(buf.data() + 64, msg, n);
    uint8_t kh[64], kr[32];
    sha512(buf.data(), buf.size(), kh);
    reduceL(kh, kr);

    Point sb, ha, sum;
    pmul(sb, k.base, sig + 32, k);
    pmul(ha, pa, kr, k);
    padd(sum, sb, ha, k.d2);
    uint8_t enc[32];
    pencode(enc, sum);
    return std::memcmp(enc, sig, 32) == 0;
}

} // namespace meshcrypto
} // namespace dect2
