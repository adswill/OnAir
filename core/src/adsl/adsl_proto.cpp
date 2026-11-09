// ADS-L air format: see adsl_proto.h. The CRC routine and the bit layouts are those of the EASA specification.
#include "dect2/adsl/adsl_proto.h"
#include "dect2/flarm/flarm_proto.h"     // the XXTEA of the FLARM mode: the same cipher, here with the published all-zero key and 6 rounds
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace adsl {

namespace {

uint32_t getBits(const uint8_t* d, int off, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) if (d[(off + i) >> 3] >> ((off + i) & 7) & 1) v |= 1u << i;
    return v;
}
void setBits(uint8_t* d, int off, int n, uint32_t v) {
    for (int i = 0; i < n; i++) {
        const int b = off + i;
        d[b >> 3] = (uint8_t)((d[b >> 3] & ~(1 << (b & 7))) | (((v >> i) & 1) << (b & 7)));
    }
}

constexpr uint32_t kZeroKey[4] = {0, 0, 0, 0};

void toWords(const uint8_t* b, int n, uint32_t* w) { for (int i = 0; i < n; i++) w[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24; }
void toBytes(const uint32_t* w, int n, uint8_t* b) { for (int i = 0; i < n; i++) { b[4 * i] = (uint8_t)w[i]; b[4 * i + 1] = (uint8_t)(w[i] >> 8); b[4 * i + 2] = (uint8_t)(w[i] >> 16); b[4 * i + 3] = (uint8_t)(w[i] >> 24); } }

void scramble(uint8_t* b, size_t n, bool encrypt) {
    const int words = (int)(n / 4);
    if (words < 2) return;
    std::vector<uint32_t> w((size_t)words);
    toWords(b, words, w.data());
    flarm::btea(w.data(), encrypt ? words : -words, kZeroKey, 6);
    toBytes(w.data(), words, b);
}

// ---- GF(256), 0x11D
struct Gf {
    uint8_t exp[512]; int log[256];
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = -1;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[log[a] + 255 - log[b]] : 0; }
    uint8_t root(int i) const { return exp[(11 * (121 + i)) % 255]; }
};
const Gf& gf() { static const Gf g; return g; }

const uint8_t* rsGenerator() {      // highest degree first, the leading 1 implied: g[0] is the x^31 coefficient
    static uint8_t g[kRsParity];
    static const bool done = [] {
        const Gf& f = gf();
        uint8_t p[kRsParity + 1] = {1};
        for (int i = 0; i < kRsParity; i++) {
            const uint8_t r = f.root(i);
            for (int j = i + 1; j > 0; j--) p[j] = (uint8_t)(p[j - 1] ^ f.mul(p[j], r));
            p[0] = f.mul(p[0], r);
        }
        for (int i = 0; i < kRsParity; i++) g[i] = p[kRsParity - 1 - i];
        return true;
    }();
    (void)done;
    return g;
}

} // namespace

uint32_t crc24(const uint8_t* d, size_t n) {
    uint32_t crc = 0;
    auto pass = [&](uint8_t input) { crc |= input; for (int b = 0; b < 8; b++) { if (crc & 0x80000000u) crc ^= 0xFFFA0480u; crc <<= 1; } };
    for (size_t i = 0; i < n; i++) pass(d[i]);
    pass(0); pass(0); pass(0);
    return crc >> 8;
}

uint8_t crc8(const uint8_t* d, size_t n) {
    uint8_t crc = 0x71;
    for (size_t i = 0; i < n; i++) { crc ^= d[i]; for (int b = 0; b < 8; b++) crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1); }
    return crc;
}

void rsEncode(const uint8_t* data, size_t n, uint8_t parity[kRsParity]) {
    const Gf& f = gf();
    const uint8_t* g = rsGenerator();
    std::memset(parity, 0, kRsParity);
    for (size_t i = 0; i < n; i++) {
        const uint8_t fb = data[i] ^ parity[0];
        std::memmove(parity, parity + 1, kRsParity - 1);
        parity[kRsParity - 1] = 0;
        if (fb) for (int j = 0; j < kRsParity; j++) parity[j] ^= f.mul(fb, g[j]);
    }
}

int rsDecode(uint8_t* block, size_t n) {
    if (n < kRsParity + 1 || n > 255) return -1;
    const Gf& f = gf();
    uint8_t s[kRsParity];
    bool bad = false;
    for (int i = 0; i < kRsParity; i++) {
        const uint8_t r = f.root(i);
        uint8_t v = 0;
        for (size_t j = 0; j < n; j++) v = (uint8_t)(f.mul(v, r) ^ block[j]);
        s[i] = v;
        if (v) bad = true;
    }
    if (!bad) return 0;
    // Berlekamp-Massey: the locator polynomial, lowest degree first
    uint8_t lam[kRsParity + 2] = {1}, b[kRsParity + 2] = {1};
    int L = 0, m = 1;
    uint8_t bb = 1;
    for (int k = 0; k < kRsParity; k++) {
        uint8_t d = s[k];
        for (int i = 1; i <= L; i++) d ^= f.mul(lam[i], s[k - i]);
        if (!d) { m++; continue; }
        uint8_t t[kRsParity + 2];
        std::memcpy(t, lam, sizeof t);
        const uint8_t c = f.div(d, bb);
        for (int i = 0; i + m < kRsParity + 2; i++) lam[i + m] ^= f.mul(c, b[i]);
        if (2 * L <= k) { L = k + 1 - L; std::memcpy(b, t, sizeof b); bb = d; m = 1; }
        else m++;
    }
    if (L > kRsParity / 2) return -1;
    // The error evaluator Omega = S * Lambda mod x^32, and Chien search over the positions of the block. With roots alpha^(11 (121 + i)) a position j
    // (counted from the end) has the locator X = alpha^(11 j).
    uint8_t om[kRsParity] = {};
    for (int i = 0; i < kRsParity; i++) for (int j = 0; j <= std::min(i, L); j++) om[i] ^= f.mul(lam[j], s[i - j]);
    int found = 0;
    for (size_t p = 0; p < n; p++) {
        const int j = (int)(n - 1 - p);
        const int xLog = (11 * j) % 255;
        const uint8_t xInv = f.exp[(255 - xLog) % 255];
        uint8_t v = 0, pw = 1, dv = 0;
        for (int i = 0; i <= L; i++) {
            v ^= f.mul(lam[i], pw);
            if (i & 1) dv ^= f.mul(lam[i], pw);        // the formal derivative times x, at x = X^-1
            pw = f.mul(pw, xInv);
        }
        if (v) continue;
        uint8_t ov = 0;
        pw = 1;
        for (int i = 0; i < kRsParity; i++) { ov ^= f.mul(om[i], pw); pw = f.mul(pw, xInv); }
        if (!dv) return -1;
        // Forney with first root 121: e = X^(1 - 121) Omega(X^-1) / Lambda'(X^-1), and dv is X^-1 Lambda'(X^-1), so e = X^(-121) Omega / dv (X = alpha^(11 j))
        const int fac = ((255 - xLog) * 121) % 255;
        block[p] ^= f.mul(f.exp[fac], f.div(ov, dv));
        found++;
    }
    if (found != L) return -1;
    for (int i = 0; i < kRsParity; i++) {                // the syndromes must vanish now: anything else is a miscorrection
        const uint8_t r = f.root(i);
        uint8_t v = 0;
        for (size_t j = 0; j < n; j++) v = (uint8_t)(f.mul(v, r) ^ block[j]);
        if (v) return -1;
    }
    return found;
}

uint32_t expDecode(uint32_t raw, int N) {
    const uint32_t e = raw >> N, base = raw & ((1u << N) - 1);
    return (((1u << N) + base) << e) - (1u << N);
}

static uint32_t expEncodeFull(uint32_t v, int N) {
    int e = 3;
    while (e > 0 && v < (1u << (N + e)) - (1u << N)) e--;
    uint32_t base = (v + (1u << N) - (1u << (N + e))) >> e;
    if (base >= (1u << N)) base = (1u << N) - 1;
    return ((uint32_t)e << N) | base;
}

uint32_t expEncode(uint32_t v, int N) {
    const uint32_t r = expEncodeFull(v, N);
    return r == (4u << N) - 1 ? r - 1 : r;               // all ones means invalid
}

void packTraffic(const Traffic& t, uint8_t p[15]) {
    std::memset(p, 0, 15);
    setBits(p, 0, 6, (uint32_t)t.timeQ & 63);
    setBits(p, 6, 2, (uint32_t)t.flightState & 3);
    setBits(p, 8, 5, (uint32_t)t.category & 31);
    setBits(p, 13, 3, (uint32_t)t.emergency & 7);
    uint32_t la = 0x800000, lo = 0x800000;
    if (t.hasPos) {
        la = (uint32_t)std::clamp((int)std::lround(t.lat * 93206.0), -0x7FFFFF, 0x7FFFFF) & 0xFFFFFF;
        lo = (uint32_t)std::clamp((int)std::lround(t.lon * 46603.0), -0x7FFFFF, 0x7FFFFF) & 0xFFFFFF;
    }
    setBits(p, 16, 24, la);
    setBits(p, 40, 24, lo);
    setBits(p, 64, 8, t.hasSpeed ? expEncode((uint32_t)std::lround(std::max(0.0, t.speedMs) * 4), 6) : 0xFF);
    setBits(p, 72, 14, t.hasAlt ? expEncode((uint32_t)std::max(0, t.altM + 320), 12) : 0x3FFF);
    uint32_t cl = 0x1FF;
    if (t.hasClimb) {
        const int q = (int)std::lround(t.climbMs * 8);
        cl = expEncodeFull((uint32_t)std::abs(q), 6);                // +119 m/s is 0x0FF; the same magnitude downwards would be the "invalid" 0x1FF
        if (q < 0) { if (cl == 0xFF) cl = 0xFE; cl |= 0x100; }
    }
    setBits(p, 86, 9, cl);
    setBits(p, 95, 9, t.hasTrack ? (uint32_t)std::lround(std::fmod(t.trackDeg + 360.0, 360.0) * 512.0 / 360.0) & 511 : 0);
    setBits(p, 104, 2, (uint32_t)t.sil); setBits(p, 106, 2, (uint32_t)t.design); setBits(p, 108, 4, (uint32_t)t.nic);
    setBits(p, 112, 3, (uint32_t)t.hAcc); setBits(p, 115, 2, (uint32_t)t.vAcc); setBits(p, 117, 2, (uint32_t)t.velAcc);
}

void unpackTraffic(const uint8_t p[15], int amt, uint32_t addr, bool relay, Traffic& o) {
    o = Traffic();
    o.amt = amt; o.addr = addr; o.relay = relay;
    o.timeQ = (int)getBits(p, 0, 6); o.flightState = (int)getBits(p, 6, 2); o.category = (int)getBits(p, 8, 5); o.emergency = (int)getBits(p, 13, 3);
    const uint32_t la = getBits(p, 16, 24), lo = getBits(p, 40, 24);
    if (la != 0x800000 && lo != 0x800000) {
        o.lat = (double)(int32_t)(la << 8) / 256.0 / 93206.0;
        o.lon = (double)(int32_t)(lo << 8) / 256.0 / 46603.0;
        o.hasPos = std::fabs(o.lat) <= 90.0 && std::fabs(o.lon) <= 180.0;
    }
    const uint32_t sp = getBits(p, 64, 8);
    if (sp != 0xFF) { o.hasSpeed = true; o.speedMs = expDecode(sp, 6) * 0.25; }
    const uint32_t al = getBits(p, 72, 14);
    if (al != 0x3FFF) { o.hasAlt = true; o.altM = (int)expDecode(al, 12) - 320; }
    const uint32_t cl = getBits(p, 86, 9);
    if (cl != 0x1FF && cl != 0x100) {                    // 0x1FF invalid (Issue 2); 0x100 ("minus zero") is what OGN and SoftRF trackers send for "no climb rate"
        o.hasClimb = true;
        o.climbMs = expDecode(cl & 0xFF, 6) * 0.125 * (cl & 0x100 ? -1.0 : 1.0);
    }
    o.hasTrack = o.hasSpeed && sp != 0;
    if (o.hasTrack) o.trackDeg = getBits(p, 95, 9) * 360.0 / 512.0;
    o.sil = (int)getBits(p, 104, 2); o.design = (int)getBits(p, 106, 2); o.nic = (int)getBits(p, 108, 4);
    o.hAcc = (int)getBits(p, 112, 3); o.vAcc = (int)getBits(p, 115, 2); o.velAcc = (int)getBits(p, 117, 2);
}

void packStatus(const Status& s, uint8_t p[7]) {
    std::memset(p, 0, 7);
    setBits(p, 0, 4, (uint32_t)s.maxVersion); setBits(p, 4, 2, (uint32_t)s.mRx); setBits(p, 6, 2, (uint32_t)s.ldrRx); setBits(p, 8, 2, (uint32_t)s.hdrRx);
    setBits(p, 10, 2, (uint32_t)s.mobile); setBits(p, 12, 8, (uint32_t)s.econRx); setBits(p, 20, 8, (uint32_t)s.econTx); setBits(p, 28, 2, (uint32_t)s.xpdr);
    setBits(p, 30, 1, s.trafficClient); setBits(p, 31, 1, s.fisClient);
}

void unpackStatus(const uint8_t p[7], int amt, uint32_t addr, Status& o) {
    o = Status();
    o.amt = amt; o.addr = addr;
    o.maxVersion = (int)getBits(p, 0, 4); o.mRx = (int)getBits(p, 4, 2); o.ldrRx = (int)getBits(p, 6, 2); o.hdrRx = (int)getBits(p, 8, 2);
    o.mobile = (int)getBits(p, 10, 2); o.econRx = (int)getBits(p, 12, 8); o.econTx = (int)getBits(p, 20, 8); o.xpdr = (int)getBits(p, 28, 2);
    o.trafficClient = getBits(p, 30, 1) != 0; o.fisClient = getBits(p, 31, 1) != 0;
}

std::vector<uint8_t> makeData(int type, int amt, uint32_t addr, bool relay, const uint8_t* payload, size_t n) {
    std::vector<uint8_t> d(5 + n, 0);
    d[0] = (uint8_t)type;
    setBits(d.data(), 8, 6, (uint32_t)amt & 63);
    setBits(d.data(), 14, 24, addr & 0xFFFFFF);
    setBits(d.data(), 39, 1, relay);
    if (n) std::memcpy(d.data() + 5, payload, n);
    return d;
}

std::vector<uint8_t> makeNetwork(const std::vector<uint8_t>& data, int version, int keyIndex, bool fec) {
    std::vector<uint8_t> net(1 + data.size());
    net[0] = (uint8_t)((version & 15) | (keyIndex & 3) << 5 | (fec ? 0x80 : 0));
    std::memcpy(net.data() + 1, data.data(), data.size());
    if (keyIndex == 0) scramble(net.data() + 1, data.size(), true);
    if (fec) {
        uint8_t par[kRsParity];
        rsEncode(net.data(), net.size(), par);
        net.insert(net.end(), par, par + kRsParity);
    } else {
        const uint32_t c = crc24(net.data(), net.size());
        net.push_back((uint8_t)(c >> 16)); net.push_back((uint8_t)(c >> 8)); net.push_back((uint8_t)c);
    }
    return net;
}

int checkParity(uint8_t* net, size_t n) {
    if (n < 4) return -1;
    if (n >= 3 && crc24(net, n - 3) == (uint32_t)(net[n - 3] << 16 | net[n - 2] << 8 | net[n - 1])) return 0;
    if (n > kRsParity && n <= 255) return rsDecode(net, n);
    return -1;
}

int correctCrc(uint8_t* net, const int* weak, int nWeak) {
    const int bytes = kTrafficNetBytes - 3;
    static const std::vector<uint32_t> syn = [] {
        std::vector<uint32_t> s((size_t)kTrafficNetBytes * 8);
        for (int b = 0; b < kTrafficNetBytes * 8; b++) {
            uint8_t v[kTrafficNetBytes] = {};
            v[b >> 3] = (uint8_t)(0x80 >> (b & 7));
            s[(size_t)b] = crc24(v, bytes) ^ (uint32_t)(v[bytes] << 16 | v[bytes + 1] << 8 | v[bytes + 2]);
        }
        return s;
    }();
    const uint32_t s0 = crc24(net, bytes) ^ (uint32_t)(net[bytes] << 16 | net[bytes + 1] << 8 | net[bytes + 2]);
    if (!s0) return 0;
    nWeak = std::min(nWeak, 4);
    for (int m = 0; m < (1 << nWeak); m++) {
        uint32_t s = s0;
        int flips = 0;
        for (int i = 0; i < nWeak; i++) if (m >> i & 1) { s ^= syn[(size_t)weak[i]]; flips++; }
        int extra = -1;
        if (s) { for (int b = 0; b < kTrafficNetBytes * 8; b++) if (syn[(size_t)b] == s) { extra = b; break; } if (extra < 0) continue; }
        for (int i = 0; i < nWeak; i++) if (m >> i & 1) net[weak[i] >> 3] ^= (uint8_t)(0x80 >> (weak[i] & 7));
        if (extra >= 0) { net[extra >> 3] ^= (uint8_t)(0x80 >> (extra & 7)); flips++; }
        return flips;
    }
    return -1;
}

Parsed parseNetwork(const uint8_t* net, size_t n) {
    Parsed o;
    if (n < 1) return o;
    o.version = net[0] & 15;
    o.signature = (net[0] >> 4 & 1) != 0;
    o.keyIndex = net[0] >> 5 & 3;
    o.fec = (net[0] & 0x80) != 0;
    const size_t parity = o.fec ? (size_t)kRsParity : 3;
    if (n < 1 + parity) return o;
    const size_t dataBytes = n - 1 - parity;
    if (dataBytes < 5 || dataBytes % 4) { o.kind = Parsed::Reserved; return o; }   // header + payload is always a whole number of words
    if (o.keyIndex == 1 || o.keyIndex == 2) { o.kind = Parsed::Encrypted; return o; }   // keys reserved by the specification, none is published
    std::vector<uint8_t> d(net + 1, net + 1 + dataBytes);
    if (o.keyIndex == 0) scramble(d.data(), d.size(), false);
    o.type = d[0];
    o.amt = (int)getBits(d.data(), 8, 6);
    o.addr = getBits(d.data(), 14, 24);
    o.relay = getBits(d.data(), 39, 1) != 0;
    const uint8_t* pay = d.data() + 5;
    const size_t payBytes = dataBytes - 5;
    if (o.type == TypeTraffic && payBytes >= 15) {
        Traffic t; unpackTraffic(pay, o.amt, o.addr, o.relay, t);
        o.targets.push_back(t);
        o.kind = Parsed::TrafficMsg;
    } else if (o.type == TypeStatus && payBytes >= 7) {
        unpackStatus(pay, o.amt, o.addr, o.status);
        o.kind = Parsed::StatusMsg;
    } else if (o.type == TypeUplink && payBytes >= 19) {
        for (int i = 0; i < kMaxTargets && (size_t)(19 * i + 19) <= payBytes; i++) {
            const uint8_t* e = pay + 19 * i;
            int amt = (int)getBits(e, 0, 6);
            const uint32_t addr = getBits(e, 6, 24);
            Traffic t; unpackTraffic(e + 4, amt, addr, true, t);
            if (addr || t.hasPos) o.targets.push_back(t);
        }
        o.kind = Parsed::UplinkMsg;
    } else o.kind = Parsed::Other;
    return o;
}

static void pushBytes(std::vector<uint8_t>& c, const uint8_t* b, size_t n, bool manchester) {
    for (size_t i = 0; i < n; i++) for (int k = 7; k >= 0; k--) {
        const uint8_t bit = (b[i] >> k) & 1;
        if (manchester) { c.push_back(bit ? 0 : 1); c.push_back(bit ? 1 : 0); }
        else c.push_back(bit);
    }
}

std::vector<uint8_t> frameChips(Band band, const std::vector<uint8_t>& net, int rep) {
    std::vector<uint8_t> c;
    if (band == BandM0 || band == BandM1) {
        if (rep < 0) rep = 8;
        for (int i = 0; i < rep; i++) { c.push_back(0); c.push_back(1); }
        for (int i = 0; i < 2; i++) { c.push_back(1); c.push_back(0); c.push_back(0); c.push_back(1); }
        const uint8_t sync[2] = {0x72, 0x4B}, len = (uint8_t)net.size();
        pushBytes(c, sync, 2, true); pushBytes(c, &len, 1, true); pushBytes(c, net.data(), net.size(), true);
    } else if (band == BandLdr) {
        if (rep < 0) rep = 40;
        for (int i = 0; i < rep; i++) { c.push_back(1); c.push_back(0); }
        const uint8_t sync[2] = {0xB4, 0x2B};
        pushBytes(c, sync, 2, false); pushBytes(c, kLdrHeader, kLdrHeaderBytes, false); pushBytes(c, net.data(), net.size(), false);
        const uint8_t cs = crc8(net.data(), net.size());
        pushBytes(c, &cs, 1, false);
    } else {
        if (rep < 0) rep = 12;
        for (int i = 0; i < rep; i++) { c.push_back(1); c.push_back(0); }
        const uint8_t sync[2] = {0x2D, 0xD4}, len = (uint8_t)net.size();
        pushBytes(c, sync, 2, false); pushBytes(c, &len, 1, false); pushBytes(c, net.data(), net.size(), false);
    }
    return c;
}

const char* categoryName(int c) {
    static const char* n[18] = {"unknown", "light aircraft", "fixed wing", "light rotorcraft", "glider", "lighter than air", "ultralight", "paraglider", "skydiver", "eVTOL / UAM",
                                "gyrocopter", "UAS (open)", "UAS (specific)", "UAS (certified)", "model plane", "heavy rotorcraft", "hang-glider", "paramotor"};
    return c >= 0 && c < 18 ? n[c] : "reserved";
}
const char* emergencyName(int e) {
    static const char* n[8] = {"undefined", "none", "general emergency", "lifeguard / medical", "no communications", "unlawful interference", "downed aircraft", "reserved"};
    return e >= 0 && e < 8 ? n[e] : "reserved";
}
const char* amtName(int a) {
    switch (a) {
    case AmtRandom: return "random";
    case AmtIcao: return "ICAO";
    case AmtFlarm: return "FLARM";
    case AmtOgn: return "OGN";
    case AmtFanet: return "FANET";
    default: return a >= 9 ? "manufacturer" : "reserved";
    }
}

} // namespace adsl
} // namespace dect2
