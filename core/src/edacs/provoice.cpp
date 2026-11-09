// ProVoice: see provoice.h.
#include "dect2/edacs/provoice.h"
#include <cmath>
#include <cstring>

namespace dect2::provoice {

#include "provoice_tables.inc"

namespace {

// ---- error control (mbelib ecc.c)
int golayMatrix[2048];
bool golayReady = false;
int golayParity(int data12) {   // data bits 11..0 = code bits 22..11
    int p = 0;
    for (int i = 0; i < 12; i++) if (data12 & (0x800 >> i)) p ^= kGolayGen[i];
    return p;
}
void golayInit() {
    if (golayReady) return;
    std::memset(golayMatrix, 0, sizeof golayMatrix);
    // syndrome of each pattern of up to three errors in the 12 data bits and 11 parity bits; a syndrome is written once, with the pattern of least weight first
    bool seen[2048] = {};
    for (int w = 0; w <= 3; w++) {
        for (int a = 0; a < 23; a++) for (int b = w >= 2 ? a + 1 : 23; b < (w >= 2 ? 23 : 24); b++) for (int c = w >= 3 ? b + 1 : 23; c < (w >= 3 ? 23 : 24); c++) {
            if (w == 0 && (a || b != 23 || c != 23)) continue;
            if (w == 1 && (b != 23 || c != 23)) continue;
            if (w == 2 && c != 23) continue;
            uint32_t e = 0;
            if (w >= 1) e |= 1u << a;
            if (w >= 2) e |= 1u << b;
            if (w >= 3) e |= 1u << c;
            int d = (int)(e >> 11) & 0xFFF, p = (int)e & 0x7FF;
            const int syn = golayParity(d) ^ p;
            if (!seen[syn]) { seen[syn] = true; golayMatrix[syn] = d; }
        }
    }
    golayReady = true;
}
// in[0..22] (bit 22 first data bit, as mbelib holds it) -> out (corrected data in 22..11)
int golay2312(const char* in, char* out) {
    golayInit();
    long block = 0;
    for (int i = 22; i >= 0; i--) block = (block << 1) + in[i];
    int exp = 0;
    long mask = 0x400000L;
    for (int i = 0; i < 12; i++) { if (block & mask) exp ^= kGolayGen[i]; mask >>= 1; }
    const int syn = exp ^ (int)(block & 0x7FFL);
    int data = (int)(block >> 11) ^ golayMatrix[syn];
    long b = (long)data;
    for (int i = 22; i >= 11; i--) { out[i] = (char)((b & 2048) >> 11); b <<= 1; }
    for (int i = 10; i >= 0; i--) out[i] = in[i];
    int errs = 0;
    for (int i = 22; i >= 11; i--) errs += out[i] != in[i];
    return errs;
}
int hammingSyndrome(int block, const int* gen) {
    int syndrome = 0;
    for (int i = 0; i < 4; i++) {
        syndrome <<= 1;
        int stmp = block & gen[i];
        int stmp2 = stmp & 1;
        for (int j = 0; j < 14; j++) { stmp >>= 1; stmp2 ^= stmp & 1; }
        syndrome |= stmp2;
    }
    return syndrome;
}
const int kHammingFlip[16] = {0x0, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000};
int hamming(const char* in, char* out, const int* gen) {
    int block = 0;
    for (int i = 14; i >= 0; i--) block = (block << 1) | in[i];
    const int s = hammingSyndrome(block, gen);
    int errs = 0;
    if (s > 0) { errs = 1; block ^= kHammingFlip[s]; }
    for (int i = 0; i < 15; i++) { out[i] = (char)(block & 1); block >>= 1; }
    return errs;
}
int hammingEncode(int data11, const int* gen) {   // data in bits 14..4, returns the 15 bits with the four parity bits set
    const int base = data11 << 4;
    for (int p = 0; p < 16; p++) if (hammingSyndrome(base | p, gen) == 0) return base | p;
    return base;
}
void pn(int seed, int count, char* pr) {   // the 0 / 1 sequence pr[1..count-1] of mbelib's modulator
    uint32_t v = (uint32_t)(16 * seed);
    uint16_t x[120];
    x[0] = (uint16_t)v;
    for (int i = 1; i < count; i++) {
        const uint32_t t = 173u * x[i - 1] + 13849u;
        x[i] = (uint16_t)(t - 65536u * (t / 65536u));
    }
    for (int i = 1; i < count; i++) pr[i] = (char)(x[i] / 32768);
}

void modulate7100(Frame7100& f, bool /*forward*/) {
    int seed = 0;
    for (int i = 18; i > 11; i--) seed = (seed << 1) | f.b[0][i];
    char pr[120];
    pn(seed, 101, pr);
    int k = 1;
    for (int j = 23; j >= 0; j--) f.b[1][j] ^= pr[k++];
    for (int i = 2; i < 4; i++) for (int j = 22; j >= 0; j--) f.b[i][j] ^= pr[k++];
    for (int i = 4; i < 6; i++) for (int j = 14; j >= 0; j--) f.b[i][j] ^= pr[k++];
}

void modulate7200(Frame7200& f) {
    int seed = 0;
    for (int i = 22; i >= 11; i--) seed = (seed << 1) | f.b[0][i];
    char pr[120];
    pn(seed, 115, pr);
    int k = 1;
    for (int i = 1; i < 4; i++) for (int j = 22; j >= 0; j--) f.b[i][j] ^= pr[k++];
    for (int i = 4; i < 7; i++) for (int j = 14; j >= 0; j--) f.b[i][j] ^= pr[k++];
}
}

// ---- the 7100 x 4400 frame. Layout of mbelib: C0 bits 18..12 are information bits (the top of the Golay word starts at bit 18), bit 0 is status; C1 24 bits with the Golay (bits 23..1); C2, C3 Golay; C4, C5 Hamming; C6 is plain.
void encode7100(const uint8_t info[kInfoBits], Frame7100& f) {
    std::memset(&f, 0, sizeof f);
    int k = 0;
    // C0: seven information bits at 18..12
    for (int j = 18; j > 11; j--) f.b[0][j] = (char)info[k++];
    // C1: 12 information bits; Golay, placed at bits 23..1
    {
        char gin[23] = {}, cw[23] = {};
        int d = 0;
        for (int j = 22; j > 10; j--) { gin[j] = (char)info[k]; d = (d << 1) | info[k]; k++; }
        const int p = golayParity(d);
        for (int j = 22; j > 10; j--) cw[j] = gin[j];
        for (int j = 10; j >= 0; j--) cw[j] = (char)((p >> j) & 1);
        for (int j = 0; j < 23; j++) f.b[1][j + 1] = cw[j];
    }
    for (int i = 2; i < 4; i++) {
        int d = 0;
        for (int j = 22; j > 10; j--) { f.b[i][j] = (char)info[k]; d = (d << 1) | info[k]; k++; }
        const int p = golayParity(d);
        for (int j = 10; j >= 0; j--) f.b[i][j] = (char)((p >> j) & 1);
    }
    for (int i = 4; i < 6; i++) {
        int d = 0;
        for (int j = 14; j >= 4; j--) { d = (d << 1) | info[k]; k++; }
        const int w = hammingEncode(d, kHamming7100);
        for (int j = 0; j < 15; j++) f.b[i][j] = (char)((w >> j) & 1);
    }
    for (int j = 22; j >= 0; j--) f.b[6][j] = (char)info[k++];
    // C0 Golay: the 7 bits in its 18..12 place form (with the rest zero) the code; bits 18..12 of C0 are the data of a shortened Golay word, parity at 11..1
    {
        char in[23] = {}, cw[23] = {};
        int d = 0;
        for (int j = 17; j > 10; j--) { in[j] = f.b[0][j + 1]; d = (d << 1) | in[j]; }
        // the 7 data bits are the top of a 12-bit field (the unused data bits are zero); the parity bits go into C0 bits 12..1
        int d12 = 0;
        for (int j = 22; j > 10; j--) d12 = (d12 << 1) | (j <= 17 ? in[j] : 0);
        const int p = golayParity(d12);
        for (int j = 10; j >= 0; j--) cw[j] = (char)((p >> j) & 1);
        for (int j = 22; j > 10; j--) cw[j] = j <= 17 ? in[j] : 0;
        for (int j = 0; j < 18; j++) f.b[0][j + 1] = cw[j];
    }
    modulate7100(f, true);
}

bool decode7100(Frame7100 f, uint8_t info[kInfoBits], int* errsOut) {
    int errs = 0;
    {   // C0
        char in[23] = {}, out[23] = {};
        for (int j = 0; j < 18; j++) in[j] = f.b[0][j + 1];
        errs += golay2312(in, out);
        for (int j = 0; j < 18; j++) f.b[0][j + 1] = out[j];
    }
    modulate7100(f, false);
    int k = 0;
    for (int j = 18; j > 11; j--) info[k++] = (uint8_t)f.b[0][j];
    {   // C1
        char gin[23], gout[23];
        for (int j = 0; j < 23; j++) gin[j] = f.b[1][j + 1];
        errs += golay2312(gin, gout);
        for (int j = 22; j > 10; j--) info[k++] = (uint8_t)gout[j];
    }
    for (int i = 2; i < 4; i++) {
        char gin[23], gout[23];
        for (int j = 0; j < 23; j++) gin[j] = f.b[i][j];
        errs += golay2312(gin, gout);
        for (int j = 22; j > 10; j--) info[k++] = (uint8_t)gout[j];
    }
    for (int i = 4; i < 6; i++) {
        char hin[15], hout[15];
        for (int j = 0; j < 15; j++) hin[j] = f.b[i][j];
        errs += hamming(hin, hout, kHamming7100);
        for (int j = 14; j >= 4; j--) info[k++] = (uint8_t)hout[j];
    }
    for (int j = 22; j >= 0; j--) info[k++] = (uint8_t)f.b[6][j];
    if (errsOut) *errsOut = errs;
    return errs < 10;
}

namespace {
void kFromInfo(const uint8_t* d, int& K) {
    int b0 = 0;
    for (int i = 1; i <= 6; i++) b0 = (b0 << 1) | d[i];
    b0 = (b0 << 1) | d[86]; b0 = (b0 << 1) | d[87];
    const float w0 = (float)(4 * M_PI) / ((float)b0 + 39.5f);
    const int L = (int)(0.9254 * (int)((M_PI / w0) + 0.25));
    K = L < 37 ? (int)((float)(L + 2) / 3.0f) : 12;
}
}

void convert7100to7200(uint8_t d[kInfoBits]) {
    int K;
    kFromInfo(d, K);
    uint8_t t[88] = {};
    t[87] = d[0];
    t[48 + K] = d[42];
    t[49 + K] = d[43];
    int j = 48, k = 44;
    for (int i = 0; i < K; i++) t[j++] = d[k++];
    j = 0; k = 1;
    while (j < 87) {
        t[j] = d[k];
        if (++j == 48) j += K + 2;
        if (++k == 42) k += K + 2;
    }
    std::memcpy(d, t, 88);
}

void convert7200to7100(uint8_t d[kInfoBits]) {
    // the same K is found from the 7100 bits, so it is searched: the arrangement that converts back to the input
    uint8_t in[88];
    std::memcpy(in, d, 88);
    for (int K = 3; K <= 12; K++) {
        uint8_t t[88] = {};
        t[0] = in[87];
        t[42] = in[48 + K]; t[43] = in[49 + K];
        int j = 48, k = 44;
        for (int i = 0; i < K; i++) t[k++] = in[j++];
        j = 0; k = 1;
        while (j < 87) {
            t[k] = in[j];
            if (++j == 48) j += K + 2;
            if (++k == 42) k += K + 2;
        }
        uint8_t check[88];
        std::memcpy(check, t, 88);
        convert7100to7200(check);
        if (std::memcmp(check, in, 88) == 0) { std::memcpy(d, t, 88); return; }
    }
    std::memset(d, 0, 88);
}

void encode7200(const uint8_t info[kInfoBits], Frame7200& f) {
    std::memset(&f, 0, sizeof f);
    int k = 0;
    // c0..c3: Golay(23,12), information at 22..11
    for (int i = 0; i < 4; i++) {
        int d = 0;
        for (int j = 22; j > 10; j--) { f.b[i][j] = (char)info[k]; d = (d << 1) | info[k]; k++; }
        const int p = golayParity(d);
        for (int j = 10; j >= 0; j--) f.b[i][j] = (char)((p >> j) & 1);
    }
    // c4..c6: Hamming (15,11)
    for (int i = 4; i < 7; i++) {
        int d = 0;
        for (int j = 14; j >= 4; j--) { d = (d << 1) | info[k]; k++; }
        const int w = hammingEncode(d, kHamming);
        for (int j = 0; j < 15; j++) f.b[i][j] = (char)((w >> j) & 1);
    }
    for (int j = 6; j >= 0; j--) f.b[7][j] = (char)info[k++];
    modulate7200(f);
}

namespace {
// One entry per bit after the sync: -1 skipped; otherwise the frame (0..3) and the table index into kPW / kPX. Filled once from the order the fields come in on the air.
struct Slot { int8_t frame; int16_t idx; };
std::vector<Slot> schedule;

void addPair(int f1, int f2) {
    int idx = 0;
    const auto put = [&](int frame, int from, int n) { for (int k = 0; k < n; k++) schedule.push_back({(int8_t)frame, (int16_t)(from + k)}); };
    const auto line = [&](int n) { put(f1, idx, n); put(f2, idx, n); idx += n; };
    for (int i = 0; i < 11; i++) line(6);
    put(f1, idx, 6); put(f2, idx, 4);
    schedule.push_back({-1, 0}); schedule.push_back({-1, 0});   // spacer
    put(f2, idx + 4, 2);
    idx += 6;
    for (int i = 0; i < 3; i++) line(6);
    line(5);
    for (int i = 0; i < 7; i++) line(6);
    line(5);
}

const std::vector<Slot>& sched() {
    if (schedule.empty()) {
        for (int i = 0; i < 144; i++) schedule.push_back({-1, 0});      // 64 bits, the lid (16), 64 bits
        addPair(0, 1);
        for (int i = 0; i < 2 + 16; i++) schedule.push_back({-1, 0});   // spacer, 16 bits
        addPair(2, 3);
        for (int i = 0; i < 2; i++) schedule.push_back({-1, 0});
    }
    return schedule;
}

// the sync words of DSD, characters 1 / 3 as payload bits 0 / 1 (dsd_dibit.c)
uint32_t syncFrom(const char* s) { uint32_t v = 0; for (; *s; s++) v = (v << 1) | (*s == '3' ? 1u : 0u); return v; }
}

const uint32_t kSyncStdValue = syncFrom("13131333111311311133113311331133");
const uint32_t kSyncEaValue = syncFrom("31131311331331111133131311311133");

std::vector<uint8_t> makeBlock(const Frame7100 frames[4], uint32_t sync, uint16_t lid) {
    if (sync == kSyncStd) sync = kSyncStdValue;
    else if (sync == kSyncEa) sync = kSyncEaValue;
    std::vector<uint8_t> b((size_t)kBlockBits, 0);
    for (int i = 0; i < kSyncBits; i++) b[(size_t)i] = (uint8_t)((sync >> (31 - i)) & 1);
    const auto& sc = sched();
    for (size_t i = 0; i < sc.size(); i++) {
        const Slot& s = sc[i];
        uint8_t v = 0;
        if (s.frame >= 0) v = (uint8_t)frames[s.frame].b[kPW[s.idx]][kPX[s.idx]];
        else if (i >= 64 && i < 80) v = (uint8_t)((lid >> (79 - i)) & 1);
        b[(size_t)kSyncBits + i] = v;
    }
    return b;
}

void splitBlock(const uint8_t bits[736], Frame7100 frames[4], uint16_t* lid) {
    std::memset(frames, 0, sizeof(Frame7100) * 4);
    const auto& sc = sched();
    uint16_t l = 0;
    for (size_t i = 0; i < sc.size(); i++) {
        const Slot& s = sc[i];
        if (s.frame >= 0) frames[s.frame].b[kPW[s.idx]][kPX[s.idx]] = (char)(bits[i] & 1);
        else if (i >= 64 && i < 80) l = (uint16_t)((l << 1) | (bits[i] & 1));
    }
    if (lid) *lid = l;
}

} // namespace dect2::provoice
