// Packet layers of the radio-control and mesh links; see rc_proto.h and docs/MESH-RC-3.8.md.
#include "dect2/rclink/rc_proto.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace rclink {

const char* protoName(int p) {
    switch (p) {
    case Reticulum: return "Reticulum";
    case Elrs: return "ExpressLRS";
    case Sik: return "MAVLink / SiK";
    case Frsky: return "FrSky";
    case Flysky: return "FlySky";
    }
    return "?";
}

std::string hexOf(const uint8_t* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

static std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char t[160];
    std::snprintf(t, sizeof t, f, a, b, c);
    return t;
}

// ---------------------------------------------------------------- Reticulum

bool rnodeUnframe(const uint8_t* p, size_t n, RnodeReassembly& st, std::vector<uint8_t>& packet) {
    packet.clear();
    if (n < 2) return false;
    const int seq = p[0] >> 4;
    const bool split = p[0] & 1;
    if (split) {
        if (st.seq == seq && !st.first.empty()) {                 // second part
            packet = st.first;
            packet.insert(packet.end(), p + 1, p + n);
            st.seq = -1; st.first.clear();
            return true;
        }
        st.seq = seq;                                             // first part
        st.first.assign(p + 1, p + n);
        return false;
    }
    st.seq = -1; st.first.clear();
    packet.assign(p + 1, p + n);
    return true;
}

Decoded reticulumParse(const uint8_t* p, size_t n) {
    Decoded d;
    d.proto = Reticulum;
    if (n < 19) return d;
    const int flags = p[0], hops = p[1];
    const int hdr = (flags >> 6) & 1, ctxFlag = (flags >> 5) & 1, transport = (flags >> 4) & 1, dstType = (flags >> 2) & 3, pktType = flags & 3;
    if (flags & 0x80) return d;                                   // the IFAC bit: an authenticated interface, nothing readable without its key
    if (hops >= 128) return d;                                    // Transport.PATHFINDER_M
    const size_t addr = hdr ? 32 : 16;
    if (n < 2 + addr + 1 + 1) return d;
    const uint8_t* dest = p + 2 + (hdr ? 16 : 0);
    const int context = p[2 + addr];
    const uint8_t* data = p + 2 + addr + 1;
    const size_t dn = n - (2 + addr + 1);
    static const char* const kTypes[4] = {"DATA", "ANNOUNCE", "LINKREQUEST", "PROOF"};
    static const char* const kDst[4] = {"single", "group", "plain", "link"};
    d.ok = true;
    d.id = hexOf(dest, 16);
    d.type = kTypes[pktType];
    d.detail = std::string(kDst[dstType]) + fmt(", %.0f hops, %.0f bytes", hops, (double)dn);
    if (hdr && transport) d.detail += ", via " + hexOf(p + 2, 16).substr(0, 8);
    if (pktType == 0 && dstType != 2) d.encrypted = true;         // single, group and link traffic is encrypted to the destination
    if (pktType == 1) {
        const size_t ratchet = ctxFlag ? 32 : 0, fixed = 64 + 10 + 10 + ratchet + 64;
        if (dn < fixed) { d.ok = false; return d; }
        const uint8_t* pub = data, *nameHash = data + 64, *random = data + 74, *rat = data + 84, *sig = data + 84 + ratchet;
        const uint8_t* app = data + fixed;
        const size_t an = dn - fixed;
        std::vector<uint8_t> signedData(dest, dest + 16);
        signedData.insert(signedData.end(), pub, pub + 64);
        signedData.insert(signedData.end(), nameHash, nameHash + 10);
        signedData.insert(signedData.end(), random, random + 10);
        signedData.insert(signedData.end(), rat, rat + ratchet);
        signedData.insert(signedData.end(), app, app + an);
        if (!meshcrypto::ed25519Verify(sig, signedData.data(), signedData.size(), pub + 32)) { d.ok = false; return d; }
        uint8_t identity[32], check[32];
        meshcrypto::sha256(pub, 64, identity);
        uint8_t material[26];
        std::memcpy(material, nameHash, 10);
        std::memcpy(material + 10, identity, 16);
        meshcrypto::sha256(material, 26, check);
        if (std::memcmp(check, dest, 16) != 0) { d.ok = false; return d; }
        d.detail = "identity " + hexOf(identity, 16).substr(0, 16) + fmt(", %.0f hops, signature good", hops);
        if (an) {
            std::string text;
            for (size_t i = 0; i < an && i < 40; i++) if (app[i] >= 32 && app[i] < 127) text += (char)app[i];
            if (text.size() >= 3) d.detail += ", \"" + text + "\"";
        }
    } else if (context) {
        char t[32];
        std::snprintf(t, sizeof t, ", context 0x%02X", (unsigned)context);
        d.detail += t;
    }
    return d;
}

std::vector<uint8_t> reticulumAnnounce(const uint8_t seed[32], const uint8_t x25519Pub[32], const std::string& fullName, const uint8_t randomHash[10],
                                       const std::vector<uint8_t>& appData, int hops, std::array<uint8_t, 16>* destOut) {
    uint8_t edPub[32], pub[64], identity[32], nameHash[32], dest[32], sig[64];
    meshcrypto::ed25519PublicKey(seed, edPub);
    std::memcpy(pub, x25519Pub, 32);
    std::memcpy(pub + 32, edPub, 32);
    meshcrypto::sha256(pub, 64, identity);
    meshcrypto::sha256((const uint8_t*)fullName.data(), fullName.size(), nameHash);
    uint8_t material[26];
    std::memcpy(material, nameHash, 10);
    std::memcpy(material + 10, identity, 16);
    meshcrypto::sha256(material, 26, dest);
    if (destOut) std::memcpy(destOut->data(), dest, 16);
    std::vector<uint8_t> sd(dest, dest + 16);
    sd.insert(sd.end(), pub, pub + 64);
    sd.insert(sd.end(), nameHash, nameHash + 10);
    sd.insert(sd.end(), randomHash, randomHash + 10);
    sd.insert(sd.end(), appData.begin(), appData.end());
    meshcrypto::ed25519Sign(seed, sd.data(), sd.size(), sig);
    std::vector<uint8_t> out = {0x01, (uint8_t)hops};             // HEADER_1, broadcast, single destination, announce
    out.insert(out.end(), dest, dest + 16);
    out.push_back(0);
    out.insert(out.end(), pub, pub + 64);
    out.insert(out.end(), nameHash, nameHash + 10);
    out.insert(out.end(), randomHash, randomHash + 10);
    out.insert(out.end(), sig, sig + 64);
    out.insert(out.end(), appData.begin(), appData.end());
    return out;
}

// ---------------------------------------------------------------- ExpressLRS

namespace {
void md5(const uint8_t* msg, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    std::vector<uint8_t> m(msg, msg + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m.push_back((uint8_t)(bits >> (8 * i)));
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4 * i] | (uint32_t)m[off + 4 * i + 1] << 8 | (uint32_t)m[off + 4 * i + 2] << 16 | (uint32_t)m[off + 4 * i + 3] << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = d;
            d = c; c = b;
            const uint32_t x = a + f + K[i] + w[g];
            const int s = S[(i / 16) * 4 + i % 4];
            b = b + ((x << s) | (x >> (32 - s)));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (8 * j));
}
} // namespace

std::array<uint8_t, 6> elrsUidFromPhrase(const std::string& phrase) {
    const std::string define = "-DMY_BINDING_PHRASE=\"" + phrase + "\"";
    uint8_t d[16];
    md5((const uint8_t*)define.data(), define.size(), d);
    std::array<uint8_t, 6> u;
    std::copy(d, d + 6, u.begin());
    return u;
}

uint16_t elrsCrc(int bits, uint16_t poly, const uint8_t* p, size_t n, uint16_t crc) {
    const uint32_t mask = (1u << bits) - 1, high = 1u << (bits - 1);
    uint32_t c = crc;
    for (size_t i = 0; i < n; i++) {
        uint32_t t = (((c >> (bits - 8)) ^ p[i]) & 0xFF) << (bits - 8);
        for (int j = 0; j < 8; j++) t = (t << 1) ^ ((t & high) ? poly : 0);
        c = (c << 8) ^ t;
    }
    return (uint16_t)(c & mask);
}

uint16_t elrsCrcInit(uint8_t uid4, uint8_t uid5) { return (uint16_t)((((unsigned)uid4 << 8) | uid5) ^ ((unsigned)kElrsOtaVersion << 8)); }

static bool elrsCrcOk(const uint8_t* p, size_t n, uint16_t init) {
    if (n == kElrsOta4) {
        uint8_t m[7];
        std::memcpy(m, p, 7);
        m[0] &= 3;
        const uint16_t c = ((uint16_t)(p[0] >> 2) << 8) | p[7];
        return elrsCrc(14, kElrsCrc14Poly, m, 7, init) == c;
    }
    const uint16_t c = (uint16_t)p[11] | ((uint16_t)p[12] << 8);
    return elrsCrc(16, kElrsCrc16Poly, p, 11, init) == c;
}

Decoded elrsParse(const uint8_t* p, size_t n, ElrsLink& link) {
    Decoded d;
    d.proto = Elrs;
    if (n != kElrsOta4 && n != kElrsOta8) return d;
    const int type = p[0] & 3;
    char id[16];
    if (type == 2) {                                              // SYNC: carries UID4 / UID5 itself
        const uint8_t* s = p + 1;
        const uint8_t u4 = s[4], u5 = s[5];
        if (!elrsCrcOk(p, n, elrsCrcInit(u4, u5))) return d;
        if (link.phrase && (link.uid[4] != u4 || link.uid[5] != u5)) { /* another link: keep the phrase link, show this one as structure */ }
        else { link.known = true; link.uid4 = u4; link.uid5 = u5; }
        d.ok = true;
        std::snprintf(id, sizeof id, "%02X%02X", u4, u5);
        d.id = id;
        d.type = "SYNC";
        d.detail = fmt("hop %.0f, nonce %.0f, rate index %.0f", s[0], s[1], s[2]);
        return d;
    }
    if (!link.known) return d;                                    // no link to check it against: only activity
    const uint16_t init = elrsCrcInit(link.uid4, link.uid5);
    int nonce = -1;
    for (int k = 0; k < 256 && nonce < 0; k++) if (elrsCrcOk(p, n, (uint16_t)(init ^ k))) nonce = k;
    if (nonce < 0) return d;
    d.ok = true;
    std::snprintf(id, sizeof id, "%02X%02X", link.uid4, link.uid5);
    d.id = id;
    d.type = type == 1 ? "DATA" : "RC or link stats";
    d.detail = fmt("%.0f bytes, nonce %.0f", (double)n, nonce);
    d.encrypted = !link.phrase;
    if (link.phrase && type == 0) {
        const uint8_t* raw = n == kElrsOta4 ? p + 1 : p + 1;
        uint32_t ch[4] = {};
        uint32_t acc = 0; int have = 0, idx = 0;
        for (int b = 0; b < 5 && idx < 4; b++) {
            acc |= (uint32_t)raw[b] << have;
            have += 8;
            while (have >= 10 && idx < 4) { ch[idx++] = acc & 1023; acc >>= 10; have -= 10; }
        }
        char t[120];
        std::snprintf(t, sizeof t, "ch1-4 %u %u %u %u, armed %d", ch[0], ch[1], ch[2], ch[3], n == kElrsOta4 ? (p[6] >> 7) & 1 : (p[0] >> 7) & 1);
        d.detail = t;
    }
    return d;
}

// ---------------------------------------------------------------- MAVLink

namespace {
struct MavRow { uint32_t id; int extra; const char* name; };
const MavRow kMav[] = {
#include "rc_mav_table.inc"
};
const MavRow* mavRow(uint32_t id) {
    size_t lo = 0, hi = sizeof kMav / sizeof kMav[0];
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (kMav[m].id < id) lo = m + 1; else hi = m;
    }
    return lo < sizeof kMav / sizeof kMav[0] && kMav[lo].id == id ? &kMav[lo] : nullptr;
}
int32_t le32(const uint8_t* p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }
int16_t le16(const uint8_t* p) { return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
float lef(const uint8_t* p) { uint32_t v = (uint32_t)le32(p); float f; std::memcpy(&f, &v, 4); return f; }

void mavDescribe(uint32_t id, const std::vector<uint8_t>& pl, Decoded& d) {
    const size_t n = pl.size();
    const uint8_t* p = pl.data();
    char t[160];
    t[0] = 0;
    std::vector<uint8_t> z;           // v2 trims trailing zero bytes: put them back
    size_t need = 0;
    if (id == 0) need = 9; else if (id == 1) need = 31; else if (id == 24) need = 30; else if (id == 30) need = 28; else if (id == 33) need = 28; else if (id == 74) need = 20;
    else if (id == 253) need = 51;
    if (n < need) { z.assign(p, p + n); z.resize(need, 0); p = z.data(); }
    switch (id) {
    case 0: std::snprintf(t, sizeof t, "type %d, autopilot %d, base mode 0x%02X, status %d, armed %d", p[4], p[5], p[6], p[7], (p[6] >> 7) & 1); break;
    case 1: std::snprintf(t, sizeof t, "battery %.2f V, %d %%", (double)(uint16_t)le16(p + 14) / 1000.0, (int)(int8_t)p[30]); break;
    case 24: {
        const double lat = le32(p + 8) * 1e-7, lon = le32(p + 12) * 1e-7, alt = le32(p + 16) * 1e-3;
        std::snprintf(t, sizeof t, "fix %d, %d sats, %.6f %.6f, %.0f m", p[28], p[29], lat, lon, alt);
        if (p[28] >= 2 && lat != 0) { d.hasPos = true; d.lat = lat; d.lon = lon; d.altM = alt; }
        break;
    }
    case 30: std::snprintf(t, sizeof t, "roll %.1f, pitch %.1f, yaw %.1f deg", lef(p + 4) * 57.29578, lef(p + 8) * 57.29578, lef(p + 12) * 57.29578); break;
    case 33: {
        const double lat = le32(p + 4) * 1e-7, lon = le32(p + 8) * 1e-7, alt = le32(p + 12) * 1e-3, rel = le32(p + 16) * 1e-3;
        std::snprintf(t, sizeof t, "%.6f %.6f, %.1f m (%.1f m above home)", lat, lon, alt, rel);
        if (lat != 0 || lon != 0) { d.hasPos = true; d.lat = lat; d.lon = lon; d.altM = alt; }
        break;
    }
    case 74: std::snprintf(t, sizeof t, "airspeed %.1f, ground %.1f m/s, alt %.1f m, heading %d", lef(p), lef(p + 4), lef(p + 8), (int)le16(p + 16)); break;
    case 253: {
        std::string s;
        for (int i = 1; i < 51 && p[i]; i++) s += p[i] >= 32 && p[i] < 127 ? (char)p[i] : '?';
        d.detail = "\"" + s + "\"";
        return;
    }
    default: std::snprintf(t, sizeof t, "%zu bytes", n);
    }
    d.detail = t;
}
} // namespace

int mavCrcExtra(uint32_t id) { const MavRow* r = mavRow(id); return r ? r->extra : -1; }
const char* mavName(uint32_t id) { const MavRow* r = mavRow(id); return r ? r->name : nullptr; }

uint16_t mavX25(const uint8_t* p, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; i++) {
        uint8_t t = (uint8_t)(p[i] ^ (crc & 0xFF));
        t = (uint8_t)(t ^ (t << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)t << 8) ^ ((uint16_t)t << 3) ^ (t >> 4));
    }
    return crc;
}

std::vector<Decoded> mavlinkScan(const uint8_t* p, size_t n, size_t* used) {
    std::vector<Decoded> out;
    size_t i = 0, keep = n;
    while (i < n) {
        if (p[i] != 0xFE && p[i] != 0xFD) { i++; continue; }
        const bool v2 = p[i] == 0xFD;
        if (i + 2 > n) { keep = i; break; }
        const size_t len = p[i + 1];
        const size_t hdr = v2 ? 10 : 6;
        if (i + hdr > n) { keep = i; break; }
        const bool sign = v2 && (p[i + 2] & 1);
        const size_t total = hdr + len + 2 + (sign ? 13 : 0);
        if (i + total > n) { keep = i; break; }
        const uint32_t id = v2 ? ((uint32_t)p[i + 7] | (uint32_t)p[i + 8] << 8 | (uint32_t)p[i + 9] << 16) : p[i + 5];
        const int extra = mavCrcExtra(id);
        bool good = false;
        if (extra >= 0) {
            uint16_t c = mavX25(p + i + 1, hdr - 1 + len);
            const uint8_t e = (uint8_t)extra;
            c = mavX25(&e, 1, c);
            good = c == (uint16_t)(p[i + hdr + len] | p[i + hdr + len + 1] << 8);
        }
        if (!good) { i++; continue; }
        Decoded d;
        d.ok = true;
        d.proto = Sik;
        const int sys = p[i + (v2 ? 5 : 3)], comp = p[i + (v2 ? 6 : 4)];
        d.id = "sys " + std::to_string(sys) + " / " + std::to_string(comp);
        d.type = mavName(id) ? mavName(id) : "?";
        mavDescribe(id, std::vector<uint8_t>(p + i + hdr, p + i + hdr + len), d);
        if (sign) d.detail += ", signed";
        out.push_back(std::move(d));
        i += total;
    }
    if (used) *used = keep;
    return out;
}

std::vector<uint8_t> mavlinkFrame(int version, uint32_t id, int seq, int sys, int comp, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f;
    if (version == 2) {
        f = {0xFD, (uint8_t)payload.size(), 0, 0, (uint8_t)seq, (uint8_t)sys, (uint8_t)comp, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16)};
    } else {
        f = {0xFE, (uint8_t)payload.size(), (uint8_t)seq, (uint8_t)sys, (uint8_t)comp, (uint8_t)id};
    }
    f.insert(f.end(), payload.begin(), payload.end());
    uint16_t c = mavX25(f.data() + 1, f.size() - 1);
    const uint8_t e = (uint8_t)mavCrcExtra(id);
    c = mavX25(&e, 1, c);
    f.push_back((uint8_t)c);
    f.push_back((uint8_t)(c >> 8));
    return f;
}

// ---------------------------------------------------------------- SiK

uint16_t sikCrc16(const uint8_t* p, size_t n) {                  // crc.c: CRC-CCITT tables, the message byte shifted into the low end, no augmentation, init 0
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t t = (uint16_t)(c & 0xFF00);
        for (int b = 0; b < 8; b++) t = (t & 0x8000) ? (uint16_t)((t << 1) ^ 0x1021) : (uint16_t)(t << 1);
        c = (uint16_t)(((c << 8) | p[i]) ^ t);
    }
    return c;
}

namespace {
uint16_t golayCheck(unsigned v) {                                // Golay (23,12), generator 0xC75 (checked against every entry of golay23.h)
    uint32_t val = (uint32_t)v << 11;
    for (int i = 22; i >= 11; i--) if (val >> i & 1) val ^= (uint32_t)0xC75 << (i - 11);
    return (uint16_t)val;
}
unsigned golayFix(unsigned v, unsigned check, int* count) {      // v 12 bits, check 11 bits: up to 3 bit errors, brute force over the error patterns of weight <= 3
    if (golayCheck(v) == check) return v;
    const uint32_t word = ((uint32_t)v << 11) | check;
    for (int a = 0; a < 23; a++) {
        const uint32_t w1 = word ^ (1u << a);
        if (golayCheck(w1 >> 11) == (w1 & 0x7FF)) { (*count)++; return w1 >> 11; }
    }
    for (int a = 0; a < 23; a++) for (int b = a + 1; b < 23; b++) {
        const uint32_t w2 = word ^ (1u << a) ^ (1u << b);
        if (golayCheck(w2 >> 11) == (w2 & 0x7FF)) { (*count)++; return w2 >> 11; }
    }
    for (int a = 0; a < 23; a++) for (int b = a + 1; b < 23; b++) for (int c = b + 1; c < 23; c++) {
        const uint32_t w3 = word ^ (1u << a) ^ (1u << b) ^ (1u << c);
        if (golayCheck(w3 >> 11) == (w3 & 0x7FF)) { (*count)++; return w3 >> 11; }
    }
    return v;
}
void golay3to6(const uint8_t g3[3], uint8_t g6[6]) {             // golay_encode24 of golay.c
    unsigned v = g3[0] | ((unsigned)g3[1] & 0x0F) << 8;
    unsigned s = golayCheck(v);
    g6[0] = s & 0xFF;
    g6[1] = (uint8_t)((g3[0] & 0x1F) << 3 | s >> 8);
    g6[2] = (uint8_t)((g3[0] & 0xE0) >> 5 | (g3[1] & 0x0F) << 3);
    v = g3[2] | ((unsigned)g3[1] & 0xF0) << 4;
    s = golayCheck(v);
    g6[3] = s & 0xFF;
    g6[4] = (uint8_t)((g3[2] & 0x1F) << 3 | s >> 8);
    g6[5] = (uint8_t)((g3[2] & 0xE0) >> 5 | (g3[1] & 0xF0) >> 1);
}
void golay6to3(const uint8_t g6[6], uint8_t g3[3], int* corrected) {
    unsigned v = (unsigned)(g6[2] & 0x7F) << 5 | (g6[1] & 0xF8) >> 3;
    unsigned chk = g6[0] | ((unsigned)g6[1] & 0x07) << 8;
    v = golayFix(v, chk, corrected);
    g3[0] = v & 0xFF;
    g3[1] = (uint8_t)(v >> 8);
    v = (unsigned)(g6[5] & 0x7F) << 5 | (g6[4] & 0xF8) >> 3;
    chk = g6[3] | ((unsigned)g6[4] & 0x07) << 8;
    v = golayFix(v, chk, corrected);
    g3[1] |= (uint8_t)((v >> 4) & 0xF0);
    g3[2] = v & 0xFF;
}
} // namespace

void sikGolayEncode(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    for (size_t i = 0; i + 3 <= n; i += 3) {
        uint8_t g[6];
        golay3to6(in + i, g);
        out.insert(out.end(), g, g + 6);
    }
}

bool sikGolayDecode(const uint8_t* in, size_t n, std::vector<uint8_t>& out, int* corrected) {
    if (n % 6) return false;
    out.clear();
    int c = 0;
    for (size_t i = 0; i < n; i += 6) {
        uint8_t g[3];
        golay6to3(in + i, g, &c);
        out.insert(out.end(), g, g + 3);
    }
    if (corrected) *corrected = c;
    return true;
}

static void sikTrailer(SikPacket& s) {
    if (s.data.size() < 2) { s.ok = false; return; }
    const unsigned t = (unsigned)s.data[s.data.size() - 2] | (unsigned)s.data.back() << 8;
    s.window = t & 0x1FFF;
    s.command = (t >> 13) & 1;
    s.bonus = (t >> 14) & 1;
    s.resend = (t >> 15) & 1;
    s.data.resize(s.data.size() - 2);
}

SikPacket sikParse(const uint8_t* b, size_t n) {
    SikPacket s;
    // no ECC: packet handler of the Si443x: 2 header bytes, length, data, CRC-16 (IBM, over header + length + data)
    if (n >= 5) {
        const size_t len = b[2];
        if (len >= 2 && len <= 252 && n >= 3 + len + 2) {
            uint16_t c = 0xFFFF;
            for (size_t i = 0; i < 3 + len; i++) {
                c ^= (uint16_t)b[i] << 8;
                for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x8005) : (uint16_t)(c << 1);
            }
            if (c == (uint16_t)((b[3 + len] << 8) | b[3 + len + 1])) {
                s.ok = true;
                s.netId = b[0] << 8 | b[1];                  // transmit header 3 (high byte) goes first
                s.data.assign(b + 3, b + 3 + len);
                sikTrailer(s);
                return s;
            }
        }
    }
    // ECC (golay.c, radio_transmit_golay): length is the first byte of the Si443x payload; 6 coded bytes net id + length, 6 coded bytes CRC, coded data
    if (n >= 13) {
        const size_t hwLen = b[0];
        if (hwLen >= 12 && (hwLen % 6) == 0 && n >= 1 + hwLen) {
            std::vector<uint8_t> h;
            int cor = 0;
            sikGolayDecode(b + 1, 12, h, &cor);
            const size_t len = h[2];
            if (len >= 2 && len <= 252) {
                std::vector<uint8_t> dec;
                size_t coded = ((len + 2) / 3) * 6;
                if (12 + coded <= hwLen) {
                    int c2 = 0;
                    sikGolayDecode(b + 1 + 12, coded, dec, &c2);
                    dec.resize(len);
                    const uint16_t c = sikCrc16(dec.data(), dec.size());
                    if (((uint16_t)h[3] | (uint16_t)h[4] << 8) == c) {
                        s.ok = true; s.golay = true;
                        s.netId = h[0] | h[1] << 8;
                        s.data = std::move(dec);
                        s.corrected = cor + c2;
                        sikTrailer(s);
                        return s;
                    }
                }
            }
        }
    }
    return s;
}

std::vector<uint8_t> sikBuild(int netId, const std::vector<uint8_t>& data, int window, bool golay) {
    std::vector<uint8_t> d = data;
    d.push_back((uint8_t)window);
    d.push_back((uint8_t)((window >> 8) & 0x1F));
    std::vector<uint8_t> out;
    if (!golay) {
        out = {(uint8_t)(netId >> 8), (uint8_t)netId, (uint8_t)d.size()};
        out.insert(out.end(), d.begin(), d.end());
        uint16_t c = 0xFFFF;
        for (uint8_t v : out) {
            c ^= (uint16_t)v << 8;
            for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x8005) : (uint16_t)(c << 1);
        }
        out.push_back((uint8_t)(c >> 8));
        out.push_back((uint8_t)c);
        return out;
    }
    uint8_t g[3] = {(uint8_t)netId, (uint8_t)(netId >> 8), (uint8_t)d.size()};
    std::vector<uint8_t> a, b, e;
    sikGolayEncode(g, 3, a);
    const uint16_t c = sikCrc16(d.data(), d.size());
    uint8_t g2[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)d.size()};
    sikGolayEncode(g2, 3, b);
    std::vector<uint8_t> padded = d;
    while (padded.size() % 3) padded.push_back(0);
    sikGolayEncode(padded.data(), padded.size(), e);
    out.push_back((uint8_t)(a.size() + b.size() + e.size()));
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    out.insert(out.end(), e.begin(), e.end());
    return out;
}

// ---------------------------------------------------------------- FrSky

uint16_t cc2500Crc(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x8005) : (uint16_t)(c << 1);
    }
    return c;
}

uint16_t frskyXCrc(const uint8_t* p, size_t n, uint16_t c) {
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = (uint8_t)((c >> 8) ^ p[i]);
        static const uint16_t kShort[16] = {0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF, 0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7};
        c = (uint16_t)((c << 8) ^ (kShort[v & 15] ^ (uint16_t)(0x1081 * (v >> 4))));
    }
    return c;
}

Decoded frskyD8Parse(const uint8_t* p, size_t n) {
    Decoded d;
    d.proto = Frsky;
    if (n < 20 || p[0] != 0x11) return d;
    if (cc2500Crc(p, 18) != (uint16_t)((p[18] << 8) | p[19])) return d;
    char t[140];
    d.ok = true;
    if (p[1] == 3 && p[2] == 1) {
        std::snprintf(t, sizeof t, "%02X%02X", p[3], p[4]);
        d.id = t;
        d.type = "D8 bind";
        std::snprintf(t, sizeof t, "hop table index %d", p[5]);
        d.detail = t;
        return d;
    }
    std::snprintf(t, sizeof t, "%02X%02X", p[1], p[2]);
    d.id = t;
    d.type = "D8 channels";
    uint16_t c[8];
    for (int i = 0; i < 4; i++) c[i] = (uint16_t)(p[6 + i] | (uint16_t)((p[10 + (i >> 1)] >> (4 * (i & 1))) & 15) << 8);
    for (int i = 4; i < 8; i++) c[i] = (uint16_t)(p[8 + i] | (uint16_t)((p[16 + ((i - 4) >> 1)] >> (4 * (i & 1))) & 15) << 8);
    std::snprintf(t, sizeof t, "%u %u %u %u %u %u %u %u", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    d.detail = t;
    return d;
}

Decoded frskyXParse(const uint8_t* p, size_t n) {
    Decoded d;
    d.proto = Frsky;
    if (n < 8) return d;
    const size_t len = p[0];
    if (len < 8 || len > 34 || n < len + 1) return d;
    if (frskyXCrc(p + 3, len - 4) != (uint16_t)((p[len - 1] << 8) | p[len])) return d;
    char t[140];
    d.ok = true;
    std::snprintf(t, sizeof t, "%02X%02X", p[1], p[2]);
    d.id = t;
    d.type = p[6] == 3 ? "D16 bind" : "D16 frame";
    std::snprintf(t, sizeof t, "%zu bytes, hop index %d, chanskip %d", len, p[4] & 0x3F, (p[4] >> 6) | (p[5] << 2));
    d.detail = t;
    return d;
}

std::vector<uint8_t> frskyD8Build(uint16_t txId, uint8_t counter, const uint16_t ch[8]) {
    std::vector<uint8_t> p(18, 0);
    p[0] = 0x11;
    p[1] = (uint8_t)(txId >> 8);
    p[2] = (uint8_t)txId;
    p[3] = counter;
    p[5] = 1;
    for (int i = 0; i < 4; i++) {
        p[6 + i] = (uint8_t)ch[i];
        p[10 + (i >> 1)] |= (uint8_t)(((ch[i] >> 8) & 15) << (4 * (i & 1)));
    }
    for (int i = 4; i < 8; i++) {
        p[8 + i] = (uint8_t)ch[i];
        p[16 + ((i - 4) >> 1)] |= (uint8_t)(((ch[i] >> 8) & 15) << (4 * (i & 1)));
    }
    const uint16_t c = cc2500Crc(p.data(), p.size());
    p.push_back((uint8_t)(c >> 8));
    p.push_back((uint8_t)c);
    return p;
}

std::vector<uint8_t> frskyXBuild(uint16_t txId, uint8_t hopIdx, const std::vector<uint8_t>& body) {
    const size_t len = 6 + body.size() + 2;
    std::vector<uint8_t> p = {(uint8_t)len, (uint8_t)(txId >> 8), (uint8_t)txId, 1, hopIdx, 0, 0};
    p.insert(p.end(), body.begin(), body.end());
    const uint16_t c = frskyXCrc(p.data() + 3, len - 4);
    p.push_back((uint8_t)(c >> 8));
    p.push_back((uint8_t)c);
    return p;
}

// ---------------------------------------------------------------- FlySky

Decoded flyskyParse(const uint8_t* p, size_t n) {
    Decoded d;
    d.proto = Flysky;
    char t[150];
    if (n >= 21 && (p[0] == 0x55 || p[0] == 0xAA)) {
        uint16_t c[8];
        bool plausible = true;
        for (int i = 0; i < 8; i++) {
            c[i] = (uint16_t)(p[5 + 2 * i] | p[6 + 2 * i] << 8);
            if (p[0] == 0x55 && (c[i] < 900 || c[i] > 2200)) plausible = false;
        }
        if (!plausible) return d;
        d.ok = true;
        std::snprintf(t, sizeof t, "%02X%02X%02X%02X", p[4], p[3], p[2], p[1]);
        d.id = t;
        d.type = p[0] == 0x55 ? "AFHDS channels" : "AFHDS bind";
        if (p[0] == 0x55) {
            std::snprintf(t, sizeof t, "%u %u %u %u %u %u %u %u", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
            d.detail = t;
        }
        return d;
    }
    if (n >= 38 && (p[0] == 0x58 || p[0] == 0x56 || p[0] == 0xBB || p[0] == 0xBC || p[0] == 0xAA || p[0] == 0xAC)) {
        if (p[0] == 0x58) {
            uint16_t c[14];
            for (int i = 0; i < 14; i++) {
                c[i] = (uint16_t)(p[9 + 2 * i] | (p[10 + 2 * i] & 15) << 8);
                if (c[i] < 800 || c[i] > 2300) return d;
            }
            std::snprintf(t, sizeof t, "%u %u %u %u %u %u %u %u ...", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
            d.detail = t;
        }
        d.ok = true;
        std::snprintf(t, sizeof t, "%02X%02X%02X%02X", p[1], p[2], p[3], p[4]);
        d.id = t;
        d.type = p[0] == 0x58 ? "AFHDS 2A channels" : p[0] == 0x56 ? "AFHDS 2A failsafe" : p[0] == 0xBB || p[0] == 0xBC ? "AFHDS 2A bind" : "AFHDS 2A telemetry";
        return d;
    }
    return d;
}

std::vector<uint8_t> flyskyBuild(uint32_t txId, const uint16_t ch[8]) {
    std::vector<uint8_t> p(21, 0);
    p[0] = 0x55;
    p[1] = (uint8_t)txId; p[2] = (uint8_t)(txId >> 8); p[3] = (uint8_t)(txId >> 16); p[4] = (uint8_t)(txId >> 24);
    for (int i = 0; i < 8; i++) { p[5 + 2 * i] = (uint8_t)ch[i]; p[6 + 2 * i] = (uint8_t)(ch[i] >> 8); }
    return p;
}

std::vector<uint8_t> afhds2aBuild(uint32_t txId, uint32_t rxId, const uint16_t ch[14]) {
    std::vector<uint8_t> p(38, 0xFF);
    p[0] = 0x58;
    p[1] = (uint8_t)(txId >> 24); p[2] = (uint8_t)(txId >> 16); p[3] = (uint8_t)(txId >> 8); p[4] = (uint8_t)txId;
    p[5] = (uint8_t)(rxId >> 24); p[6] = (uint8_t)(rxId >> 16); p[7] = (uint8_t)(rxId >> 8); p[8] = (uint8_t)rxId;
    for (int i = 0; i < 14; i++) { p[9 + 2 * i] = (uint8_t)ch[i]; p[10 + 2 * i] = (uint8_t)((ch[i] >> 8) & 15); }
    return p;
}

} // namespace rclink
} // namespace dect2
