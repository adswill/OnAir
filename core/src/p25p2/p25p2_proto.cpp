// P25 Phase 2 protocol pieces (see p25p2_proto.h for what is established and what is this program's own layout).
#include "dect2/p25p2/p25p2_proto.h"
#include "dect2/p25/p25_proto.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace p25p2 {

namespace {
// I-ISCH (40,9) code words, by their 7 information bits (XMC_SDR P25_P2_Decode)
const uint64_t kIschWords[128] = {
    0x184229d461ULL, 0x18761451f6ULL, 0x181ae27e2fULL, 0x182edffbb8ULL, 0x18df8a7510ULL, 0x18ebb7f087ULL,
    0x188741df5eULL, 0x18b37c5ac9ULL, 0x1146a44f13ULL, 0x117299ca84ULL, 0x111e6fe55dULL, 0x112a5260caULL,
    0x11db07ee62ULL, 0x11ef3a6bf5ULL, 0x1183cc442cULL, 0x11b7f1c1bbULL, 0x1a4a2e239eULL, 0x1a7e13a609ULL,
    0x1a12e589d0ULL, 0x1a26d80c47ULL, 0x1ad78d82efULL, 0x1ae3b00778ULL, 0x1a8f4628a1ULL, 0x1abb7bad36ULL,
    0x134ea3b8ecULL, 0x137a9e3d7bULL, 0x13166812a2ULL, 0x1322559735ULL, 0x13d300199dULL, 0x13e73d9c0aULL,
    0x138bcbb3d3ULL, 0x13bff63644ULL, 0x1442f705efULL, 0x1476ca8078ULL, 0x141a3cafa1ULL, 0x142e012a36ULL,
    0x14df54a49eULL, 0x14eb692109ULL, 0x14879f0ed0ULL, 0x14b3a28b47ULL, 0x1d467a9e9dULL, 0x1d72471b0aULL,
    0x1d1eb134d3ULL, 0x1d2a8cb144ULL, 0x1ddbd93fecULL, 0x1defe4ba7bULL, 0x1d831295a2ULL, 0x1db72f1035ULL,
    0x164af0f210ULL, 0x167ecd7787ULL, 0x16123b585eULL, 0x162606ddc9ULL, 0x16d7535361ULL, 0x16e36ed6f6ULL,
    0x168f98f92fULL, 0x16bba57cb8ULL, 0x1f4e7d6962ULL, 0x1f7a40ecf5ULL, 0x1f16b6c32cULL, 0x1f228b46bbULL,
    0x1fd3dec813ULL, 0x1fe7e34d84ULL, 0x1f8b15625dULL, 0x1fbf28e7caULL, 0x084d62c339ULL, 0x08795f46aeULL,
    0x0815a96977ULL, 0x082194ece0ULL, 0x08d0c16248ULL, 0x08e4fce7dfULL, 0x08880ac806ULL, 0x08bc374d91ULL,
    0x0149ef584bULL, 0x017dd2dddcULL, 0x011124f205ULL, 0x0125197792ULL, 0x01d44cf93aULL, 0x01e0717cadULL,
    0x018c875374ULL, 0x01b8bad6e3ULL, 0x0a456534c6ULL, 0x0a7158b151ULL, 0x0a1dae9e88ULL, 0x0a29931b1fULL,
    0x0ad8c695b7ULL, 0x0aecfb1020ULL, 0x0a800d3ff9ULL, 0x0ab430ba6eULL, 0x0341e8afb4ULL, 0x0375d52a23ULL,
    0x03192305faULL, 0x032d1e806dULL, 0x03dc4b0ec5ULL, 0x03e8768b52ULL, 0x038480a48bULL, 0x03b0bd211cULL,
    0x044dbc12b7ULL, 0x0479819720ULL, 0x041577b8f9ULL, 0x04214a3d6eULL, 0x04d01fb3c6ULL, 0x04e4223651ULL,
    0x0488d41988ULL, 0x04bce99c1fULL, 0x0d493189c5ULL, 0x0d7d0c0c52ULL, 0x0d11fa238bULL, 0x0d25c7a61cULL,
    0x0dd49228b4ULL, 0x0de0afad23ULL, 0x0d8c5982faULL, 0x0db864076dULL, 0x0645bbe548ULL, 0x06718660dfULL,
    0x061d704f06ULL, 0x06294dca91ULL, 0x06d8184439ULL, 0x06ec25c1aeULL, 0x0680d3ee77ULL, 0x06b4ee6be0ULL,
    0x0f41367e3aULL, 0x0f750bfbadULL, 0x0f19fdd474ULL, 0x0f2dc051e3ULL, 0x0fdc95df4bULL, 0x0fe8a85adcULL,
    0x0f845e7505ULL, 0x0fb063f092ULL,
};
// DUID (8,4): the DUID of every 8-bit value within one bit error of a code word, -1 beyond (XMC_SDR P25_P2_Decode)
const int8_t kDuidLookup[256] = {
    0,0,0,-1,0,-1,-1,1,0,-1,-1,4,-1,8,2,-1,0,-1,-1,1,-1,1,1,1,-1,3,9,-1,5,-1,-1,1,
    0,-1,-1,10,-1,6,2,-1,-1,3,2,-1,2,-1,2,2,-1,3,7,-1,11,-1,-1,1,3,3,-1,3,-1,3,2,-1,
    0,-1,-1,4,-1,6,12,-1,-1,4,4,4,5,-1,-1,4,-1,13,7,-1,5,-1,-1,1,5,-1,-1,4,5,5,5,-1,
    -1,6,7,-1,6,6,-1,6,14,-1,-1,4,-1,6,2,-1,7,-1,7,7,-1,6,7,-1,-1,3,7,-1,5,-1,-1,15,
    0,-1,-1,10,-1,8,12,-1,-1,8,9,-1,8,8,-1,8,-1,13,9,-1,11,-1,-1,1,9,-1,9,9,-1,8,9,-1,
    -1,10,10,10,11,-1,-1,10,14,-1,-1,10,-1,8,2,-1,11,-1,-1,10,11,11,11,-1,-1,3,9,-1,11,-1,-1,15,
    -1,13,12,-1,12,-1,12,12,14,-1,-1,4,-1,8,12,-1,13,13,-1,13,-1,13,12,-1,-1,13,9,-1,5,-1,-1,15,
    14,-1,-1,10,-1,6,12,-1,14,14,14,-1,14,-1,-1,15,-1,13,7,-1,11,-1,-1,15,14,-1,-1,15,-1,15,15,15,
};
// the AMBE+2 bit order of a voice frame: code word row and column of each of the 72 bits as sent (TIA-102.BAAC, as in DSD-FME and XMC_SDR)
const int kC0[24] = {23, 5, 22, 4, 21, 3, 20, 2, 19, 1, 18, 0, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6};
const int kC1[23] = {10, 9, 8, 7, 6, 5, 22, 4, 21, 3, 20, 2, 19, 1, 18, 0, 17, 16, 15, 14, 13, 12, 11};
const int kC2[11] = {3, 2, 1, 0, 10, 9, 8, 7, 6, 5, 4};
const int kC3[14] = {13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
const int kRow[72] = {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 3, 0, 0, 1, 3, 0, 1, 1, 3, 0, 1, 1, 3, 0, 1, 1, 3, 0, 1, 1, 3,
                      0, 1, 1, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
const int kRowLen[4] = {24, 23, 11, 14};
struct AmbePos {   // row and column of each bit of the frame as sent
    int row[72], col[72];
    AmbePos() {
        int q[4] = {};
        const int* c[4] = {kC0, kC1, kC2, kC3};
        for (int x = 0; x < 72; x++) { row[x] = kRow[x]; col[x] = c[kRow[x]][q[kRow[x]]++]; }
    }
};
const AmbePos& ambePos() { static const AmbePos p; return p; }
bool isDuidPos(int i) { return i == kDuidPos[0] || i == kDuidPos[1] || i == kDuidPos[2] || i == kDuidPos[3]; }
inline int burstBit(const uint8_t* b, int pos) { const uint8_t d = b[kIschDibits + pos / 2]; return (pos & 1) ? (d & 1) : ((d >> 1) & 1); }
inline void setBurstBit(uint8_t* b, int pos, int v) {
    uint8_t& d = b[kIschDibits + pos / 2];
    const int m = (pos & 1) ? 1 : 2;
    d = (uint8_t)(v ? (d | m) : (d & ~m));
}
} // namespace

const char* duidName(int d) {
    switch (d) {
    case kDuid4V: return "4V voice";
    case kDuid2V: return "2V voice";
    case kDuidSacch: return "SACCH";
    case kDuidFacch: return "FACCH";
    case kDuidSacchClear: return "SACCH (not scrambled)";
    case kDuidFacchClear: return "FACCH (not scrambled)";
    case kDuidLcch: return "LCCH";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------- ISCH
void ischSync(uint8_t d[kIschDibits]) {
    for (int i = 0; i < kIschDibits; i++) d[i] = (uint8_t)((kSyncWord >> (38 - 2 * i)) & 3);
}

uint64_t ischValue(const uint8_t d[kIschDibits]) {
    uint64_t v = 0;
    for (int i = 0; i < kIschDibits; i++) v = (v << 2) | (d[i] & 3);
    return v;
}

int syncDistance(const uint8_t d[kIschDibits]) { return __builtin_popcountll(ischValue(d) ^ kSyncWord); }

uint64_t ischWord(int info) { return kIschWords[info & 127]; }

void ischInfo(int info, uint8_t d[kIschDibits]) {
    const uint64_t w = ischWord(info);
    for (int i = 0; i < kIschDibits; i++) d[i] = (uint8_t)((w >> (38 - 2 * i)) & 3);
}

int ischDecode(uint64_t v, int* errs) {
    int best = -1, bestDist = 41;
    for (int i = 0; i < 128; i++) {
        const int dist = __builtin_popcountll(v ^ kIschWords[i]);
        if (dist < bestDist) { bestDist = dist; best = i; }
    }
    if (errs) *errs = bestDist;
    return bestDist <= 7 ? best : -1;   // the code is (40, 9, 16): seven errors are corrected
}

// ---------------------------------------------------------------- scrambling (TIA-102.BBAC 7.2.5, as XMC_SDR P25_P2_Decode)
void scrambleSeq(int wacn, int sysId, int nac, uint8_t seq[kScrambleDibits]) {
    uint64_t s = ((uint64_t)(wacn & 0xFFFFF) << 24) | ((uint64_t)(sysId & 0xFFF) << 12) | (uint64_t)(nac & 0xFFF);
    for (int i = 0; i < 10; i++) seq[i] = 0;   // the ISCH half that leads the superframe
    for (int i = 10; i < kScrambleDibits; i++) {
        int d = 0;
        for (int k = 0; k < 2; k++) {
            d = (d << 1) | (int)((s >> 43) & 1);
            const uint64_t b = ((s >> 33) ^ (s >> 19) ^ (s >> 14) ^ (s >> 8) ^ (s >> 3) ^ (s >> 43)) & 1;
            s = ((s << 1) | b) & ((1ULL << 44) - 1);
        }
        seq[i] = (uint8_t)d;
    }
}

// ---------------------------------------------------------------- DUID
int duidWord(int duid) {
    for (int v = 0; v < 256; v++) {
        if ((v >> 4) != (duid & 15) || kDuidLookup[v] != (duid & 15)) continue;
        bool centre = true;
        for (int b = 0; b < 8; b++) centre = centre && kDuidLookup[v ^ (1 << b)] == (duid & 15);
        if (centre) return v;
    }
    return 0;
}

void duidPut(int duid, uint8_t burst[kBurstDibits]) {
    const int w = duidWord(duid);
    for (int i = 0; i < 4; i++) burst[kDuidPos[i]] = (uint8_t)((w >> (6 - 2 * i)) & 3);
}

int duidGet(const uint8_t burst[kBurstDibits], int* raw) {
    int v = 0;
    for (int i = 0; i < 4; i++) v = (v << 2) | (burst[kDuidPos[i]] & 3);
    if (raw) *raw = v;
    return kDuidLookup[v];
}

// ---------------------------------------------------------------- voice
void ambeGet(const uint8_t burst[kBurstDibits], int bitOff, char fr[4][24]) {
    memset(fr, 0, 4 * 24);
    const AmbePos& p = ambePos();
    for (int x = 0; x < kVoiceBits; x++) fr[p.row[x]][p.col[x]] = (char)burstBit(burst, bitOff + x);
}

void ambePut(const char fr[4][24], int bitOff, uint8_t burst[kBurstDibits]) {
    const AmbePos& p = ambePos();
    for (int x = 0; x < kVoiceBits; x++) setBurstBit(burst, bitOff + x, fr[p.row[x]][p.col[x]] & 1);
}

void ambeFlatten(const char fr[4][24], uint8_t bits[kVoiceBits]) {
    int k = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < kRowLen[r]; c++) bits[k++] = (uint8_t)(fr[r][c] & 1);
}

void ambeUnflatten(const uint8_t bits[kVoiceBits], char fr[4][24]) {
    memset(fr, 0, 4 * 24);
    int k = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < kRowLen[r]; c++) fr[r][c] = (char)(bits[k++] & 1);
}

// ---------------------------------------------------------------- ESS
void hexbitsPut(const uint8_t* hex, int n, uint8_t* d) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 3; j++) d[i * 3 + j] = (uint8_t)((hex[i] >> (4 - 2 * j)) & 3);
}

void hexbitsGet(const uint8_t* d, int n, uint8_t* hex) {
    for (int i = 0; i < n; i++) hex[i] = (uint8_t)(((d[i * 3] & 3) << 4) | ((d[i * 3 + 1] & 3) << 2) | (d[i * 3 + 2] & 3));
}

void essEncode(int algId, int keyId, const uint8_t mi[9], uint8_t essB[16], uint8_t essA[28]) {
    uint8_t bits[96];
    for (int i = 0; i < 8; i++) bits[i] = (uint8_t)((algId >> (7 - i)) & 1);
    for (int i = 0; i < 16; i++) bits[8 + i] = (uint8_t)((keyId >> (15 - i)) & 1);
    for (int i = 0; i < 72; i++) bits[24 + i] = (uint8_t)((mi[i >> 3] >> (7 - (i & 7))) & 1);
    uint8_t sym[44] = {};
    for (int i = 0; i < 16; i++) {
        int v = 0;
        for (int b = 0; b < 6; b++) v = (v << 1) | bits[i * 6 + b];
        sym[i] = (uint8_t)v;
    }
    p25::rsEncode(sym, 44, 16);
    std::copy_n(sym, 16, essB);
    std::copy_n(sym + 16, 28, essA);
}

bool essDecode(uint8_t essB[16], const uint8_t essA[28], int& algId, int& keyId, uint8_t mi[9], int* fixed) {
    uint8_t sym[44];
    std::copy_n(essB, 16, sym);
    std::copy_n(essA, 28, sym + 16);
    int errs = 0;
    if (!p25::rsDecode(sym, 44, 16, &errs)) return false;
    if (fixed) *fixed = errs;
    std::copy_n(sym, 16, essB);
    algId = (essB[0] << 2) | (essB[1] >> 4);
    if (algId == 0) algId = kAlgClear;
    keyId = ((essB[1] & 0x0F) << 12) | (essB[2] << 6) | essB[3];
    for (int g = 0; g < 3; g++) {   // the MI: hexbits 4..15 (four per group) -> bytes 0..8 (three per group)
        const uint8_t* h = essB + 4 + 4 * g;
        uint8_t* m = mi + 3 * g;
        m[0] = (uint8_t)((h[0] << 2) | (h[1] >> 4));
        m[1] = (uint8_t)(((h[1] & 0x0F) << 4) | (h[2] >> 2));
        m[2] = (uint8_t)(((h[2] & 0x03) << 6) | h[3]);
    }
    return true;
}

// ---------------------------------------------------------------- MAC PDU codewords in the burst (this program's layout)
void payloadPut(const uint8_t* d, int n, uint8_t burst[kBurstDibits]) {
    int k = 0;
    for (int i = kIschDibits; i < kBurstDibits && k < n; i++)
        if (!isDuidPos(i)) burst[i] = d[k++];
}

void payloadGet(const uint8_t burst[kBurstDibits], uint8_t d[kPayloadDibits]) {
    int k = 0;
    for (int i = kIschDibits; i < kBurstDibits; i++)
        if (!isDuidPos(i)) d[k++] = burst[i];
}

// ---------------------------------------------------------------- MAC PDU codewords
void macEncode(bool facch, const uint8_t* mac, uint8_t* dibits) {
    const int nBytes = facch ? kFacchMacBytes : kSacchMacBytes;
    const int n = facch ? 52 : 46, k = facch ? 30 : 26;
    uint8_t bits[180] = {};
    for (int i = 0; i < nBytes * 8; i++) bits[i] = (uint8_t)((mac[i >> 3] >> (7 - (i & 7))) & 1);
    const uint16_t crc = p25::crcCcitt(bits, nBytes * 8);
    for (int i = 0; i < 16; i++) bits[nBytes * 8 + i] = (uint8_t)((crc >> (15 - i)) & 1);
    uint8_t sym[63] = {};
    for (int i = 0; i < k; i++) {
        int v = 0;
        for (int b = 0; b < 6; b++) v = (v << 1) | bits[i * 6 + b];
        sym[i] = (uint8_t)v;
    }
    p25::rsEncode(sym, n, k);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 3; j++) dibits[i * 3 + j] = (uint8_t)((sym[i] >> (4 - 2 * j)) & 3);
}

bool macDecode(bool facch, const uint8_t* dibits, uint8_t* mac, int* corrected) {
    const int nBytes = facch ? kFacchMacBytes : kSacchMacBytes;
    const int n = facch ? 52 : 46, k = facch ? 30 : 26;
    uint8_t sym[63] = {};
    for (int i = 0; i < n; i++) sym[i] = (uint8_t)(((dibits[i * 3] & 3) << 4) | ((dibits[i * 3 + 1] & 3) << 2) | (dibits[i * 3 + 2] & 3));
    int errs = 0;
    if (!p25::rsDecode(sym, n, k, &errs)) return false;
    if (corrected) *corrected = errs;
    uint8_t bits[180];
    for (int i = 0; i < k; i++)
        for (int b = 0; b < 6; b++) bits[i * 6 + b] = (uint8_t)((sym[i] >> (5 - b)) & 1);
    uint16_t crc = 0;
    for (int i = 0; i < 16; i++) crc = (uint16_t)((crc << 1) | bits[nBytes * 8 + i]);
    if (crc != p25::crcCcitt(bits, nBytes * 8)) return false;
    for (int i = 0; i < nBytes; i++) {
        int v = 0;
        for (int b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
        mac[i] = (uint8_t)v;
    }
    return true;
}


// ---------------------------------------------------------------- MAC PDU codewords as on the air
namespace {
struct Gf64 {
    uint8_t ex[126], lg[64];
    Gf64() {
        int x = 1;
        for (int i = 0; i < 63; i++) { ex[i] = ex[i + 63] = (uint8_t)x; lg[x] = (uint8_t)i; x <<= 1; if (x & 64) x ^= 0x43; }
        lg[0] = 0;
    }
    uint8_t mul(int a, int b) const { return a && b ? ex[lg[a] + lg[b]] : 0; }
    uint8_t inv(int a) const { return ex[63 - lg[a]]; }
};
const Gf64& gf() { static const Gf64 g; return g; }
constexpr int kRoots = 28;
} // namespace

bool rs63Decode(uint8_t sym[63], const int* erasures, int ne, int* errs) {
    const Gf64& G = gf();
    uint8_t r[63];
    for (int i = 0; i < 63; i++) r[i] = sym[i] & 63;
    for (int e = 0; e < ne; e++) r[erasures[e]] = 0;
    uint8_t S[kRoots];   // S[j] = r(alpha^(j+1))
    bool any = false;
    for (int j = 0; j < kRoots; j++) {
        uint8_t acc = 0;
        const uint8_t a = G.ex[j + 1];
        for (int i = 0; i < 63; i++) acc = (uint8_t)(G.mul(acc, a) ^ r[i]);
        S[j] = acc;
        any = any || acc;
    }
    if (errs) *errs = 0;
    if (!any) { memcpy(sym, r, 63); return true; }
    if (ne > kRoots) return false;
    // error + erasure locator (Berlekamp-Massey started from the erasure locator)
    uint8_t lam[kRoots + 2] = {1}, b[kRoots + 2] = {1};
    for (int e = 0; e < ne; e++) {
        const uint8_t X = G.ex[62 - erasures[e]];
        for (int i = kRoots; i >= 1; i--) lam[i] ^= G.mul(lam[i - 1], X);
    }
    memcpy(b, lam, sizeof b);
    int el = ne;
    for (int rr = ne + 1; rr <= kRoots; rr++) {
        uint8_t d = 0;
        for (int i = 0; i < rr; i++) d ^= G.mul(lam[i], S[rr - i - 1]);
        if (!d) { memmove(b + 1, b, kRoots); b[0] = 0; continue; }
        uint8_t t[kRoots + 2];
        t[0] = lam[0];
        for (int i = 0; i < kRoots + 1; i++) t[i + 1] = (uint8_t)(lam[i + 1] ^ G.mul(d, b[i]));
        if (2 * el <= rr + ne - 1) {
            el = rr + ne - el;
            const uint8_t di = G.inv(d);
            for (int i = 0; i <= kRoots; i++) b[i] = G.mul(lam[i], di);
        } else { memmove(b + 1, b, kRoots); b[0] = 0; }
        memcpy(lam, t, sizeof lam);
    }
    int deg = 0;
    for (int i = 0; i <= kRoots; i++) if (lam[i]) deg = i;
    if (deg != el || deg > kRoots) return false;
    // evaluator omega = S * lambda mod x^kRoots
    uint8_t om[kRoots] = {};
    for (int i = 0; i < kRoots; i++)
        for (int j = 0; j <= i && j <= deg; j++) om[i] ^= G.mul(lam[j], S[i - j]);
    int found = 0, corrected = 0;
    for (int pos = 0; pos < 63; pos++) {
        const int xi = (63 - (62 - pos)) % 63;   // log of X^-1 for X = alpha^(62 - pos)
        uint8_t v = 0, xp = 1;
        for (int i = 0; i <= deg; i++) { v ^= G.mul(lam[i], xp); xp = G.mul(xp, G.ex[xi]); }
        if (v) continue;
        found++;
        uint8_t num = 0, den = 0, p1 = 1;
        for (int i = 0; i < kRoots; i++) { num ^= G.mul(om[i], p1); p1 = G.mul(p1, G.ex[xi]); }
        uint8_t p2 = 1;
        for (int i = 1; i <= deg; i += 2) { den ^= G.mul(lam[i], p2); p2 = G.mul(p2, G.mul(G.ex[xi], G.ex[xi])); }
        if (!den) return false;
        r[pos] ^= G.mul(num, G.inv(den));
        bool era = false;
        for (int e = 0; e < ne; e++) era = era || erasures[e] == pos;
        if (!era) corrected++;
    }
    if (found != deg) return false;
    for (int j = 0; j < kRoots; j++) {   // the result must be a codeword
        uint8_t acc = 0;
        const uint8_t a = G.ex[j + 1];
        for (int i = 0; i < 63; i++) acc = (uint8_t)(G.mul(acc, a) ^ r[i]);
        if (acc) return false;
    }
    memcpy(sym, r, 63);
    if (errs) *errs = corrected;
    return true;
}

uint16_t crc12(const uint8_t* bits, int n) {
    static const uint8_t poly[13] = {1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1};
    uint8_t buf[256] = {};
    if (n < 0 || n + 12 > (int)sizeof buf) return 0;
    for (int i = 0; i < n; i++) buf[i] = bits[i] & 1;
    for (int i = 0; i < n; i++)
        if (buf[i]) for (int j = 0; j < 13; j++) buf[i + j] ^= poly[j];
    uint16_t crc = 0;
    for (int i = 0; i < 12; i++) crc = (uint16_t)((crc << 1) | buf[n + i]);
    return (uint16_t)(crc ^ 0xFFF);
}

namespace {
// payload index of the k-th codeword dibit
int airIndex(bool facch, int k) { return facch && k >= 67 ? k + 21 : k; }
}

void macEncodeAir(bool facch, const uint8_t* mac, uint8_t payload[kPayloadDibits]) {
    const int nBytes = facch ? kFacchAirBytes : kSacchAirBytes;
    const int first = facch ? 9 : 5, nData = facch ? 26 : 30, nSent = facch ? 45 : 52;
    uint8_t bits[180] = {};
    for (int i = 0; i < nBytes * 8; i++) bits[i] = (uint8_t)((mac[i >> 3] >> (7 - (i & 7))) & 1);
    const uint16_t crc = crc12(bits, nBytes * 8);
    for (int i = 0; i < 12; i++) bits[nBytes * 8 + i] = (uint8_t)((crc >> (11 - i)) & 1);
    uint8_t sym[63] = {};
    for (int i = 0; i < nData; i++) {
        int v = 0;
        for (int b = 0; b < 6; b++) v = (v << 1) | bits[i * 6 + b];
        sym[first + i] = (uint8_t)v;
    }
    p25::rsEncode(sym, 63, 35);
    memset(payload, 0, kPayloadDibits);
    for (int i = 0; i < nSent; i++)
        for (int j = 0; j < 3; j++) payload[airIndex(facch, i * 3 + j)] = (uint8_t)((sym[first + i] >> (4 - 2 * j)) & 3);
}

bool macDecodeAir(bool facch, const uint8_t payload[kPayloadDibits], uint8_t* mac, int* corrected) {
    const int nBytes = facch ? kFacchAirBytes : kSacchAirBytes;
    const int first = facch ? 9 : 5, nData = facch ? 26 : 30, nSent = facch ? 45 : 52;
    uint8_t rx[63] = {};
    for (int i = 0; i < nSent; i++)
        rx[first + i] = (uint8_t)(((payload[airIndex(facch, i * 3)] & 3) << 4) | ((payload[airIndex(facch, i * 3 + 1)] & 3) << 2) | (payload[airIndex(facch, i * 3 + 2)] & 3));
    // erasures: the punctured parity at the end; first the shortened front symbols are taken as the zeros they are, then (if that fails) as erased too
    for (int attempt = 0; attempt < 2; attempt++) {
        int era[20], ne = 0;
        for (int p = first + nSent; p < 63; p++) era[ne++] = p;
        if (attempt) for (int p = 0; p < first; p++) era[ne++] = p;
        uint8_t sym[63];
        memcpy(sym, rx, 63);
        int errs = 0;
        if (!rs63Decode(sym, era, ne, &errs)) continue;
        uint8_t bits[180];
        for (int i = 0; i < nData; i++)
            for (int b = 0; b < 6; b++) bits[i * 6 + b] = (uint8_t)((sym[first + i] >> (5 - b)) & 1);
        uint16_t crc = 0;
        for (int i = 0; i < 12; i++) crc = (uint16_t)((crc << 1) | bits[nBytes * 8 + i]);
        if (crc != crc12(bits, nBytes * 8)) continue;
        for (int i = 0; i < nBytes; i++) {
            int v = 0;
            for (int b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
            mac[i] = (uint8_t)v;
        }
        if (corrected) *corrected = errs;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- MAC PDU
const char* macOpName(int op) {
    switch (op) {
    case kMacSignal: return "MAC_SIGNAL";
    case kMacPtt: return "MAC_PTT";
    case kMacEndPtt: return "MAC_END_PTT";
    case kMacIdle: return "MAC_IDLE";
    case kMacActive: return "MAC_ACTIVE";
    case kMacHangtime: return "MAC_HANGTIME";
    default: return "MAC reserved";
    }
}

const char* macMsgName(int op) {
    switch (op) {
    case kMsgNull: return "Null";
    case kMsgGrpVUser: return "GRP_V_CH_USR";
    case kMsgUuVUser: return "UU_V_CH_USR";
    case kMsgGrpVGrant: return "GRP_V_CH_GRANT";
    case kMsgGrpVGrantUpd: return "GRP_V_CH_GRANT_UPDT";
    case kMsgUuVGrant: return "UU_V_CH_GRANT";
    case kMsgIdenUpVu: return "IDEN_UP_VU";
    case kMsgRfssSts: return "RFSS_STS_BCST";
    case kMsgNetSts: return "NET_STS_BCST";
    case kMsgAdjSts: return "ADJ_STS_BCST";
    case kMsgIdenUp: return "IDEN_UP";
    default: return "unknown";
    }
}

// Lengths (bytes, opcode byte included) of the MAC messages by opcode byte: the two top bits say the family (0 TDMA's own, 1 the abbreviated form of a
// TSBK, 2 manufacturer, 3 the extended / explicit forms). Collected from OP25's message length table (the lengths are facts of the layout).
int macMsgLen(int op) {
    static const struct { uint8_t op, len; } kLen[] = {{0x01, 7}, {0x02, 8}, {0x03, 7}, {0x05, 16}, {0x21, 14}, {0x22, 15}, {0x25, 15}, {0x30, 5}, {0x31, 7}, {0x40, 9}, {0x41, 7}, {0x42, 9}, {0x44, 9}, {0x45, 8}, {0x46, 9}, {0x48, 10}, {0x49, 10}, {0x4A, 9}, {0x4C, 10}, {0x54, 9}, {0x55, 7}, {0x58, 10}, {0x5A, 7}, {0x5C, 10}, {0x5D, 8}, {0x5E, 14}, {0x5F, 7}, {0x60, 9}, {0x61, 9}, {0x64, 9}, {0x67, 9}, {0x68, 10}, {0x6A, 7}, {0x6B, 10}, {0x6C, 10}, {0x6D, 7}, {0x6F, 9}, {0x70, 9}, {0x71, 29}, {0x72, 9}, {0x73, 9}, {0x74, 9}, {0x75, 9}, {0x76, 10}, {0x77, 13}, {0x78, 9}, {0x79, 9}, {0x7A, 9}, {0x7B, 11}, {0x7C, 9}, {0x7D, 9}, {0x80, 8}, {0x83, 7}, {0x84, 11}, {0xA0, 16}, {0xA3, 11}, {0xA4, 13}, {0xA5, 11}, {0xA6, 11}, {0xA7, 11}, {0xA8, 10}, {0xC0, 11}, {0xC3, 8}, {0xC4, 15}, {0xC5, 12}, {0xC6, 15}, {0xC7, 32}, {0xC8, 12}, {0xC9, 12}, {0xCB, 27}, {0xCC, 14}, {0xCD, 29}, {0xCE, 29}, {0xCF, 32}, {0xD6, 9}, {0xD8, 14}, {0xD9, 29}, {0xDA, 11}, {0xDB, 27}, {0xDC, 14}, {0xDE, 40}, {0xDF, 11}, {0xE0, 28}, {0xE3, 14}, {0xE4, 17}, {0xE5, 14}, {0xE8, 16}, {0xE9, 8}, {0xEA, 11}, {0xEC, 13}, {0xED, 19}, {0xF2, 16}, {0xF3, 14}, {0xF6, 12}, {0xF8, 22}, {0xFA, 11}, {0xFB, 13}, {0xFC, 11}, {0xFE, 15}};
    for (const auto& e : kLen) if (e.op == (op & 0xFF)) return e.len;
    return 0;
}

int macMsgLenAt(const uint8_t* m, int avail) {
    if (avail <= 0) return 0;
    const int op = m[0];
    if (op == 0x00) return avail;
    if (avail < 2) return macMsgLen(op);
    if (op == 0x08) return m[1] & 0x3F;
    if (op == 0x11) return (((m[1] & 3) + 1) * 2) + 2;
    if (op == 0x12) return (((m[1] & 3) + 1) * 3) + 2;
    if ((op >> 6) == 2 && avail > 2 && (m[2] & 0x3F)) return m[2] & 0x3F;
    return macMsgLen(op);
}

namespace {
std::vector<uint8_t> pad(std::vector<uint8_t> v, int n) { v.resize((size_t)n, 0); return v; }
std::vector<uint8_t> argMsg(int op, uint64_t a) {
    std::vector<uint8_t> m(9);
    m[0] = (uint8_t)op;
    for (int i = 0; i < 8; i++) m[1 + i] = (uint8_t)(a >> (56 - 8 * i));
    return m;
}
} // namespace

std::vector<uint8_t> macPtt(int n, int src, int group, int algId, int keyId, const uint8_t* mi) {
    std::vector<uint8_t> v((size_t)kPttLen, 0);
    v[0] = macHeaderByte(kMacPtt, 0);
    v[kPttAlgOff] = (uint8_t)algId;
    put16(&v[kPttKeyOff], (uint32_t)keyId);
    if (mi) for (int i = 0; i < 9; i++) v[(size_t)(kPttMiOff + i)] = mi[i];
    put24(&v[kPttSrcOff], (uint32_t)src);
    put16(&v[kPttGrpOff], (uint32_t)group);
    return pad(v, n);
}

std::vector<uint8_t> macEndPtt(int n, int src, int group, int colorCode) {
    std::vector<uint8_t> v((size_t)kEndLen, 0);
    v[0] = macHeaderByte(kMacEndPtt, 0);
    put16(&v[kEndColorOff], (uint32_t)(colorCode & 0xFFF));
    put24(&v[kEndSrcOff], (uint32_t)src);
    put16(&v[kEndGrpOff], (uint32_t)group);
    return pad(v, n);
}

std::vector<uint8_t> macActive(int n, int svcOpts, int group, int src) {
    std::vector<uint8_t> v(8, 0);
    v[0] = macHeaderByte(kMacActive, 0);
    v[1] = kMsgGrpVUser; v[2] = (uint8_t)svcOpts;
    put16(&v[3], (uint32_t)group); put24(&v[5], (uint32_t)src);
    return pad(v, n);
}

std::vector<uint8_t> macActiveUu(int n, int svcOpts, int target, int src) {
    std::vector<uint8_t> v(9, 0);
    v[0] = macHeaderByte(kMacActive, 0);
    v[1] = kMsgUuVUser; v[2] = (uint8_t)svcOpts;
    put24(&v[3], (uint32_t)target); put24(&v[6], (uint32_t)src);
    return pad(v, n);
}

std::vector<uint8_t> macSignal(int n, const std::vector<std::vector<uint8_t>>& msgs) {
    std::vector<uint8_t> v{macHeaderByte(kMacSignal, 0)};
    for (const auto& m : msgs) v.insert(v.end(), m.begin(), m.end());
    return pad(v, n);
}

std::vector<uint8_t> macIdle(int n) { return pad({macHeaderByte(kMacIdle, 0)}, n); }

std::vector<uint8_t> msgGrpVGrant(int svc, int channel, int group, int src) {
    return argMsg(kMsgGrpVGrant, ((uint64_t)(svc & 0xFF) << 56) | ((uint64_t)(channel & 0xFFFF) << 40) | ((uint64_t)(group & 0xFFFF) << 24) | ((uint64_t)src & 0xFFFFFF));
}
std::vector<uint8_t> msgGrpVGrantUpd(int ch1, int g1, int ch2, int g2) {
    return argMsg(kMsgGrpVGrantUpd, ((uint64_t)(ch1 & 0xFFFF) << 48) | ((uint64_t)(g1 & 0xFFFF) << 32) | ((uint64_t)(ch2 & 0xFFFF) << 16) | (uint64_t)(g2 & 0xFFFF));
}
std::vector<uint8_t> msgUuVGrant(int channel, int target, int src) {
    return argMsg(kMsgUuVGrant, ((uint64_t)(channel & 0xFFFF) << 48) | ((uint64_t)(target & 0xFFFFFF) << 24) | ((uint64_t)src & 0xFFFFFF));
}
std::vector<uint8_t> msgRfssSts(int lra, int sysId, int rfss, int site, int channel, int ssc) {
    return argMsg(kMsgRfssSts, ((uint64_t)(lra & 0xFF) << 56) | ((uint64_t)(sysId & 0xFFF) << 40) | ((uint64_t)(rfss & 0xFF) << 32) | ((uint64_t)(site & 0xFF) << 24) |
                                   ((uint64_t)(channel & 0xFFFF) << 8) | (uint64_t)(ssc & 0xFF));
}
std::vector<uint8_t> msgNetSts(int lra, int wacn, int sysId, int channel, int ssc) {
    return argMsg(kMsgNetSts, ((uint64_t)(lra & 0xFF) << 56) | ((uint64_t)(wacn & 0xFFFFF) << 36) | ((uint64_t)(sysId & 0xFFF) << 24) | ((uint64_t)(channel & 0xFFFF) << 8) | (uint64_t)(ssc & 0xFF));
}
std::vector<uint8_t> msgAdjSts(int lra, int sysId, int rfss, int site, int channel, int ssc) {
    return argMsg(kMsgAdjSts, ((uint64_t)(lra & 0xFF) << 56) | ((uint64_t)(sysId & 0xFFF) << 40) | ((uint64_t)(rfss & 0xFF) << 32) | ((uint64_t)(site & 0xFF) << 24) |
                                   ((uint64_t)(channel & 0xFFFF) << 8) | (uint64_t)(ssc & 0xFF));
}
std::vector<uint8_t> msgIdenUp(int id, int bw125, int txOffset250k, int spacing125, uint32_t base5Hz) {
    return argMsg(kMsgIdenUp, ((uint64_t)(id & 15) << 60) | ((uint64_t)(bw125 & 0x1FF) << 51) | ((uint64_t)(txOffset250k & 0x1FF) << 42) | ((uint64_t)(spacing125 & 0x3FF) << 32) | base5Hz);
}

} // namespace p25p2
} // namespace dect2
