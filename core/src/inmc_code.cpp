#include "dect2/inmc_code.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace inmc {

// inmarsatc_decoder.h UWFinder::nrmPolUwPattern, 64 symbols in row order
const uint8_t kUw[kRows] = {
    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,
    1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0,
    0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1,
    0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0};

namespace {
struct Tables {
    uint8_t flag[160];
    uint8_t out[128];
    Tables() {
        // inmarsatc Descrambler: start 0x80 (the manuals say 0x40, the decoder author found 0x80 correct), output bit 0, feedback x7^x5^x4^x3
        unsigned r = 0x80;
        for (int g = 0; g < 160; g++) {
            flag[g] = r & 1;
            const unsigned fb = (r & 1) ^ ((r >> 2) & 1) ^ ((r >> 3) & 1) ^ ((r >> 4) & 1);
            r = (r >> 1) | (fb << 7);
        }
        for (unsigned s = 0; s < 128; s++) {
            const unsigned a = __builtin_parity(s & 0x6d), b = __builtin_parity(s & 0x4f);
            out[s] = (uint8_t)((a << 1) | b);
        }
    }
};
const Tables& T() { static const Tables t; return t; }
} // namespace

uint8_t scramblerFlag(int g) { return T().flag[g]; }

void scrambleBytes(uint8_t b[kFrameBytes]) {
    for (int g = 0; g < 160; g++)
        if (T().flag[g])
            for (int k = 0; k < 4; k++) b[g * 4 + k] = (uint8_t)~b[g * 4 + k];
}

uint8_t convOutput(unsigned reg7) { return T().out[reg7 & 127]; }

void encodeFrame(const uint8_t info[kFrameBytes], uint8_t sym[kFrameSyms]) {
    uint8_t b[kFrameBytes];
    std::memcpy(b, info, kFrameBytes);
    scrambleBytes(b);
    b[kFrameBytes - 1] = 0;     // the flush byte is added after scrambling so that the encoder ends in state 0
    static thread_local uint8_t coded[kCodedSyms];
    unsigned reg = 0;
    int k = 0;
    for (int n = 0; n < kFrameBytes; n++)
        for (int bit = 0; bit < 8; bit++) {      // least significant bit first
            reg = ((reg << 1) | ((b[n] >> bit) & 1)) & 127;
            const uint8_t o = T().out[reg];
            coded[k++] = o >> 1;
            coded[k++] = o & 1;
        }
    // the coded stream is written down the columns of a 64 x 160 matrix; rows go out in permuted order, each after its unique word symbols
    for (int j = 0; j < kRows; j++) {
        const int i = matrixRowOfTxRow(j);
        uint8_t* p = sym + j * kCols;
        p[0] = p[1] = kUw[j];
        for (int c = 0; c < kDataCols; c++) p[2 + c] = coded[c * kRows + i];
    }
}

int uwErrors(const uint8_t* h) {
    int e = 0;
    for (int r = 0; r < kRows; r++) e += (h[r * kCols] != kUw[r]) + (h[r * kCols + 1] != kUw[r]);
    return e;
}

int uwFit(const uint8_t d[kRows], bool& startRev, int& slipRow) {
    int all = 0;
    for (int r = 0; r < kRows; r++) all += d[r];
    startRev = all > kRows;
    slipRow = kRows;
    int best = startRev ? 2 * kRows - all : all;
    const int plain = best;
    for (int rev = 0; rev < 2; rev++) {
        int c = 0;           // wrong symbols so far when the frame starts with polarity rev and flips before row k
        int tail = 0;        // wrong symbols of the rows from k on, in the flipped polarity
        for (int r = 0; r < kRows; r++) tail += rev ? d[r] : 2 - d[r];
        for (int k = 1; k < kRows; k++) {
            c += rev ? 2 - d[k - 1] : d[k - 1];
            tail -= rev ? d[k - 1] : 2 - d[k - 1];
            if (c + tail + 8 <= plain && c + tail < best) { best = c + tail; slipRow = k; startRev = rev != 0; }
        }
    }
    return best;
}

void decodeFrame(const float soft[kFrameSyms], FrameDecode& out) {
    static thread_local float coded[kCodedSyms];
    int uwErr = 0;
    for (int j = 0; j < kRows; j++) {
        const int i = matrixRowOfTxRow(j);
        const float* p = soft + j * kCols;
        uwErr += ((p[0] > 0) != (kUw[j] != 0)) + ((p[1] > 0) != (kUw[j] != 0));
        for (int c = 0; c < kDataCols; c++) coded[c * kRows + i] = p[2 + c];
    }
    out.uwErrors = uwErr;
    // Viterbi, 64 states, state = last six bits (newest lowest), start in state 0, end in state 0 (the flush byte)
    const int nb = kCodedSyms / 2;
    static thread_local uint64_t dec[kCodedSyms / 2];
    float pm[64], nm[64];
    for (int s = 0; s < 64; s++) pm[s] = -1e30f;
    pm[0] = 0;
    uint8_t outTab[128];
    for (int s = 0; s < 128; s++) outTab[s] = T().out[s];
    for (int t = 0; t < nb; t++) {
        const float s0 = coded[2 * t], s1 = coded[2 * t + 1];
        float bm[4];   // metric of expected pair (first symbol << 1 | second): correlation with +-1
        bm[0] = -s0 - s1; bm[1] = -s0 + s1; bm[2] = s0 - s1; bm[3] = s0 + s1;
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const int bit = ns & 1, p0 = ns >> 1, p1 = p0 | 32;
            const float m0 = pm[p0] + bm[outTab[(p0 << 1) | bit]];
            const float m1 = pm[p1] + bm[outTab[(p1 << 1) | bit]];
            if (m1 > m0) { nm[ns] = m1; d |= (uint64_t)1 << ns; } else nm[ns] = m0;
        }
        dec[t] = d;
        std::memcpy(pm, nm, sizeof pm);
        if ((t & 63) == 63) {   // keep the metrics in range
            float mx = pm[0];
            for (int s = 1; s < 64; s++) mx = std::max(mx, pm[s]);
            for (int s = 0; s < 64; s++) pm[s] -= mx;
        }
    }
    out.pathMetric = pm[0];
    static thread_local uint8_t bits[kCodedSyms / 2];
    int st = 0;
    for (int t = nb - 1; t >= 0; t--) {
        bits[t] = st & 1;
        const int p0 = st >> 1;
        st = ((dec[t] >> st) & 1) ? (p0 | 32) : p0;
    }
    for (int n = 0; n < kFrameBytes; n++) {
        uint8_t v = 0;
        for (int bit = 0; bit < 8; bit++) v |= (uint8_t)(bits[n * 8 + bit] << bit);
        out.bytes[n] = v;
    }
    // channel symbol errors: re-encode the decoded bits and compare with the signs received
    int se = 0;
    unsigned reg = 0;
    for (int t = 0; t < nb; t++) {
        reg = ((reg << 1) | bits[t]) & 127;
        const uint8_t o = outTab[reg];
        se += (((o >> 1) & 1) != (coded[2 * t] > 0)) + ((o & 1) != (coded[2 * t + 1] > 0));
    }
    out.symbolErrors = se;
    scrambleBytes(out.bytes);
}

static void sums(const uint8_t* p, int len, int& c0, int& c1) {
    // inmarsatc PacketDecoder::computeCRC: the two check bytes count as zero; both sums run over all len bytes
    c0 = 0; c1 = 0;
    for (int i = 0; i < len; i++) {
        c0 = (c0 + (i < len - 2 ? p[i] : 0)) & 0xFFFF;
        c1 = (c1 + c0) & 0xFFFF;
    }
}

void packetCheckSet(uint8_t* p, int len) {
    int c0, c1;
    sums(p, len, c0, c1);
    p[len - 2] = (uint8_t)(c0 - c1);
    p[len - 1] = (uint8_t)(c1 - 2 * c0);
}

bool packetCheckOk(const uint8_t* p, int len) {
    if (len < 3) return false;
    int c0, c1;
    sums(p, len, c0, c1);
    return p[len - 2] == (uint8_t)(c0 - c1) && p[len - 1] == (uint8_t)(c1 - 2 * c0);
}

} // namespace inmc
} // namespace dect2
