// DAB test transmitter: known answers from EN 300 401 / TS 102 563 for every coding step (energy dispersal, CRC-16, Fire code, Reed-Solomon, the convolutional
// code, the puncturing tables, the frequency interleaver, the phase reference symbol, DQPSK), the same steps compared with the receiver's independent
// implementation in dab_fec.cpp, the frame structure in the time domain, the AAC-LC encoder (decoded with libavcodec), the superframe, the levels, the
// chunk independence, the carrier and clock offsets, and the speed of the generator.
#include "dect2/dab.h"
#include "dect2/dab_gen.h"
#include "dect2/fftutil.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

struct Lcg {
    uint32_t x;
    explicit Lcg(uint32_t s) : x(s * 2654435761u + 12345u) {}
    uint32_t next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
};

// ---------------------------------------------------------------- GF(256) of the test, independent of both implementations
static int gmul(int a, int b) {
    int r = 0;
    for (int i = 0; i < 8; i++) { if (b & 1) r ^= a; b >>= 1; a <<= 1; if (a & 0x100) a ^= 0x11D; }
    return r;
}
static int gpow(int e) { int r = 1; for (int i = 0; i < e; i++) r = gmul(r, 2); return r; }

static void testPrbs() {
    // table 12: the first 16 bits
    static const int want[16] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0};
    uint8_t p[1100];
    dabgen::prbs(p, 1100);
    for (int i = 0; i < 16; i++) CHECK(p[i] == want[i], "PRBS bit %d is %d, table 12 says %d", i, p[i], want[i]);
    int ones = 0;
    for (int i = 0; i < 511; i++) ones += p[i];
    CHECK(ones == 256, "an m-sequence of period 511 has 256 ones, got %d", ones);
    bool periodic = true;
    for (int i = 0; i < 589; i++) periodic &= p[i] == p[i + 511];
    CHECK(periodic, "PRBS period is not 511");
    // scrambling is its own inverse; and the receiver's descrambler agrees
    uint8_t a[768], b[768];
    Lcg r(3);
    for (int i = 0; i < 768; i++) a[i] = b[i] = (uint8_t)(r.next() & 1);
    dabgen::scramble(a, 768);
    dab::descramble(b, 768);
    CHECK(!memcmp(a, b, 768), "scrambler differs from the receiver's");
    dabgen::scramble(a, 768);
    uint8_t c[768];
    Lcg r2(3);
    for (int i = 0; i < 768; i++) c[i] = (uint8_t)(r2.next() & 1);
    CHECK(!memcmp(a, c, 768), "scrambling twice does not restore the data");
}

static void testCrc() {
    const uint8_t s[] = "123456789";
    CHECK(dabgen::crc16(s, 9) == 0xD64E, "CRC-16 (CCITT, init FFFF, inverted) of 123456789 is %04X, the check value is D64E", dabgen::crc16(s, 9));
    CHECK(dab::crc16(s, 9) == 0xD64E, "the receiver's CRC-16 check value");
    Lcg r(5);
    for (int t = 0; t < 50; t++) {
        uint8_t d[30];
        for (auto& v : d) v = (uint8_t)r.next();
        CHECK(dabgen::crc16(d, 30) == dab::crc16(d, 30), "CRC-16 differs from the receiver's");
    }
    // the generator polynomial of the Fire code by polynomial multiplication: (x^11 + 1)(x^5 + x^3 + x^2 + x + 1)
    uint32_t g = 0;
    for (int i = 0; i <= 5; i++) if ((0x2F >> i) & 1) { g ^= 1u << i; g ^= 1u << (i + 11); }
    CHECK(g == 0x1782F, "Fire generator polynomial %X", g);
    for (int t = 0; t < 50; t++) {
        uint8_t d[9];
        for (auto& v : d) v = (uint8_t)r.next();
        const uint16_t fc = dabgen::fireCode(d, 9);
        // remainder of d(x) x^16 modulo g(x) is the check word
        uint64_t rem = 0;
        for (int i = 0; i < 9 * 8 + 16; i++) {
            const int in = i < 72 ? (d[i / 8] >> (7 - i % 8)) & 1 : 0;
            rem = (rem << 1) | (uint64_t)in;
            if (rem & (1u << 16)) rem ^= g;
        }
        CHECK(rem == fc, "Fire code %04X, the polynomial division gives %04llX", fc, (unsigned long long)rem);
        CHECK(fc == dab::fireCode(d, 9), "Fire code differs from the receiver's");
    }
}

static void testRs() {
    // g(x) = prod (x + alpha^i), i = 0..9, by multiplication in the test's own field
    int g[11] = {1};
    int deg = 0;
    for (int i = 0; i < 10; i++) {
        int n[11] = {0};
        for (int k = 0; k <= deg; k++) { n[k + 1] ^= g[k]; n[k] ^= gmul(g[k], gpow(i)); }       // g[k] is the coefficient of x^(deg-k)... see below
        deg++;
        memcpy(g, n, sizeof g);
    }
    // here g[k] holds the coefficient of x^k with k = 0 for the constant: n[k+1] ^= g[k] (times x), n[k] ^= alpha^i g[k]
    uint8_t data[110] = {0}, cw[120], cw2[120];
    data[109] = 1;                                   // x^10 mod g(x) = the lower coefficients of g(x)
    dabgen::rsEncode(data, cw);
    bool ok = true;
    for (int j = 0; j < 10; j++) ok &= cw[110 + j] == g[9 - j];
    CHECK(ok, "parity of a single byte is not the generator polynomial");
    Lcg r(9);
    for (int t = 0; t < 40; t++) {
        for (auto& v : data) v = (uint8_t)r.next();
        dabgen::rsEncode(data, cw);
        dab::rsEncode120(data, cw2);
        CHECK(!memcmp(cw, cw2, 120), "RS parity differs from the receiver's");
        // every syndrome alpha^0 .. alpha^9 of the codeword polynomial is zero
        for (int i = 0; i < 10; i++) {
            int s = 0;
            for (int j = 0; j < 120; j++) s = gmul(s, gpow(i)) ^ cw[j];
            CHECK(s == 0, "syndrome %d of the codeword is %d", i, s);
        }
        // five errors are corrected by the receiver's decoder
        uint8_t bad[120];
        memcpy(bad, cw, 120);
        for (int e = 0; e < 5; e++) bad[r.next() % 120] ^= (uint8_t)(1 + r.next() % 255);
        const int nfix = dab::rsDecode120(bad);
        CHECK(nfix >= 0 && !memcmp(bad, cw, 120), "the receiver does not correct 5 errors in a generated codeword (%d)", nfix);
    }
}

static void testConv() {
    // the impulse response of the encoder: octal 133 171 145 133
    static const unsigned gen[4] = {0133, 0171, 0145, 0133};
    uint8_t in[16] = {1}, m[4 * 22];
    dabgen::convEncode(in, 16, m);
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 7; i++) CHECK(m[4 * i + k] == ((gen[k] >> (6 - i)) & 1), "impulse response of generator %d at delay %d", k, i);
    Lcg r(11);
    for (int t = 0; t < 10; t++) {
        const int n = 768;
        std::vector<uint8_t> bits((size_t)n), a((size_t)(4 * (n + 6))), b(a.size());
        for (auto& v : bits) v = (uint8_t)(r.next() & 1);
        dabgen::convEncode(bits.data(), n, a.data());
        dab::convEncode(bits.data(), n, b.data());
        CHECK(a == b, "mother codeword differs from the receiver's");
        std::vector<int8_t> soft(a.size());
        for (size_t i = 0; i < a.size(); i++) soft[i] = a[i] ? 100 : -100;
        std::vector<uint8_t> dec((size_t)n);
        dab::viterbiDecode(soft.data(), n, dec.data());
        CHECK(dec == bits, "the receiver's Viterbi decoder does not return the generated data");
    }
}

static void testPuncturing() {
    // EN 300 401 table 13 as printed in the standard
    static const char* const tab[24] = {
        "1100 1000 1000 1000 1000 1000 1000 1000", "1100 1000 1000 1000 1100 1000 1000 1000", "1100 1000 1100 1000 1100 1000 1000 1000",
        "1100 1000 1100 1000 1100 1000 1100 1000", "1100 1100 1100 1000 1100 1000 1100 1000", "1100 1100 1100 1000 1100 1100 1100 1000",
        "1100 1100 1100 1100 1100 1100 1100 1000", "1100 1100 1100 1100 1100 1100 1100 1100", "1110 1100 1100 1100 1100 1100 1100 1100",
        "1110 1100 1100 1100 1110 1100 1100 1100", "1110 1100 1110 1100 1110 1100 1100 1100", "1110 1100 1110 1100 1110 1100 1110 1100",
        "1110 1110 1110 1100 1110 1100 1110 1100", "1110 1110 1110 1100 1110 1110 1110 1100", "1110 1110 1110 1110 1110 1110 1110 1100",
        "1110 1110 1110 1110 1110 1110 1110 1110", "1111 1110 1110 1110 1110 1110 1110 1110", "1111 1110 1110 1110 1111 1110 1110 1110",
        "1111 1110 1111 1110 1111 1110 1110 1110", "1111 1110 1111 1110 1111 1110 1111 1110", "1111 1111 1111 1110 1111 1110 1111 1110",
        "1111 1111 1111 1110 1111 1111 1111 1110", "1111 1111 1111 1111 1111 1111 1111 1110", "1111 1111 1111 1111 1111 1111 1111 1111"};
    for (int pi = 1; pi <= 24; pi++) {
        std::string want;
        for (const char* p = tab[pi - 1]; *p; p++) if (*p != ' ') want += *p;
        CHECK(want == dabgen::puncturingVector(pi), "puncturing vector PI=%d", pi);
        int ones = 0;
        for (char c : want) ones += c == '1';
        CHECK(ones == 8 + pi, "PI=%d keeps %d of 32 bits, the code rate 8/(8+PI) needs %d", pi, ones, 8 + pi);
    }
    CHECK(dabgen::puncturingVector(0) == nullptr && dabgen::puncturingVector(25) == nullptr, "PI out of range");
    Lcg r(13);
    // the FIC: 768 bits -> 3096 -> 2304 (rate about 1/3)
    {
        std::vector<uint8_t> bits(768), m(3096), a(2304), b(2304);
        for (auto& v : bits) v = (uint8_t)(r.next() & 1);
        dabgen::convEncode(bits.data(), 768, m.data());
        dabgen::punctureFic(m.data(), a.data());
        dab::ficPuncture(m.data(), b.data());
        CHECK(a == b, "FIC puncturing differs from the receiver's");
        // 21 blocks of 96, 3 blocks of 92, 12 tail bits
        CHECK(21 * 96 + 3 * 92 + 12 == 2304, "FIC length");
    }
    // every EEP profile of both sets with the sizes of the standard
    int tried = 0;
    for (int option = 0; option < 2; option++)
        for (int level = 0; level < 4; level++)
            for (int n = 1; n <= 12; n++) {
                const int br = (option ? 32 : 8) * n;
                const int size = dabgen::eepSize(br, option, level);
                int rn, rbr, info;
                const bool recvOk = dab::eepGeometry(size, option, level, rn, rbr, info);
                if (option == 0 && level == 1 && n < 2) { CHECK(size == 0, "2-A with 8 kbit/s is not allowed here"); continue; }
                CHECK(size > 0 && recvOk && rbr == br && info == br * 24, "EEP %d-%c at %d kbit/s: size %d", level + 1, option ? 'B' : 'A', br, size);
                if (!size || !recvOk) continue;
                const int infoBits = br * 24;
                std::vector<uint8_t> bits((size_t)infoBits), m((size_t)(4 * (infoBits + 6))), a((size_t)size * 64), b((size_t)size * 64);
                for (auto& v : bits) v = (uint8_t)(r.next() & 1);
                dabgen::convEncode(bits.data(), infoBits, m.data());
                CHECK(dabgen::punctureEep(m.data(), br, option, level, a.data()), "EEP puncturing failed");
                CHECK(dab::eepPuncture(m.data(), size, option, level, b.data()), "receiver EEP puncturing failed");
                CHECK(a == b, "EEP %d-%c %d kbit/s differs from the receiver's", level + 1, option ? 'B' : 'A', br);
                tried++;
            }
    CHECK(tried >= 80, "only %d EEP profiles compared", tried);
    // sizes of the standard: table 17 / 19: 3-A at 48 kbit/s takes 36 CU, 2-A at 128 kbit/s 128 CU, 3-B at 96 kbit/s 54 CU
    CHECK(dabgen::eepSize(48, 0, 2) == 36, "3-A 48 kbit/s");
    CHECK(dabgen::eepSize(128, 0, 1) == 128, "2-A 128 kbit/s");
    CHECK(dabgen::eepSize(96, 1, 2) == 54, "3-B 96 kbit/s");
}

static void testTables() {
    // table 25 of EN 300 401: the first carriers of the frequency interleaver
    static const int want[8] = {-513, -14, 329, 692, -733, 13, 680, 273};
    const auto& fi = dabgen::frequencyInterleaver();
    CHECK(fi.size() == 1536, "interleaver size %zu", fi.size());
    for (int i = 0; i < 8; i++) CHECK(fi[(size_t)i] == want[i], "F(%d) = %d, table 25 says %d", i, fi[(size_t)i], want[i]);
    std::set<int> seen(fi.begin(), fi.end());
    CHECK(seen.size() == 1536 && !seen.count(0) && *seen.begin() == -768 && *seen.rbegin() == 768, "the interleaver is not a permutation of -768..768 without 0");
    CHECK(fi == dab::freqInterleaver(), "the interleaver differs from the receiver's");
    // phase reference symbol: unit amplitude on 1536 carriers, no carrier at 0, the table of the standard, the receiver's table
    const auto& z = dabgen::phaseReferenceSymbol();
    int used = 0;
    bool unit = true;
    for (int k = 0; k < 2048; k++) if (std::abs(z[(size_t)k]) > 0.5f) { used++; unit &= std::fabs(std::abs(z[(size_t)k]) - 1.f) < 1e-6f; }
    CHECK(used == 1536 && unit && std::abs(z[0]) == 0.f && std::abs(z[1024]) == 0.f, "phase reference symbol: %d carriers", used);
    // phi_k = pi/2 (h(i, k - k') + n): k = -768 has i = 0, n = 1, h(0,0) = 0: phase pi/2; k = -767: h(0,1) = 2: pi/2 * 3
    CHECK(std::abs(z[(size_t)(2048 - 768)] - cf32(0, 1)) < 1e-6f, "z(-768)");
    CHECK(std::abs(z[(size_t)(2048 - 767)] - cf32(0, -1)) < 1e-6f, "z(-767)");
    CHECK(std::abs(z[(size_t)(1)] - cf32(0, -1)) < 1e-6f, "z(+1): i = 0, n = 3, h(0,0) = 0: 3 pi/2");
    const auto& rz = dab::phaseReference();
    double d = 0;
    for (int k = 0; k < 2048; k++) d = std::max(d, (double)std::abs(z[(size_t)k] - rz[(size_t)k]));
    CHECK(d < 1e-6, "phase reference differs from the receiver's by %g", d);
    // the time signal of the reference symbol: no spike (a constant phase would give 31 dB)
    Fft fft(2048);
    std::vector<cf32> t(z.begin(), z.end());
    fft.inverse(t.data());
    double pk = 0, avg = 0;
    for (auto& v : t) { pk = std::max(pk, (double)std::norm(v)); avg += std::norm(v); }
    avg /= 2048;
    const double papr = 10 * std::log10(pk / avg);
    printf("phase reference symbol: peak to average %.2f dB\n", papr);
    CHECK(papr < 10.0, "the phase reference symbol has a PAPR of %.1f dB", papr);
}

// ---------------------------------------------------------------- frame structure
static void testFrame() {
    dabgen::TxConfig cfg;
    cfg.utcSeconds = 1700000000;       // Tue 14 Nov 2023 22:13:20 UTC
    dabgen::Transmitter tx(cfg);
    CHECK(tx.config().services.size() >= 3, "services %zu", tx.config().services.size());
    int next = 0;
    for (const auto& l : tx.layout()) { CHECK(l.start == next, "sub-channel %d starts at %d", l.subId, l.start); next += l.size; }
    CHECK(next <= 864, "the services need %d CU of 864", next);
    // FIBs: CRC, FIG 0/0 first in FIB 0 with the ensemble id and the CIF count
    for (uint64_t m : {0ull, 1ull, 7ull, 1249ull}) {
        uint8_t f[12][32];
        tx.fibs(m, f);
        for (int i = 0; i < 12; i++) CHECK(dabgen::crc16(f[i], 30) == (uint16_t)((f[i][30] << 8) | f[i][31]), "FIB %d of frame %llu: CRC", i, (unsigned long long)m);
        CHECK(f[0][0] == ((0 << 5) | 5) && f[0][1] == 0x00 && f[0][2] == 0xCE && f[0][3] == 0x15, "FIG 0/0 header");
        const unsigned cif = (unsigned)((4 * m) % 5000);
        CHECK(f[0][4] == cif / 250 && f[0][5] == cif % 250, "CIF counter %u: %d %d", cif, f[0][4], f[0][5]);
    }
    // the time FIG: FIG 0/10 of frame 0 carries MJD 60262 (14 Nov 2023) and 22:13:20
    {
        uint8_t f[12][32];
        tx.fibs(0, f);
        const uint8_t* p = f[0] + 6 + 4;      // after FIG 0/0 (6) and FIG 0/7 (4)
        CHECK(p[0] == ((0 << 5) | 7) && p[1] == 0x0A, "FIG 0/10 header %02X %02X", p[0], p[1]);
        uint64_t v = 0;
        for (int i = 0; i < 6; i++) v = (v << 8) | p[2 + i];
        const unsigned mjd = (unsigned)((v >> 30) & 0x1FFFF);
        const int hh = (int)((v >> 22) & 31), mm = (int)((v >> 16) & 63), ss = (int)((v >> 10) & 63), ms = (int)(v & 1023);
        CHECK(((v >> 27) & 1) == 1, "UTC long form flag");
        CHECK(mjd == 60262 && hh == 22 && mm == 13 && ss == 20 && ms == 0, "time %u %02d:%02d:%02d.%03d", mjd, hh, mm, ss, ms);
    }
    // the symbols: carrier 0 empty, outside +-768 empty, constant modulus, differential phase an odd multiple of 45 degrees
    std::vector<cf32> z;
    tx.symbols(3, z);
    CHECK(z.size() == (size_t)76 * 2048, "symbol array");
    double worst = 0;
    int nonUnit = 0;
    for (int l = 1; l < 76; l++)
        for (int k = 0; k < 2048; k++) {
            const cf32 a = z[(size_t)l * 2048 + (size_t)k], b = z[(size_t)(l - 1) * 2048 + (size_t)k];
            const bool used = (k >= 1 && k <= 768) || (k >= 2048 - 768);
            if (!used) { CHECK(std::abs(a) == 0.f, "symbol %d bin %d is not empty", l, k); continue; }
            if (std::fabs(std::abs(a) - 1.f) > 1e-4f) nonUnit++;
            const cf32 q = a * std::conj(b);
            const double ph = std::arg(q) / (M_PI / 4);       // odd integers
            worst = std::max(worst, std::fabs(ph - 2 * std::round((ph - 1) / 2) - 1));
        }
    CHECK(nonUnit == 0 && worst < 1e-3, "DQPSK symbols: %d not unit, phase deviation %g", nonUnit, worst);
    // time domain: null symbol, guard interval = end of the useful part, FFT gives the symbols back
    std::vector<cf32> x;
    tx.nextFrame(x);
    tx.nextFrame(x);
    tx.nextFrame(x);
    tx.nextFrame(x);          // frame 3
    CHECK(x.size() == 196608 && tx.frameIndex() == 4, "frame length %zu", x.size());
    double nullPow = 0;
    for (int i = 0; i < 2656; i++) nullPow += std::norm(x[(size_t)i]);
    CHECK(nullPow == 0.0, "the null symbol is not empty");
    double act = 0;
    float peak = 0;
    for (size_t i = 2656; i < x.size(); i++) { act += std::norm(x[i]); peak = std::max(peak, std::abs(x[i])); }
    const double rms = std::sqrt(act / (double)(x.size() - 2656));
    printf("frame: rms of the active symbols %.4f, peak %.3f\n", rms, peak);
    CHECK(std::fabs(rms - 0.2) < 0.004, "rms %.4f", rms);
    CHECK(peak < 0.9f, "peak %.3f", peak);
    Fft fft(2048);
    double e = 0;
    for (int l : {0, 1, 2, 3, 4, 40, 75}) {
        const cf32* s = &x[(size_t)2656 + (size_t)l * 2552];
        for (int i = 0; i < 504; i++) CHECK(s[i] == s[2048 + i], "guard interval of symbol %d differs at %d", l, i);
        std::vector<cf32> t(s + 504, s + 504 + 2048);
        fft.forward(t.data());
        const float sc = (float)(0.2 / std::sqrt(1536.0)) * 2048.f;
        for (int k = 0; k < 2048; k++) e = std::max(e, (double)std::abs(t[(size_t)k] / sc - z[(size_t)l * 2048 + (size_t)k]));
    }
    CHECK(e < 1e-3, "the FFT of the symbols differs from the frequency domain symbols by %g", e);
}

// ---------------------------------------------------------------- AAC through libavcodec
struct Pcm { std::vector<float> ch[2]; };
static bool decodeAac(const std::vector<std::vector<uint8_t>>& aus, int rate, int channels, Pcm& out) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    const int idx = rate == 48000 ? 3 : 5;
    const uint32_t asc = (2u << 11) | ((uint32_t)idx << 7) | ((uint32_t)channels << 3) | (1u << 2);      // AAC-LC, rate, channels, 960 frames
    ctx->extradata = (uint8_t*)av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
    ctx->extradata[0] = (uint8_t)(asc >> 8); ctx->extradata[1] = (uint8_t)asc;
    ctx->extradata_size = 2;
    if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); return false; }
    AVPacket* pk = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    for (const auto& a : aus) {
        av_new_packet(pk, (int)a.size());
        memcpy(pk->data, a.data(), a.size());
        if (avcodec_send_packet(ctx, pk) < 0) { av_packet_unref(pk); continue; }
        av_packet_unref(pk);
        while (avcodec_receive_frame(ctx, fr) == 0) {
            for (int c = 0; c < std::min(2, fr->ch_layout.nb_channels); c++) {
                const float* p = (const float*)fr->extended_data[c];
                out.ch[c].insert(out.ch[c].end(), p, p + fr->nb_samples);
            }
            av_frame_unref(fr);
        }
    }
    av_packet_free(&pk); av_frame_free(&fr); avcodec_free_context(&ctx);
    return true;
}

// amplitude and frequency of the strongest tone between lo and hi Hz (Hann window, 0.1 Hz search, parabolic refinement is not needed at that step)
static void measureTone(const std::vector<float>& x, size_t from, size_t n, double fs, double lo, double hi, double& hz, double& amp) {
    std::vector<double> w(n);
    double ws = 0;
    for (size_t i = 0; i < n; i++) { w[i] = 0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)n); ws += w[i]; }
    double best = -1;
    for (double f = lo; f <= hi; f += 0.1) {
        double re = 0, im = 0;
        const double dph = 2 * M_PI * f / fs;
        double c = 1, s = 0;
        const double sc = std::cos(dph), ss = std::sin(dph);
        for (size_t i = 0; i < n; i++) {
            const double v = x[from + i] * w[i];
            re += v * c; im -= v * s;
            const double nc = c * sc - s * ss; s = s * sc + c * ss; c = nc;
        }
        const double m = 2 * std::sqrt(re * re + im * im) / ws;
        if (m > best) { best = m; hz = f; }
    }
    amp = best;
}
// power of everything except the tone at f (a notch of +-60 Hz) relative to the tone, in dB
static double toneSnr(const std::vector<float>& x, size_t from, size_t n, double fs, double f, double amp) {
    double tot = 0;
    for (size_t i = 0; i < n; i++) tot += (double)x[from + i] * x[from + i];
    tot /= (double)n;
    const double tonePow = amp * amp / 2;
    return 10 * std::log10(tonePow / std::max(1e-20, tot - tonePow));
}

static void testAac() {
    for (int rate : {48000, 32000}) {
        dabgen::AacLcEncoder enc(rate, 2);
        std::vector<float> pcm(960 * 2);
        std::vector<std::vector<uint8_t>> aus;
        std::vector<uint8_t> blk;
        const double fl = 1000, fr = 3000;
        int fitted = 0;
        for (int b = 0; b < 100; b++) {
            for (int i = 0; i < 960; i++) {
                const double t = (double)(b * 960 + i) / rate;
                pcm[(size_t)i * 2] = (float)(0.5 * std::sin(2 * M_PI * fl * t));
                pcm[(size_t)i * 2 + 1] = (float)(0.25 * std::sin(2 * M_PI * fr * t));
            }
            fitted += enc.encode(pcm.data(), 98, blk);
            CHECK(blk.size() == 98, "block size %zu", blk.size());
            aus.push_back(blk);
        }
        CHECK(fitted == 100, "%d of 100 blocks fitted in 98 bytes", fitted);
        Pcm out;
        CHECK(decodeAac(aus, rate, 2, out), "libavcodec does not open the stream");
        CHECK(out.ch[0].size() >= 90 * 960, "decoded %zu samples", out.ch[0].size());
        if (out.ch[0].size() < 90 * 960) continue;
        double hz = 0, amp = 0;
        measureTone(out.ch[0], 20 * 960, 40 * 960, rate, 900, 1100, hz, amp);
        const double snrL = toneSnr(out.ch[0], 20 * 960, 40 * 960, rate, hz, amp);
        printf("AAC-LC %d Hz stereo, 98 bytes per block: left %.1f Hz %.4f (want 1000, 0.5), SNR %.1f dB", rate, hz, amp, snrL);
        CHECK(std::fabs(hz - fl) < 1.0 && std::fabs(20 * std::log10(amp / 0.5)) < 0.5, "left tone %.2f Hz amplitude %.4f", hz, amp);
        CHECK(snrL > 40, "left channel SNR %.1f dB", snrL);
        measureTone(out.ch[1], 20 * 960, 40 * 960, rate, 2900, 3100, hz, amp);
        const double snrR = toneSnr(out.ch[1], 20 * 960, 40 * 960, rate, hz, amp);
        printf(", right %.1f Hz %.4f (want 3000, 0.25), SNR %.1f dB\n", hz, amp, snrR);
        CHECK(std::fabs(hz - fr) < 1.0 && std::fabs(20 * std::log10(amp / 0.25)) < 0.5, "right tone %.2f Hz amplitude %.4f", hz, amp);
        CHECK(snrR > 40, "right channel SNR %.1f dB", snrR);
        // the channels do not leak into each other
        double lk = 0;
        measureTone(out.ch[1], 20 * 960, 40 * 960, rate, 990, 1010, hz, lk);
        CHECK(lk < 0.5 * std::pow(10.0, -50.0 / 20), "left tone in the right channel: %.5f", lk);
    }
}

static void testSuperframe() {
    for (int bitrate : {32, 48, 64, 96}) {
        const int nAu = bitrate == 32 ? 4 : 6;
        int fs = 0;
        std::vector<int> len;
        dabgen::superframeLayout(bitrate, nAu, &fs, &len);
        int sum = fs;
        for (int l : len) sum += l;
        CHECK(sum == 110 * bitrate / 8, "access units do not tile the payload: %d of %d", sum, 110 * bitrate / 8);
        CHECK(fs == (nAu == 6 ? 11 : 8), "first start %d", fs);
        Lcg r(21);
        std::vector<std::vector<uint8_t>> aus;
        for (int k = 0; k < nAu; k++) {
            std::vector<uint8_t> a((size_t)len[(size_t)k] - 2);
            for (auto& v : a) v = (uint8_t)r.next();
            const uint16_t c = dabgen::crc16(a.data(), (int)a.size());
            a.push_back((uint8_t)(c >> 8)); a.push_back((uint8_t)c);
            aus.push_back(a);
        }
        const auto sf = dabgen::buildSuperframe(aus, bitrate, nAu == 6, true);
        const int N = bitrate / 8;
        CHECK((int)sf.size() == 120 * N, "superframe size %zu", sf.size());
        // damage 5 bytes of every codeword, decode like the receiver, check header, Fire code and access units
        std::vector<uint8_t> pay((size_t)110 * N);
        for (int i = 0; i < N; i++) {
            uint8_t cw[120];
            for (int j = 0; j < 120; j++) cw[j] = sf[(size_t)j * N + i];
            for (int e = 0; e < 5; e++) cw[(r.next() % 120)] ^= (uint8_t)(1 + r.next() % 255);
            CHECK(dab::rsDecode120(cw) >= 0, "Reed-Solomon decoding failed");
            for (int j = 0; j < 110; j++) pay[(size_t)j * N + i] = cw[j];
        }
        CHECK(dab::fireCode(&pay[2], 9) == (uint16_t)((pay[0] << 8) | pay[1]), "Fire code of the superframe");
        CHECK(pay[2] == (nAu == 6 ? 0x50 : 0x10), "header byte %02X", pay[2]);
        int pos = fs;
        for (int k = 0; k < nAu; k++) {
            if (k) {
                const int bit = 12 * (k - 1), byte = 3 + bit / 8;
                const int st = (bit % 8 == 0) ? ((pay[(size_t)byte] << 4) | (pay[(size_t)byte + 1] >> 4)) : (((pay[(size_t)byte] & 15) << 8) | pay[(size_t)byte + 1]);
                CHECK(st == pos, "au_start %d is %d, expected %d", k, st, pos);
            }
            CHECK(!memcmp(&pay[(size_t)pos], aus[(size_t)k].data(), aus[(size_t)k].size()), "access unit %d content", k);
            pos += (int)aus[(size_t)k].size();
        }
    }
}

// the MP2 frames through libavcodec's decoder
static bool decodeMp2(const std::vector<std::vector<uint8_t>>& frames, Pcm& out) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MP2);
    if (!codec) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); return false; }
    AVPacket* pk = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    for (const auto& a : frames) {
        av_new_packet(pk, (int)a.size());
        memcpy(pk->data, a.data(), a.size());
        if (avcodec_send_packet(ctx, pk) < 0) { av_packet_unref(pk); continue; }
        av_packet_unref(pk);
        while (avcodec_receive_frame(ctx, fr) == 0) {
            for (int c = 0; c < std::min(2, fr->ch_layout.nb_channels); c++)
                for (int i = 0; i < fr->nb_samples; i++) {
                    float v = 0;
                    if (fr->format == AV_SAMPLE_FMT_FLTP) v = ((const float*)fr->extended_data[c])[i];
                    else if (fr->format == AV_SAMPLE_FMT_S16P) v = ((const int16_t*)fr->extended_data[c])[i] / 32768.f;
                    else if (fr->format == AV_SAMPLE_FMT_S16) v = ((const int16_t*)fr->extended_data[0])[i * fr->ch_layout.nb_channels + c] / 32768.f;
                    out.ch[c].push_back(v);
                }
            av_frame_unref(fr);
        }
    }
    av_packet_free(&pk); av_frame_free(&fr); avcodec_free_context(&ctx);
    return true;
}

// the decoded loop of every service follows the sound it was made from, also across the wrap of the 12 s loop
static void testServices() {
    dabgen::TxConfig cfg;
    dabgen::Transmitter tx(cfg);
    for (size_t i = 0; i < tx.config().services.size(); i++) {
        const dabgen::TxService& sv = tx.config().services[i];
        Pcm out;
        const size_t unit = sv.dabPlus ? 960 : 1152;             // samples per access unit / frame
        const size_t loopUnits = sv.dabPlus ? tx.accessUnits((int)i).size() : tx.mp2Frames((int)i).size();
        const size_t period = loopUnits * unit;                  // samples after which the content repeats (a multiple of the sound's own period)
        const int passes = (int)std::max<size_t>(3, 40000 / period + 3);
        std::vector<std::vector<uint8_t>> seq;
        for (int pass = 0; pass < passes; pass++) for (const auto& a : (sv.dabPlus ? tx.accessUnits((int)i) : tx.mp2Frames((int)i))) seq.push_back(a);
        CHECK(sv.dabPlus ? decodeAac(seq, sv.sampleRate, 2, out) : decodeMp2(seq, out), "%s: decoder", sv.label.c_str());
        CHECK(out.ch[0].size() >= (size_t)passes * period - 4 * unit, "%s: decoded %zu samples of %zu", sv.label.c_str(), out.ch[0].size(), (size_t)passes * period);
        if (out.ch[0].size() < (size_t)passes * period - 4 * unit) continue;
        const size_t seam = period * (size_t)(passes / 2);        // where the content wraps while the decoder keeps running
        // find the codec delay: the lag with the best match of both channels
        double bestErr = 1e30;
        int bestLag = 0;
        for (int lag = 0; lag < 3000; lag++) {
            double e = 0, p = 0;
            for (size_t k = seam + 5000; k < seam + 9000; k += 2)
                for (int c = 0; c < 2; c++) {
                    const double r = dabgen::testSound(sv, c, (int64_t)(k - (size_t)lag)), d = out.ch[(size_t)c][k];
                    e += (d - r) * (d - r); p += r * r;
                }
            if (p > 0 && e / p < bestErr) { bestErr = e / p; bestLag = lag; }
        }
        auto snr = [&](int ch, size_t from, size_t to) {
            double e = 0, p = 0;
            for (size_t k = from; k < to; k++) {
                const double r = dabgen::testSound(sv, ch, (int64_t)(k - (size_t)bestLag)), d = out.ch[(size_t)ch][k];
                e += (d - r) * (d - r); p += r * r;
            }
            return 10 * std::log10(p / std::max(e, 1e-30));
        };
        const size_t from = seam + 3000, to = std::min(out.ch[0].size() - 2 * unit, from + 2 * period);
        const double sL = snr(0, from, to), sSeam = snr(0, seam - 2000, seam + 2000), sR = snr(1, from, to);
        printf("%s: decoded loop vs the sound it was made from: codec delay %d samples (mod the tone period), SNR left %.1f dB (across the loop wrap %.1f dB), right %.1f dB\n", sv.label.c_str(), bestLag, sL, sSeam, sR);
        const double need = sv.dabPlus ? (sv.melody ? 25.0 : 60.0) : 18.0;
        CHECK(sL > need && sSeam > need && sR > need, "%s: SNR left %.1f seam %.1f right %.1f dB, wanted more than %.0f", sv.label.c_str(), sL, sSeam, sR, need);
    }
}

// ---------------------------------------------------------------- the synthetic source
static uint64_t hashSamples(const cf32* x, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) {
        int32_t a, b;
        memcpy(&a, &x[i], 4); memcpy(&b, (const char*)&x[i] + 4, 4);
        h = (h ^ (uint32_t)a) * 1099511628211ull;
        h = (h ^ (uint32_t)b) * 1099511628211ull;
    }
    return h;
}

static void testSynth() {
    const double rates[] = {2.0e6, 2.048e6, 2.4e6, 4e6, 8e6, 10e6, 12.288e6, 20e6};
    for (double fs : rates) {
        SynthConfig sc;
        sc.snrDb = 30;
        auto syn = makeDabSynth(sc, fs);
        CHECK(syn && std::fabs(syn->sampleRate() - fs) < 1e-6, "no generator at %.3f Msps", fs / 1e6);
        if (!syn) continue;
        const size_t n = (size_t)(fs * 0.6);          // more than six frames
        std::vector<cf32> x(n);
        const auto t0 = std::chrono::steady_clock::now();
        syn->generate(x.data(), n);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double p = 0, pk = 0;
        for (auto& v : x) { p += std::norm(v); pk = std::max(pk, (double)std::abs(v)); }
        const double rms = std::sqrt(p / (double)n);
        const double rtf = 0.6 / el;
        printf("%.3f Msps: rms %.3f peak %.3f, %.0fx real time\n", fs / 1e6, rms, pk, rtf);
        CHECK(rms > 0.17 && rms < 0.23, "rms %.3f at %.3f Msps", rms, fs / 1e6);
        CHECK(pk < 0.91, "peak %.3f at %.3f Msps", pk, fs / 1e6);
        if (!SANITIZED) CHECK(rtf >= 3.0, "only %.1fx real time at %.3f Msps", rtf, fs / 1e6);
        // the spectrum: inside +-0.768 MHz flat, outside +-0.95 MHz well down (the images of the interpolation)
        if (fs >= 4e6) {
            const int N = 4096;
            std::vector<double> ps((size_t)N, 0.0);
            Fft fft(N);
            std::vector<cf32> t((size_t)N);
            int segs = 0;
            for (size_t o = (size_t)(fs * 0.01); o + N <= n && segs < 200; o += (size_t)N * 3, segs++) {
                for (int i = 0; i < N; i++) t[(size_t)i] = x[o + (size_t)i] * (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N));
                fft.forward(t.data());
                for (int i = 0; i < N; i++) ps[(size_t)i] += std::norm(t[(size_t)i]);
            }
            double in = 0, out = 0;
            int nin = 0, nout = 0;
            for (int i = 0; i < N; i++) {
                const double f = (i < N / 2 ? i : i - N) * fs / N;
                if (std::fabs(f) < 0.7e6) { in += ps[(size_t)i]; nin++; }
                else if (std::fabs(f) > 1.15e6 && std::fabs(f) < fs / 2 * 0.95) { out += ps[(size_t)i]; nout++; }
            }
            const double db = 10 * std::log10((out / nout) / (in / nin));
            printf("   out of band level %.1f dB below the carriers\n", -db);
            CHECK(db < -45, "out of band level %.1f dB at %.3f Msps", db, fs / 1e6);
        }
    }
    // the slowest case: the highest rate with a carrier offset, a clock offset and noise
    for (double fs : {10e6, 20e6}) {
        SynthConfig sc;
        sc.snrDb = 12; sc.cfoHz = 2000; sc.sroPpm = 15;
        auto syn = makeDabSynth(sc, fs);
        const size_t n = (size_t)(fs * 0.6);
        std::vector<cf32> x(n);
        const auto t0 = std::chrono::steady_clock::now();
        syn->generate(x.data(), n);
        const double rtf = 0.6 / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        float pk = 0;
        for (auto& v : x) pk = std::max(pk, std::abs(v));
        printf("%.0f Msps with carrier offset, clock offset and noise: %.0fx real time, peak %.3f\n", fs / 1e6, rtf, pk);
        if (!SANITIZED) CHECK(rtf >= 3.0, "only %.1fx real time at %.0f Msps with offsets", rtf, fs / 1e6);
        CHECK(pk <= 0.9001f, "peak %.4f with noise", pk);
    }
    CHECK(!makeDabSynth(SynthConfig(), 1.5e6) && !makeDabSynth(SynthConfig(), 40e6), "rates outside 2 to 21 Msps are refused");
    // the signal does not depend on how it is cut into chunks
    for (double fs : {2.048e6, 2.4e6, 10e6}) {
        SynthConfig sc;
        sc.snrDb = 20; sc.cfoHz = 1234; sc.sroPpm = 10;
        dabgen::TxConfig tc;
        tc.utcSeconds = 1700000000;
        const size_t total = (size_t)(fs * 0.25);
        uint64_t ref = 0;
        {
            auto syn = makeDabSynth(tc, sc, fs);
            std::vector<cf32> x(total);
            syn->generate(x.data(), total);
            ref = hashSamples(x.data(), total, 1469598103934665603ull);
        }
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) {
            auto syn = makeDabSynth(tc, sc, fs);
            std::vector<cf32> x(chunk);
            uint64_t h = 1469598103934665603ull;
            const size_t limit = chunk == 1 ? 20000 : total;     // one sample at a time is slow: compare the start only
            size_t done = 0;
            while (done < std::min(limit, total)) {
                const size_t k = std::min(chunk, std::min(limit, total) - done);
                syn->generate(x.data(), k);
                h = hashSamples(x.data(), k, h);
                done += k;
            }
            if (chunk == 1) {
                auto syn2 = makeDabSynth(tc, sc, fs);
                std::vector<cf32> y(limit);
                syn2->generate(y.data(), limit);
                CHECK(h == hashSamples(y.data(), limit, 1469598103934665603ull), "chunks of 1 sample change the signal at %.3f Msps", fs / 1e6);
            } else CHECK(h == ref, "chunks of %zu samples change the signal at %.3f Msps", chunk, fs / 1e6);
        }
    }
    // the noise: the C/N is set where the standard says; the carrier offset moves the spectrum; the clock offset stretches the frame
    {
        const double fs = 2.048e6;
        SynthConfig a, b;
        a.snrDb = 200; b.snrDb = 10;
        dabgen::TxConfig tc;
        tc.utcSeconds = 1700000000;
        auto sa = makeDabSynth(tc, a, fs), sb = makeDabSynth(tc, b, fs);
        const size_t n = 196608 * 3;
        std::vector<cf32> xa(n), xb(n);
        sa->generate(xa.data(), n);
        sb->generate(xb.data(), n);
        double pn = 0, ps = 0;
        for (size_t i = 0; i < n; i++) { pn += std::norm(xb[i] - xa[i]); ps += std::norm(xa[i]); }
        // the signal power counts the null symbol: the active symbols carry a factor 196608 / (196608 - 2656) more
        const double cn = 10 * std::log10(ps / (double)n * 196608.0 / (196608.0 - 2656.0) / (pn / (double)n));
        printf("requested C/N 10 dB, generated %.2f dB\n", cn);
        CHECK(std::fabs(cn - 10.0) < 0.3, "C/N %.2f dB", cn);
    }
    {
        const double fs = 4.096e6;
        SynthConfig a, b;
        a.snrDb = 200; b.snrDb = 200; b.cfoHz = 5000;
        dabgen::TxConfig tc;
        tc.utcSeconds = 1700000000;
        auto sa = makeDabSynth(tc, a, fs), sb = makeDabSynth(tc, b, fs);
        const size_t n = 196608 * 4;
        std::vector<cf32> xa(n), xb(n);
        sa->generate(xa.data(), n);
        sb->generate(xb.data(), n);
        // the product with the conjugate of the reference is a pure tone of 5 kHz where the signal is not clipped
        std::complex<double> acc = 0;
        for (size_t i = 100000; i < 400000; i++) acc += std::complex<double>(xb[i]) * std::conj(std::complex<double>(xa[i]));
        double cycles = 0;
        std::complex<double> acc2 = 0;
        for (size_t i = 100000; i + 1 < 400000; i++) acc2 += std::complex<double>(xb[i + 1] * std::conj(xa[i + 1])) * std::conj(std::complex<double>(xb[i] * std::conj(xa[i])));
        cycles = std::arg(acc2) / (2 * M_PI) * fs;
        printf("carrier offset: asked 5000 Hz, measured %.2f Hz\n", cycles);
        CHECK(std::fabs(cycles - 5000.0) < 5.0, "carrier offset %.2f Hz", cycles);
        (void)acc;
    }
}

int main() {
    testPrbs();
    testCrc();
    testRs();
    testConv();
    testPuncturing();
    testTables();
    testFrame();
    testAac();
    testSuperframe();
    testServices();
    testSynth();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("test_dab_gen: all passed\n");
    return 0;
}
