// Bluetooth Classic discovery: see btc_proto.h. The sync word generator matrix and the packet unpacking follow libbtbb (GPL-2.0-or-later).
#include "dect2/btc/btc_proto.h"
#include <cstdio>

namespace dect2 {
namespace btc {

namespace {

// libbtbb: the (64,30) block code of the sync word, polynomial 0260534236651, modified for the PN overlay and the Barker code
constexpr uint64_t kDefaultCodeword = 0xb0000002c7820e7eULL;
constexpr uint64_t kMatrix[24] = {
    0xfe000002a0d1c014ULL, 0x01000003f0b9201fULL, 0x008000033ae40edbULL, 0x004000035fca99b9ULL, 0x002000036d5dd208ULL, 0x00100001b6aee904ULL,
    0x00080000db577482ULL, 0x000400006dabba41ULL, 0x00020002f46d43f4ULL, 0x000100017a36a1faULL, 0x00008000bd1b50fdULL, 0x000040029c3536aaULL,
    0x000020014e1a9b55ULL, 0x0000100265b5d37eULL, 0x0000080132dae9bfULL, 0x000004025bd5ea0bULL, 0x00000203ef526bd1ULL, 0x000001033511ab3cULL,
    0x000000819a88d59eULL, 0x00000040cd446acfULL, 0x00000022a41aabb3ULL, 0x0000001390b5cb0dULL, 0x0000000b0ae27b52ULL, 0x0000000585713da9ULL};
constexpr uint16_t kFec23[10] = {0x2c01, 0x5802, 0x1c04, 0x3808, 0x7010, 0x4c20, 0x3440, 0x6880, 0x7d00, 0x5600};

int pop64(uint64_t v) { return __builtin_popcountll(v); }

uint64_t host64(const uint8_t* b, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v |= (uint64_t)(b[i] & 1) << i; return v; }

uint8_t rev8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    return (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
}

// Whitening: x^7 + x^4 + 1, started with CLK1-6 and a one; the same call whitens and dewhitens. skip: bits already used by the header.
void whiten(uint8_t* b, size_t n, int clk, size_t skip) {
    int r[7];
    for (int i = 0; i < 6; i++) r[i] = (clk >> i) & 1;
    r[6] = 1;
    for (size_t i = 0; i < skip + n; i++) {
        const int o = r[6];
        for (int k = 6; k > 0; k--) r[k] = r[k - 1];
        r[0] = o;
        r[4] ^= o;
        if (i >= skip) b[i - skip] ^= (uint8_t)o;
    }
}

// the unknown-UAP recovery from the header: the UAP the HEC was computed with
uint8_t uapFromHec(uint16_t data, uint8_t hec) {
    for (int i = 9; i >= 0; i--) {
        if (hec & 0x80) hec ^= 0x65;
        hec = (uint8_t)((hec << 1) | (((hec >> 7) ^ (data >> i)) & 1));
    }
    return rev8(hec);
}

uint16_t crc16(const uint8_t* p, int n, int uap) {
    uint16_t reg = (uint16_t)((rev8((uint8_t)uap) << 8) & 0xff00);
    for (int i = 0; i < n; i++) {
        reg = (uint16_t)((reg >> 1) | ((((reg & 1) ^ (p[i] & 1))) << 15));
        reg ^= (uint16_t)((reg & 0x8000) >> 5);
        reg ^= (uint16_t)((reg & 0x8000) >> 12);
    }
    return reg;
}

uint16_t fec23(uint16_t data) {
    uint16_t c = 0;
    for (int i = 0; i < 10; i++) if (data & (1 << i)) c ^= kFec23[i];
    return c;
}

bool unfec13(const uint8_t* in, uint8_t* out, int n) {
    int be = 0;
    for (int i = 0; i < n; i++) {
        const int a = in[3 * i] & 1, b = in[3 * i + 1] & 1, c = in[3 * i + 2] & 1;
        out[i] = (uint8_t)((a & b) | (b & c) | (c & a));
        be += (a ^ b) | (b ^ c) | (c ^ a);
    }
    return be < n / 4;
}

// (15,10) shortened Hamming: single errors are corrected, anything else fails
bool unfec23(const uint8_t* in, uint8_t* out, int nbits) {
    for (int ip = 0, op = 0; op < nbits; ip += 15, op += 10) {
        for (int k = 0; k < 10; k++) out[op + k] = in[ip + k];
        const uint16_t data = (uint16_t)host64(in + ip, 10);
        const uint8_t check = (uint8_t)host64(in + ip + 10, 5);
        const uint8_t diff = (uint8_t)(check ^ (fec23(data) >> 10));
        if (diff & (diff - 1)) {
            static const uint8_t syn[10] = {0x0b, 0x16, 0x07, 0x0e, 0x1c, 0x13, 0x0d, 0x1a, 0x1f, 0x15};
            int k = 0;
            while (k < 10 && syn[k] != diff) k++;
            if (k == 10) return false;
            out[op + k] ^= 1;
        }
    }
    return true;
}

} // namespace

uint64_t syncWord(uint32_t lap) {
    uint64_t c = kDefaultCodeword;
    for (int i = 0; i < 24; i++) if (lap & (0x800000u >> i)) c ^= kMatrix[i];
    return c;
}

bool findAccessCode(const uint8_t* bits, size_t n, size_t start, int maxErr, AcHit& hit) {
    if (n < start + 68) return false;
    for (size_t p = start; p + 68 <= n; p++) {
        // the preamble alternates (its last bit also differs from the first sync bit, which is left to the sync word check: that bit may be in error)
        const uint8_t* s = bits + p + 4;
        if ((bits[p] ^ bits[p + 1]) != 1 || (bits[p + 1] ^ bits[p + 2]) != 1 || (bits[p + 2] ^ bits[p + 3]) != 1) continue;
        const uint64_t w = host64(s, 64);
        const int top = (int)(w >> 57);
        if (pop64((uint64_t)(top ^ 0x58)) > 1 && pop64((uint64_t)(top ^ 0x27)) > 1) continue;   // Barker + LAP MSB: 1011000 (MSB 0) or 0100111 (MSB 1)
        const uint32_t lap = (uint32_t)((w >> 34) & 0xFFFFFF);
        const int e = pop64(w ^ syncWord(lap));
        if (e > maxErr) continue;
        hit.pos = p; hit.lap = lap; hit.errors = e;
        hit.trailer = p + 72 <= n && (bits[p + 68] ^ s[63]) == 1 && (bits[p + 68] ^ bits[p + 69]) == 1 && (bits[p + 69] ^ bits[p + 70]) == 1 && (bits[p + 70] ^ bits[p + 71]) == 1;
        return true;
    }
    return false;
}

bool decodeFhs(const uint8_t* bits, size_t n, Fhs& out) {
    if (n < 54 + 240) return false;
    uint8_t hdr[18];
    if (!unfec13(bits, hdr, 18)) return false;
    uint8_t pay[160];
    if (!unfec23(bits + 54, pay, 160)) return false;
    for (int clk = 0; clk < 64; clk++) {
        uint8_t h[18];
        for (int i = 0; i < 18; i++) h[i] = hdr[i];
        whiten(h, 18, clk, 0);
        const uint16_t hd = (uint16_t)host64(h, 10);
        const uint8_t hec = (uint8_t)host64(h + 10, 8);
        if (uapFromHec(hd, hec) != 0) continue;                // an inquiry response is checked with the default UAP 0
        if (host64(h + 3, 4) != 2) continue;                   // packet type FHS
        uint8_t p[160];
        for (int i = 0; i < 160; i++) p[i] = pay[i];
        whiten(p, 160, clk, 18);
        if (crc16(p, 144, 0) != (uint16_t)host64(p + 144, 16)) continue;
        out.lap = (uint32_t)host64(p + 34, 24);
        out.scanRepMode = (int)host64(p + 60, 2);
        out.uap = (uint8_t)host64(p + 64, 8);
        out.nap = (uint16_t)host64(p + 72, 16);
        out.cod = (uint32_t)host64(p + 88, 24);
        out.ltAddr = (uint8_t)host64(p + 112, 3);
        out.clk = (uint32_t)host64(p + 115, 26);
        out.pageScanMode = (int)host64(p + 141, 3);
        out.hdrClk = clk;
        return true;
    }
    return false;
}

std::string bdAddr(const Fhs& f) {
    char b[24];
    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", f.nap >> 8, f.nap & 0xFF, f.uap, (f.lap >> 16) & 0xFF, (f.lap >> 8) & 0xFF, f.lap & 0xFF);
    return b;
}
std::string lapText(uint32_t lap) { char b[12]; snprintf(b, sizeof b, "%06X", lap & 0xFFFFFF); return b; }
const char* acKind(uint32_t lap) { return lap == kGiac ? "GIAC (inquiry)" : lap == kLiac ? "LIAC (limited inquiry)" : ""; }

const char* codMajor(uint32_t cod) {
    switch ((cod >> 8) & 0x1F) {
    case 0: return "Miscellaneous"; case 1: return "Computer"; case 2: return "Phone"; case 3: return "LAN / network access point";
    case 4: return "Audio / video"; case 5: return "Peripheral"; case 6: return "Imaging"; case 7: return "Wearable"; case 8: return "Toy";
    case 9: return "Health"; case 31: return "Uncategorised";
    default: return "Reserved";
    }
}

std::string codText(uint32_t cod) {
    const unsigned maj = (cod >> 8) & 0x1F, mn = (cod >> 2) & 0x3F;
    std::string s = codMajor(cod);
    const char* m = "";
    switch (maj) {
    case 1: { static const char* t[] = {"uncategorised", "desktop", "server", "laptop", "handheld PC", "palm-size PC", "wearable computer", "tablet"}; if (mn < 8) m = t[mn]; break; }
    case 2: { static const char* t[] = {"uncategorised", "cellular", "cordless", "smartphone", "wired modem or voice gateway", "common ISDN access"}; if (mn < 6) m = t[mn]; break; }
    case 4: {
        static const char* t[] = {"uncategorised", "wearable headset", "hands-free", "reserved", "microphone", "loudspeaker", "headphones", "portable audio", "car audio", "set-top box",
                                  "HiFi audio", "VCR", "video camera", "camcorder", "video monitor", "video display and loudspeaker", "video conferencing", "reserved", "gaming or toy"};
        if (mn < 19) m = t[mn];
        break;
    }
    case 5: { const unsigned k = (mn >> 4) & 3; static const char* t[] = {"", "keyboard", "pointing device", "keyboard and pointing device"}; m = t[k]; break; }
    case 7: { static const char* t[] = {"", "wristwatch", "pager", "jacket", "helmet", "glasses"}; if (mn < 6) m = t[mn]; break; }
    default: break;
    }
    if (*m) s += std::string(": ") + m;
    static const char* svc[] = {"Limited discoverable", nullptr, nullptr, "Positioning", "Networking", "Rendering", "Capturing", "Object transfer", "Audio", "Telephony", "Information"};
    std::string sv;
    for (int b = 13; b <= 23; b++)
        if ((cod >> b) & 1) {
            const char* n = svc[b - 13];
            if (!n) continue;
            if (!sv.empty()) sv += ", ";
            sv += n;
        }
    if (!sv.empty()) s += " (" + sv + ")";
    return s;
}

// ---------------------------------------------------------------- transmit side

std::vector<uint8_t> makeAccessCode(uint32_t lap, bool trailer) {
    const uint64_t w = syncWord(lap);
    std::vector<uint8_t> v;
    const int first = (int)(w & 1), last = (int)((w >> 63) & 1);
    v = {(uint8_t)first, (uint8_t)!first, (uint8_t)first, (uint8_t)!first};   // alternating, the last bit differs from the first sync bit
    for (int i = 0; i < 64; i++) v.push_back((uint8_t)((w >> i) & 1));
    if (trailer) { int b = !last; for (int i = 0; i < 4; i++) { v.push_back((uint8_t)b); b = !b; } }
    return v;
}

std::vector<uint8_t> makeFhsPacket(const Fhs& f, int hdrClk) {
    std::vector<uint8_t> v = makeAccessCode(kGiac, true);
    // payload: 144 information bits then the CRC, UAP 0
    uint8_t p[160] = {};
    auto put = [&](int pos, uint64_t val, int n) { for (int i = 0; i < n; i++) p[pos + i] = (uint8_t)((val >> i) & 1); };
    // the parity bits (0-33) of the FHS carry the sync word of the sender's LAP without the LAP itself
    const uint64_t sw = syncWord(f.lap);
    put(0, sw & 0x3FFFFFFFFULL, 34);
    put(34, f.lap, 24);
    put(60, (uint64_t)f.scanRepMode, 2);
    put(64, f.uap, 8);
    put(72, f.nap, 16);
    put(88, f.cod, 24);
    put(112, f.ltAddr, 3);
    put(115, f.clk, 26);
    put(141, (uint64_t)f.pageScanMode, 3);
    put(144, crc16(p, 144, 0), 16);
    whiten(p, 160, hdrClk, 18);
    // header: LT_ADDR 0, type FHS (2), flow/arqn/seqn 0, HEC with the default UAP 0 (found by search: the inverse of uapFromHec)
    uint8_t h[18] = {};
    for (int i = 0; i < 4; i++) h[3 + i] = (uint8_t)((2 >> i) & 1);
    const uint16_t data = (uint16_t)host64(h, 10);
    uint8_t hec = 0;
    for (int c = 0; c < 256; c++) if (uapFromHec(data, (uint8_t)c) == 0) { hec = (uint8_t)c; break; }
    for (int i = 0; i < 8; i++) h[10 + i] = (uint8_t)((hec >> i) & 1);
    whiten(h, 18, hdrClk, 0);
    for (int i = 0; i < 18; i++) for (int k = 0; k < 3; k++) v.push_back(h[i]);
    // FEC 2/3 of the payload
    for (int ip = 0; ip < 160; ip += 10) {
        const uint16_t d = (uint16_t)host64(p + ip, 10);
        const uint16_t cw = fec23(d);
        for (int k = 0; k < 10; k++) v.push_back((uint8_t)((d >> k) & 1));
        for (int k = 0; k < 5; k++) v.push_back((uint8_t)((cw >> (10 + k)) & 1));
    }
    return v;
}

} // namespace btc
} // namespace dect2
