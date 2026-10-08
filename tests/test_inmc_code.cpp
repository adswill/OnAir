// Inmarsat-C frame coding: known answers from the open decoder "inmarsatc" (Scytale-C library) and its documentation comments, the encoder against the
// decoder's own branch table, round trip, and the Viterbi decoder against the well known behaviour of the K = 7 rate 1/2 code.
// Nothing here is a recorded frame: no recording of a real signal was available.
#include "dect2/gen_util.h"
#include "dect2/inmc_code.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::inmc;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    // unique word: inmarsatc_decoder.h nrmPolUwPattern (64 symbols) and its reverse polarity table
    static const uint8_t uw[64] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0,
                                   0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0};
    CHECK(!memcmp(uw, kUw, 64), "unique word");
    static const uint8_t rev0[16] = {1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1};     // first row of revPolUwPattern
    for (int i = 0; i < 16; i++) CHECK(rev0[i] == (kUw[i] ^ 1), "reverse polarity pattern at %d", i);

    // scrambler: x^7 + x^5 + x^4 + x^3 from 0x80: the first eight groups worked out by hand from the rule (output bit 0, feedback into bit 7)
    static const uint8_t flags8[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    for (int i = 0; i < 8; i++) CHECK(scramblerFlag(i) == flags8[i], "scrambler flag %d is %d", i, scramblerFlag(i));
    int ones = 0;
    for (int i = 0; i < 160; i++) ones += scramblerFlag(i);
    printf("  scrambler: %d of the 160 groups are complemented\n", ones);
    CHECK(ones > 50 && ones < 110, "scrambler balance %d of 160", ones);

    // interleaver rows: the documentation comment of the Depermuter lists output 1 = row 0, output 2 = row 39, output 3 = row 14; j = (i * 23) mod 64
    CHECK(matrixRowOfTxRow(0) == 0 && matrixRowOfTxRow(1) == 39 && matrixRowOfTxRow(2) == 14, "row permutation start");
    bool seen[64] = {};
    for (int j = 0; j < 64; j++) {
        const int i = matrixRowOfTxRow(j);
        CHECK((i * 23) % 64 == j, "inverse permutation at %d", j);
        seen[i] = true;
    }
    for (int i = 0; i < 64; i++) CHECK(seen[i], "row %d never used", i);

    // convolutional code: the decoder's partabIdx (the symbol pair for each state with a zero input) must come out of the encoder
    static const uint8_t partabIdx[32] = {0, 1, 3, 2, 3, 2, 0, 1, 0, 1, 3, 2, 3, 2, 0, 1, 2, 3, 1, 0, 1, 0, 2, 3, 2, 3, 1, 0, 1, 0, 2, 3};
    for (int i = 0; i < 32; i++) CHECK(convOutput((unsigned)i << 1) == partabIdx[i], "branch table at state %d: %d", i, convOutput((unsigned)i << 1));
    // impulse response of the two generators: a single 1 followed by zeros reads out the polynomials
    for (int t = 0; t < 7; t++) {
        const uint8_t o = convOutput(1u << t);
        CHECK(((o >> 1) & 1) == ((0x6d >> t) & 1) && (o & 1) == ((0x4f >> t) & 1), "impulse response at %d", t);
    }

    // packet check bytes: worked out by hand from the reference rule for 7D 01 02 03 xx xx -> 7F FE
    uint8_t pk[6] = {0x7D, 1, 2, 3, 0, 0};
    packetCheckSet(pk, 6);
    CHECK(pk[4] == 0x7F && pk[5] == 0xFE, "check bytes %02X %02X", pk[4], pk[5]);
    CHECK(packetCheckOk(pk, 6), "check ok");
    int missed = 0, tried = 0;
    uint8_t q[20];
    for (int i = 0; i < 20; i++) q[i] = (uint8_t)(i * 37 + 5);
    packetCheckSet(q, 20);
    for (int i = 0; i < 20; i++)
        for (int v = 1; v < 256; v++) {
            uint8_t w[20];
            memcpy(w, q, 20);
            w[i] = (uint8_t)(w[i] + v);
            tried++;
            if (packetCheckOk(w, 20)) missed++;
        }
    printf("  single byte errors missed by the check: %d of %d\n", missed, tried);
    CHECK(missed == 0, "the check missed %d single byte errors", missed);

    // frame round trip, clean
    uint8_t info[kFrameBytes];
    uint32_t r = 12345;
    for (int i = 0; i < kFrameBytes; i++) { r = r * 1664525u + 1013904223u; info[i] = (uint8_t)(r >> 24); }
    static uint8_t sym[kFrameSyms];
    encodeFrame(info, sym);
    for (int j = 0; j < kRows; j++) CHECK(sym[j * kCols] == kUw[j] && sym[j * kCols + 1] == kUw[j], "unique word placement in row %d", j);
    CHECK(uwErrors(sym) == 0, "unique word errors on a clean frame");
    static float soft[kFrameSyms];
    for (int i = 0; i < kFrameSyms; i++) soft[i] = sym[i] ? 1.f : -1.f;
    FrameDecode d;
    decodeFrame(soft, d);
    CHECK(!memcmp(d.bytes, info, kFrameBytes - 1), "clean frame does not round trip");
    CHECK(d.symbolErrors == 0 && d.uwErrors == 0, "clean frame: %d symbol errors", d.symbolErrors);
    // the same frame inverted: the decoder is told the polarity, so it must fail until the caller flips it
    for (int i = 0; i < kFrameSyms; i++) soft[i] = -soft[i];
    decodeFrame(soft, d);
    CHECK(memcmp(d.bytes, info, kFrameBytes - 1) != 0 && d.uwErrors == 128, "inverted frame decoded without flipping (uw errors %d)", d.uwErrors);

    // Viterbi performance: information bit errors against Eb/N0 (soft decisions, rate 1/2: Es/N0 = Eb/N0 - 3 dB)
    genutil::PortableNormal nd;
    std::mt19937 rng(77);
    const double ebs[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double ber[5];
    for (int k = 0; k < 5; k++) {
        const double esn0 = std::pow(10.0, (ebs[k] - 3.0103) / 10.0);
        const float sigma = (float)std::sqrt(1.0 / (2.0 * esn0));
        long errs = 0, bits = 0;
        const int frames = 120;
        for (int f = 0; f < frames; f++) {
            for (int i = 0; i < kFrameBytes; i++) { r = r * 1664525u + 1013904223u; info[i] = (uint8_t)(r >> 24); }
            encodeFrame(info, sym);
            for (int i = 0; i < kFrameSyms; i++) soft[i] = (sym[i] ? 1.f : -1.f) + sigma * nd(rng);
            decodeFrame(soft, d);
            for (int i = 0; i < kFrameBytes - 1; i++) errs += __builtin_popcount((unsigned)(d.bytes[i] ^ info[i]));
            bits += (kFrameBytes - 1) * 8;
        }
        ber[k] = (double)errs / bits;
        printf("  Viterbi Eb/N0 %.0f dB: bit error rate %.2e (%ld of %ld)\n", ebs[k], ber[k], errs, bits);
    }
    // published: K = 7 rate 1/2 with unquantised soft decisions gives about 1e-5 at 4.4 dB and 1e-3 near 3 dB
    CHECK(ber[4] < 2e-4, "5 dB: %.2e", ber[4]);
    CHECK(ber[2] > 1e-4 && ber[2] < 2e-2, "3 dB: %.2e", ber[2]);
    CHECK(ber[0] > ber[2] && ber[2] > ber[4], "the error rate must fall with Eb/N0");
    // a polarity flip inside the frame (cycle slip of the carrier loop): the unique word fit finds the row, the repaired frame decodes
    {
        for (int i = 0; i < kFrameBytes; i++) { r = r * 1664525u + 1013904223u; info[i] = (uint8_t)(r >> 24); }
        encodeFrame(info, sym);
        const int rows[] = {10, 30, 55};
        for (int startRev = 0; startRev < 2; startRev++)
            for (int slipAt : rows) {
                uint8_t dd[kRows];
                for (int i = 0; i < kFrameSyms; i++) soft[i] = (sym[i] ? 1.f : -1.f) + 0.3f * nd(rng);
                for (int i = 0; i < kFrameSyms; i++) if ((i / kCols >= slipAt) != (startRev != 0)) soft[i] = -soft[i];
                for (int q = 0; q < kRows; q++) dd[q] = (uint8_t)(((soft[q * kCols] > 0) != (kUw[q] != 0)) + ((soft[q * kCols + 1] > 0) != (kUw[q] != 0)));
                bool sr; int sk;
                const int e = uwFit(dd, sr, sk);
                CHECK(sk == slipAt && sr == (startRev != 0) && e == 0, "flip at row %d (start reversed %d): found row %d, reversed %d, %d errors", slipAt, startRev, sk, (int)sr, e);
                for (int i = 0; i < kFrameSyms; i++) if ((i / kCols >= sk) != sr) soft[i] = -soft[i];
                decodeFrame(soft, d);
                CHECK(!memcmp(d.bytes, info, kFrameBytes - 1), "frame with a flip at row %d does not decode after the repair", slipAt);
            }
        // no flip: a clean frame, an inverted one and noisy ones must not be reported as flipped
        uint8_t dd[kRows];
        bool sr; int sk;
        memset(dd, 0, sizeof dd);
        CHECK(uwFit(dd, sr, sk) == 0 && sk == kRows && !sr, "clean unique word reported a flip");
        memset(dd, 2, sizeof dd);
        CHECK(uwFit(dd, sr, sk) == 0 && sk == kRows && sr, "inverted unique word reported a flip");
        int falseFlips = 0;
        for (int t = 0; t < 2000; t++) {
            for (int q = 0; q < kRows; q++) dd[q] = (uint8_t)((rng() % 100 < 15) + (rng() % 100 < 15));    // 15 % wrong symbols, no flip
            falseFlips += uwFit(dd, sr, sk) < 128 && sk != kRows;
        }
        CHECK(falseFlips == 0, "%d of 2000 noisy unique words reported a flip that is not there", falseFlips);
    }
    printf(fails ? "FAILED\n" : "inmc_code ok\n");
    return fails ? 1 : 0;
}
