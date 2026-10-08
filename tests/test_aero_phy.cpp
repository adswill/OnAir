// Inmarsat Aero coding layer: known answers taken from JAERO's constants, and frame round trips at the bit level.
//   unique word 0xE15AE893 (JAERO aerol.cpp: preambledetector.setPreamble(3780831379LL,32), sent most significant bit first)
//   scrambler start (aerol.h AeroLScrambler: register {1,1,0,1,0,0,1,0,1,0,1,1,0,0,1}, v = s[0]^s[14]); the first 16 bits worked out by hand
//   convolutional code impulse response for polynomials 109, 79 (aerol.cpp SetCode(2,7,{109,79},..); libcorrect puts the newest bit in bit 0)
//   SU check: CRC-16/X-25 (aerol.h AeroLcrc16: 0x8408 reflected, start 0xFFFF, inverted), catalogue check value 0x906E for "123456789"
//   interleaver row permutation (aerol.cpp: interleaverowdepermute[i] = (i*27) % 64)
#include "dect2/aero_phy.h"
#include "dect2/aero_demod.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> randomInfo(std::mt19937& g, int rate) {
    // SUs of random bytes with a valid check, like the real channel carries
    std::vector<uint8_t> b(aeroFrameFormat(rate)->infoBytes());
    for (size_t k = 0; k + 12 <= b.size(); k += 12) {
        for (int i = 0; i < 10; i++) b[k + i] = (uint8_t)g();
        aeroSuSetCrc(&b[k]);
    }
    return b;
}

int main() {
    // unique word bits
    {
        const uint8_t want[32] = {1,1,1,0, 0,0,0,1, 0,1,0,1, 1,0,1,0, 1,1,1,0, 1,0,0,0, 1,0,0,1, 0,0,1,1};
        AeroFrameEncoder e(1200);
        std::vector<uint8_t> info(72, 0);
        const auto f = e.frame(info.data(), aeroHeader(1, 0, 0, 0));
        CHECK(f.size() == 1200, "1200 bit/s frame length %zu", f.size());
        CHECK(std::memcmp(f.data(), want, 32) == 0, "unique word bits");
        AeroFrameEncoder e2(10500);
        std::vector<uint8_t> info2(312, 0);
        const auto f2 = e2.frame(info2.data(), aeroHeader(1, 0, 0, 0));
        CHECK(f2.size() == 5250, "10500 bit/s frame length %zu", f2.size());
        bool ok = true;
        for (int i = 0; i < 32; i++) ok &= f2[2 * i] == want[i] && f2[2 * i + 1] == want[i];
        CHECK(ok, "10500: the word on both arms");
        // header follows the word, most significant bit first: format 1 -> 0001
        CHECK(f[32] == 0 && f[33] == 0 && f[34] == 0 && f[35] == 1, "header format id bits");
        CHECK(aeroFrameFormat(600)->totalBits() == 1200 && aeroFrameFormat(600)->blocks * aeroFrameFormat(600)->blockBits() == 1152, "600 format");
    }
    // scrambler
    {
        const uint8_t want[16] = {0,0,0,1, 0,0,1,1, 0,0,0,1, 1,0,1,1};
        const auto& s = AeroScrambler::sequence();
        CHECK(s.size() == 5000 && std::memcmp(s.data(), want, 16) == 0, "scrambler start");
        // the sequence obeys v[n] = v[n-1] ^ v[n-15] (taps 0 and 14 of the register)
        bool rec = true;
        for (int n = 15; n < 5000; n++) rec &= s[n] == (s[n - 1] ^ s[n - 15]);
        CHECK(rec, "scrambler recurrence");
        int ones = 0;
        for (int n = 0; n < 32767 && n < 5000; n++) ones += s[n];
        CHECK(ones > 2300 && ones < 2700, "scrambler balance %d", ones);
    }
    // convolutional code impulse response: pairs (bit k of 109, bit k of 79)
    {
        AeroConvEncoder enc;
        const uint8_t want[14] = {1,1, 0,1, 1,1, 1,1, 0,0, 1,0, 1,1};
        uint8_t got[14];
        for (int k = 0; k < 7; k++) enc.encode(k == 0, got[2 * k], got[2 * k + 1]);
        CHECK(std::memcmp(got, want, 14) == 0, "encoder impulse response");
    }
    // CRC
    {
        const uint8_t s[] = "123456789";
        CHECK(aeroCrc16(s, 9) == 0x906E, "CRC-16/X-25 check value %04X", aeroCrc16(s, 9));
        uint8_t su[12] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x32};
        aeroSuSetCrc(su);
        CHECK(aeroSuCrcOk(su), "SU check set / test");
        su[3] ^= 0x10;
        CHECK(!aeroSuCrcOk(su), "SU check catches a flipped bit");
    }
    // interleaver
    {
        for (int cols : {6, 9, 78}) {
            std::vector<uint8_t> c(64 * cols), t(64 * cols);
            for (size_t i = 0; i < c.size(); i++) c[i] = (uint8_t)(i * 7 + 3);
            aeroInterleave(c.data(), t.data(), cols);
            // row i of the sent block holds coded row permute[i]: with depermute[1] = 27, sent row 27 is coded row 1
            CHECK(t[27 * cols + 0] == c[1] && t[27 * cols + 1] == c[1 + 64], "row permutation, %d columns", cols);
            std::vector<float> tf(t.begin(), t.end()), back(c.size());
            aeroDeinterleave(tf.data(), back.data(), cols);
            bool ok = true;
            for (size_t i = 0; i < c.size(); i++) ok &= (uint8_t)back[i] == c[i];
            CHECK(ok, "deinterleave inverts interleave, %d columns", cols);
        }
    }
    // bit-level round trips with noise and a sign flip, through the framer
    std::mt19937 g(7);
    for (int rate : {600, 1200, 10500}) {
        for (double ebn0 : {30.0, 4.0}) {
            AeroFrameEncoder enc(rate);
            AeroFramer fr(rate);
            std::vector<std::vector<uint8_t>> sent, got;
            int susOk = 0, susBad = 0;
            fr.setCallback([&](AeroFrameEvent& e) { got.push_back(e.bytes); susOk += e.susOk; susBad += e.susBad; });
            std::normal_distribution<float> nd(0.f, (float)std::sqrt(1.0 / (2 * std::pow(10.0, ebn0 / 10))));
            for (int i = 0; i < 300; i++) fr.push(nd(g) > 0 ? 1.f : -1.f);       // noise before the first frame
            const int nFrames = rate == 10500 ? 8 : 12;
            for (int f = 0; f < nFrames; f++) {
                const auto info = randomInfo(g, rate);
                sent.push_back(info);
                const auto bits = enc.frame(info.data(), aeroHeader(1, 0, f >> 4, f & 15));
                for (size_t i = 0; i < bits.size(); i++) {
                    float v = bits[i] ? 1.f : -1.f;
                    if (rate == 10500 && (i & 1)) v = -v;          // one arm turned over (phase ambiguity)
                    if (rate != 10500) v = -v;                     // MSK: the whole stream turned over
                    fr.push(v + nd(g));
                }
            }
            for (int i = 0; i < 100; i++) fr.push(nd(g));
            size_t match = 0;
            for (size_t i = 0; i < got.size() && i < sent.size(); i++) match += got[i] == sent[i];
            if (ebn0 > 20) {
                CHECK(got.size() == sent.size() && match == sent.size(), "rate %d clean: %zu of %zu frames, %zu exact", rate, got.size(), sent.size(), match);
            } else {
                printf("rate %d, Eb/N0 %.0f dB on the channel bits: %zu of %zu frames, %zu exact, SUs %d good %d bad\n", rate, ebn0, got.size(), sent.size(), match, susOk, susBad);
                CHECK(got.size() == sent.size() && susBad * 20 < susOk, "rate %d at %.0f dB", rate, ebn0);
            }
        }
    }
    // byte packing as JAERO packs it (aerol.cpp: ch |= bit * 128, then ch >>= 1): the first decoded bit lands in bit 0
    {
        const uint8_t bits[16] = {1,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,1};
        uint8_t by[2];
        aeroBitsToBytes(bits, 2, by);
        CHECK(by[0] == 0x01 && by[1] == 0x80, "first bit in bit 0: %02X %02X", by[0], by[1]);
        uint8_t back[16];
        aeroBytesToBits(by, 2, back);
        CHECK(std::memcmp(back, bits, 16) == 0, "bytes to bits");
    }
    // the blocks of a frame are one code: errors at the end of a block are corrected with the next block's help
    for (int rate : {600, 1200}) {
        std::mt19937 g(77);
        const AeroFrameFormat* f = aeroFrameFormat(rate);
        const auto info = randomInfo(g, rate);
        AeroFrameEncoder enc(rate);
        const auto bits = enc.frame(info.data(), aeroHeader(1, 0, 0, 0));
        const int bb = f->blockBits();
        std::vector<float> rx(f->codedBits);
        for (int i = 0; i < f->codedBits; i++) rx[i] = bits[f->uwBits + f->headerBits + f->skipBits + i] ? 1.f : -1.f;
        // rx index of every deinterleaved position of block 0
        std::vector<float> idx(bb), pos(bb);
        for (int i = 0; i < bb; i++) idx[i] = (float)i;
        aeroDeinterleave(idx.data(), pos.data(), f->cols);
        for (int k = bb - 14; k < bb - 6; k += 2) rx[(size_t)pos[(size_t)k]] = -rx[(size_t)pos[(size_t)k]];     // four wrong channel bits just before the block's end
        AeroFrameDecoder dec(rate);
        AeroDecodedFrame out;
        dec.decode(rx.data(), aeroHeader(1, 0, 0, 0), out);
        CHECK(out.bytes == info && out.susBad == 0, "rate %d: errors at a block end not corrected (%d SUs bad)", rate, out.susBad);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
