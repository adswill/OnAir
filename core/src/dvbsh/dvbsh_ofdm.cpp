// DVB-SH OFDM tables (ETSI EN 302 583 V1.2.1 clause 5.7): the 1K mode next to the DVB-T 2K / 4K / 8K ones, the symbol interleaver with the
// 1K permutation (table 5.16), and the TPS with the DVB-SH fields (5.7.4.3.2). The 2K / 4K / 8K pilot and TPS positions come from the DVB-T
// code (the same tables, EN 300 744 and its annex F).
#include "dect2/dvbsh/dvbsh.h"
#include "dect2/dvbt/dvbt.h"
#include <cstring>
#include <mutex>

namespace dect2 {
namespace dvbsh {

const char* modeName(int mode) { static const char* n[] = {"2K", "8K", "4K", "1K"}; return mode >= 0 && mode < 4 ? n[mode] : "?"; }

const std::vector<int>& continualPilots(int mode) {
    // Table 5.27, 1K mode
    static const std::vector<int> cp1k = {0, 48, 54, 87, 141, 156, 192, 201, 255, 279, 282, 333, 432, 450, 483, 525, 531, 618, 636, 714, 759, 765, 786, 813, 852};
    return mode == k1K ? cp1k : dvbt::continualPilots(mode);
}
const std::vector<int>& tpsCarriers(int mode) {
    static const std::vector<int> tps1k = {34, 209, 346, 413, 569, 688, 790};   // table 5.28, 1K mode
    return mode == k1K ? tps1k : dvbt::tpsCarriers(mode);
}

void carrierRoles(int mode, int symIdx, std::vector<uint8_t>& roles) {
    const int K = carriersK(mode);
    roles.assign(K, 0);
    for (int k = 3 * (symIdx % 4); k < K; k += 12) roles[k] = 1;
    for (int k : continualPilots(mode)) roles[k] = 2;
    for (int k : tpsCarriers(mode)) roles[k] = 3;
}

const std::vector<int>& symbolPermutation(int mode) {
    static std::vector<int> h[4];
    static std::once_flag once[4];
    const int m = mode & 3;
    std::call_once(once[m], [m] {
        // R' bit k goes to R bit perm[k] (tables 5.16 - 5.19)
        static const int p1k[9] = {4, 3, 0, 6, 2, 8, 1, 5, 7};
        static const int p2k[10] = {4, 3, 9, 6, 2, 8, 1, 5, 7, 0};
        static const int p4k[11] = {6, 3, 0, 9, 4, 2, 1, 8, 5, 10, 7};
        static const int p8k[12] = {7, 1, 4, 2, 9, 6, 8, 10, 0, 3, 11, 5};
        const int Mmax = fftN(m), Nmax = dataCarriers(m);
        int Nr = 0;
        while ((1 << Nr) < Mmax) Nr++;
        const int* bp = m == k1K ? p1k : m == k2K ? p2k : m == k4K ? p4k : p8k;
        std::vector<int>& H = h[m];
        H.assign(Mmax, 0);
        unsigned reg = 1;
        const unsigned mask = (1u << (Nr - 1)) - 1;
        int q = 0;
        for (int i = 0; i < Mmax; i++) {
            unsigned r = 0;
            if (i >= 2) {
                if (i > 2) {
                    unsigned nb;
                    if (m == k1K) nb = (reg ^ (reg >> 5)) & 1;
                    else if (m == k2K) nb = (reg ^ (reg >> 3)) & 1;
                    else if (m == k4K) nb = (reg ^ (reg >> 2)) & 1;
                    else nb = (reg ^ (reg >> 1) ^ (reg >> 4) ^ (reg >> 6)) & 1;
                    reg = ((reg >> 1) | (nb << (Nr - 2))) & mask;
                }
                r = reg;
            }
            unsigned nr = 0;
            for (int k = 0; k < Nr - 1; k++) nr |= ((r >> k) & 1u) << bp[k];
            H[q] = (int)(((unsigned)(i % 2) << (Nr - 1)) + nr);
            if (H[q] < Nmax) q++;
        }
        H.resize(Nmax);
    });
    return h[m];
}

double sampleRate(double bwMhz) {
    if (bwMhz < 3) return 12.8e6 / 7.0;   // 1.7 MHz channel: T = 7/12.8 us
    return 8e6 / 7.0 * bwMhz;             // 8 MHz: 64/7 MHz ... 5 MHz: 40/7 MHz
}

double netBitrate(const Params& p, double fs, bool lp) {
    const int pat = lp ? p.patLp : p.patHp;
    const double symSec = (double)(fftN(p.mode) + guardSamples(p.mode, p.guard)) / fs;
    const double shSec = symSec * 68.0 * kShFrameCu * kCuBits / (streamBitsPerSymbol(p) * 68.0);
    return codewordsPerShFrame(pat) * 8.0 * 188 * 8 / shSec;
}

// ============================================================================ TPS
namespace {
void put(std::array<uint8_t, 68>& s, int first, int last, unsigned v) { for (int i = last; i >= first; i--) { s[i] = v & 1; v >>= 1; } }
unsigned get(const std::array<uint8_t, 68>& s, int a, int b) { unsigned v = 0; for (int i = a; i <= b; i++) v = (v << 1) | s[i]; return v; }
void bch(std::array<uint8_t, 68>& s) {   // BCH (67,53) shortened from (127,113): as the DVB-T TPS
    unsigned reg = 0;
    uint8_t in[113];
    memset(in, 0, 60);
    memcpy(in + 60, &s[1], 53);
    for (int i = 0; i < 113; i++) {
        const unsigned fb = (in[i] ^ reg) & 1;
        reg >>= 1;
        reg |= fb << 13;
        reg ^= (fb << 12) ^ (fb << 11) ^ (fb << 9) ^ (fb << 8) ^ (fb << 7) ^ (fb << 5) ^ (fb << 4);
    }
    for (int i = 0; i < 14; i++) s[54 + i] = (reg >> i) & 1;
}
// The 32 interleaver bits B1..B32 (figure 5.36): B1 the MSB
uint32_t tiWord(const TiConfig& c) {
    return ((uint32_t)(c.commonMultiplier & 63) << 26) | ((uint32_t)(c.lateTaps & 63) << 20) | ((uint32_t)(c.slices & 63) << 14) | ((uint32_t)(c.sliceDistance & 255) << 6) | (uint32_t)(c.nonLateIncrement & 63);
}
inline unsigned bitsB(uint32_t w, int first, int n) { return (w >> (32 - (first - 1) - n)) & ((1u << n) - 1); }   // B(first) .. B(first + n - 1)
}

std::array<uint8_t, 68> tpsBits(const Params& p, int fi, int sfInSh) {
    std::array<uint8_t, 68> s{};
    fi &= 3;
    const uint32_t w = tiWord(p.ti);
    s[0] = dvbt::prbsW()[0];
    put(s, 1, 16, (fi & 1) ? 0xCA11 : 0x35EE);
    put(s, 17, 22, 37);
    put(s, 23, 24, (unsigned)fi);
    put(s, 25, 26, (unsigned)p.mod);
    put(s, 27, 29, (unsigned)p.hier);
    // s30..s33: the HP rate (frame 1), the LP rate (frame 3; 0 without hierarchy), interleaver bits (frames 2 and 4); s34 and s48..s52 interleaver bits
    static const int b30[4] = {0, 7, 0, 23}, b34[4] = {1, 11, 17, 27}, b48[4] = {2, 12, 18, 28};
    if (fi == 0) put(s, 30, 33, (unsigned)p.patHp);
    else if (fi == 2) put(s, 30, 33, p.hier ? (unsigned)p.patLp : 0u);
    else put(s, 30, 33, bitsB(w, b30[fi], 4));
    put(s, 34, 34, bitsB(w, b34[fi], 1));
    put(s, 35, 35, (fi & 1) ? (unsigned)(sfInSh & 1) : (unsigned)((sfInSh >> 1) & 1));
    put(s, 36, 37, (unsigned)p.guard);
    put(s, 38, 39, (unsigned)p.mode);
    put(s, 40, 47, (fi & 1) ? (unsigned)(p.cellId & 0xFF) : (unsigned)((p.cellId >> 8) & 0xFF));
    put(s, 48, 52, bitsB(w, b48[fi], 5));
    put(s, 53, 53, 1);   // DVB-SH mode
    bch(s);
    return s;
}

bool tpsDecode(const uint8_t bits[68], TpsFrame& f, int maxFix) {
    std::array<uint8_t, 68> s;
    for (int i = 0; i < 68; i++) s[i] = bits[i] & 1;
    auto valid = [&]() {
        const unsigned sync = get(s, 1, 16);
        if (sync != 0x35EE && sync != 0xCA11) return false;
        std::array<uint8_t, 68> c = s;
        bch(c);
        for (int i = 54; i < 68; i++) if (c[i] != s[i]) return false;
        return true;
    };
    bool ok = valid();
    for (int i = 1; !ok && maxFix >= 1 && i < 68; i++) {
        s[i] ^= 1;
        ok = valid();
        for (int j = i + 1; !ok && maxFix >= 2 && j < 68; j++) { s[j] ^= 1; ok = valid(); if (!ok) s[j] ^= 1; }
        if (!ok) s[i] ^= 1;
    }
    if (!ok) return false;
    f.frameIdx = (int)get(s, 23, 24);
    if (((f.frameIdx & 1) != 0) != (get(s, 1, 16) == 0xCA11)) return false;
    f.length = (int)get(s, 17, 22);
    f.mod = (int)get(s, 25, 26);
    f.hier = (int)get(s, 27, 29);
    f.s30_33 = (int)get(s, 30, 33);
    f.s34 = (int)get(s, 34, 34);
    f.s35 = (int)get(s, 35, 35);
    f.guard = (int)get(s, 36, 37);
    f.mode = (int)get(s, 38, 39);
    f.cellByte = (int)get(s, 40, 47);
    f.s48_52 = (int)get(s, 48, 52);
    f.sh = get(s, 53, 53) != 0;
    return true;
}

bool tpsCombine(const TpsFrame f[4], Params& p, int& sfInSh) {
    for (int i = 0; i < 4; i++) {
        if (f[i].frameIdx != i || !f[i].sh) return false;
        if (f[i].mode != f[0].mode || f[i].guard != f[0].guard || f[i].mod != f[0].mod || f[i].hier != f[0].hier) return false;
    }
    if (f[0].mod > 1 || f[0].hier > 3 || (f[0].hier && f[0].mod != k16Qam)) return false;
    p.mode = f[0].mode; p.guard = f[0].guard; p.mod = f[0].mod; p.hier = f[0].hier;
    p.patHp = f[0].s30_33;
    p.patLp = p.hier ? f[2].s30_33 : p.patHp;
    if (p.patHp >= kNumPatterns || p.patLp >= kNumPatterns) return false;
    uint32_t w = 0;
    auto add = [&](unsigned v, int n) { w = (w << n) | v; };
    add((unsigned)f[0].s34, 1); add((unsigned)f[0].s48_52, 5);                                // B1..B6
    add((unsigned)f[1].s30_33, 4); add((unsigned)f[1].s34, 1); add((unsigned)f[1].s48_52, 5);   // B7..B16
    add((unsigned)f[2].s34, 1); add((unsigned)f[2].s48_52, 5);                                // B17..B22
    add((unsigned)f[3].s30_33, 4); add((unsigned)f[3].s34, 1); add((unsigned)f[3].s48_52, 5);   // B23..B32
    p.ti.commonMultiplier = (int)bitsB(w, 1, 6);
    p.ti.lateTaps = (int)bitsB(w, 7, 6);
    p.ti.slices = (int)bitsB(w, 13, 6);
    p.ti.sliceDistance = (int)bitsB(w, 19, 8);
    p.ti.nonLateIncrement = (int)bitsB(w, 27, 6);
    p.cellId = (f[0].cellByte << 8) | f[1].cellByte;
    sfInSh = f[0].s35 * 2 + f[1].s35;
    std::vector<int> L;
    return tiDelays(p.ti, kShFrameCu, L);
}

} // namespace dvbsh
} // namespace dect2
