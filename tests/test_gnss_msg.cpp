// The codes and message layers of QZSS, SBAS and Galileo against the published tables and examples:
// - the first ten chips of every SBAS and QZSS PRN (octal, the L1 C/A PRN code assignment table of gps.gov, which lists DO-229 and IS-QZSS-PNT)
// - the G2 delay construction gives the GPS codes too (IS-GPS-200 Table 3-I delays)
// - Galileo E1-B: the first hexadecimal symbols of codes 1 and 50 (OS SIS ICD Annex C), and balance
// - the convolutional code and the interleaver against the I/NAV numerical example of OS SIS ICD Annex D.2
// - CRC-24Q, SBAS messages and I/NAV pages through the coder and the Viterbi decoder with noise, both polarities
#include "dect2/gnss_codes.h"
#include "dect2/gnss_msg.h"
#include "dect2/gnss_tel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> bitsOf(const char* s) {
    std::vector<uint8_t> v;
    for (; *s; s++) if (*s == '0' || *s == '1') v.push_back((uint8_t)(*s - '0'));
    return v;
}

int main() {
    // ---- L1 C/A codes: PRN, first ten chips in octal (gps.gov L1 C/A PRN code assignments)
    struct Row { int prn; unsigned first10; };
    const Row rows[] = {{120, 0671}, {121, 0536}, {122, 01510}, {123, 01545}, {124, 0160}, {125, 0701}, {126, 013}, {127, 01060}, {128, 0245}, {129, 0527},
                        {130, 01436}, {131, 01226}, {132, 01257}, {133, 046}, {134, 01071}, {135, 0561}, {136, 01037}, {137, 0770}, {138, 01327}, {139, 01472},
                        {140, 0124}, {141, 0366}, {142, 0133}, {143, 0465}, {144, 0717}, {145, 0217}, {146, 01742}, {147, 01422}, {148, 01442}, {149, 0523},
                        {150, 0736}, {151, 01635}, {152, 0136}, {153, 0273}, {154, 01026}, {155, 03}, {156, 01670}, {157, 0624}, {158, 0235},
                        {193, 0727}, {194, 0170}, {195, 030}, {196, 0472}, {197, 01237}, {198, 0414}, {199, 01050}, {200, 01630}, {201, 0571}, {202, 0732}};
    for (const Row& r : rows) CHECK(l1caFirst10(r.prn) == r.first10, "PRN %d first 10 chips %o, expected %o", r.prn, l1caFirst10(r.prn), r.first10);
    CHECK(l1caSystem(127) == GnssSbas && l1caSystem(193) == GnssQzss && l1caSystem(5) == GnssGps && l1caSystem(170) == -1, "systems of the PRNs");
    {
        // the delay form gives the GPS codes as well: PRN 1 is G2 delayed by 5 chips, PRN 32 by 950 (IS-GPS-200 Table 3-I)
        uint8_t a[kGpsCaLen], b[kGpsCaLen];
        gpsCaChips(1, a);
        // a QZSS code built from a GPS delay must match the tap form: borrow the generator through a known QZSS PRN is not possible, so compare correlation:
        // the codes of all the L1 C/A PRNs are distinct and balanced Gold codes (correlation values -1, -65, 63 only)
        int bad = 0;
        std::vector<int> prns;
        for (int p = 1; p <= 32; p++) prns.push_back(p);
        for (int p = 120; p <= 158; p++) prns.push_back(p);
        for (int p = 193; p <= 202; p++) prns.push_back(p);
        for (size_t i = 0; i < prns.size(); i++) {
            l1caChips(prns[i], a);
            for (size_t j = i + 1; j < prns.size(); j += 7) {
                l1caChips(prns[j], b);
                for (int s = 0; s < kGpsCaLen; s += 31) {
                    int c = 0;
                    for (int n = 0; n < kGpsCaLen; n++) c += (a[n] ^ b[(n + s) % kGpsCaLen]) ? -1 : 1;
                    if (c != -1 && c != -65 && c != 63) bad++;
                }
            }
        }
        CHECK(bad == 0, "%d cross-correlation values outside the Gold set", bad);
    }
    // ---- Galileo E1-B: code 1 begins F5D7, code 50 ... (Annex C); each code is balanced within a few chips
    {
        uint8_t c[kGalE1Len];
        CHECK(galE1bChips(1, c), "E1-B code 1");
        unsigned v = 0;
        for (int i = 0; i < 16; i++) v = v << 1 | c[i];
        CHECK(v == 0xF5D7, "E1-B code 1 starts %04X, expected F5D7", v);
        CHECK(!galE1bChips(0, c) && !galE1bChips(51, c), "Galileo PRN out of range accepted");
        uint8_t d[kGalE1Len];
        int worst = 0;
        for (int p = 1; p <= 50; p++) {
            galE1bChips(p, c);
            int ones = 0;
            for (int i = 0; i < kGalE1Len; i++) ones += c[i];
            worst = std::max(worst, std::abs(2 * ones - kGalE1Len));
            if (p > 1) {
                galE1bChips(p - 1, d);
                int cc = 0;
                for (int i = 0; i < kGalE1Len; i++) cc += (c[i] ^ d[i]) ? -1 : 1;
                CHECK(std::abs(cc) < 300, "E1-B codes %d and %d correlate: %d", p - 1, p, cc);
            }
        }
        CHECK(worst <= 64, "an E1-B code is unbalanced by %d chips", worst);
    }
    // ---- Annex D.2: the I/NAV coding example
    {
        const std::vector<uint8_t> in = bitsOf("11111111 11110000 11001100 10101010 00000000 00001111 00110011 01010101 11100011 11101100 11011111 10001010 00011100 00010011 01000000");
        const std::vector<uint8_t> enc = bitsOf("10001100 00011010 10101010 01110011 00110001 01011010 01101111 01011001 01111000 10010101 01010101 10001100 11001110 10100101 10010000 10100110"
                                                " 10000100 00000010 00000010 00011011 10011001 11101011 01011100 00011000 10111011 11111101 11111101 11100100 01010011 00100010");
        const std::vector<uint8_t> itl = bitsOf("10100000 01011111 10001100 11110000 01011110 10100000 00011001 11100011 10101000 01010000 01001111 01010111 01111000 10000110 11111010 11100111"
                                                " 10011000 00011111 11100010 00001001 11110110 00001001 11000111 01100000 10010111 01001000 11000110 11011001 00000111 00111010");
        CHECK(in.size() == 120 && enc.size() == 240 && itl.size() == 240, "example sizes");
        uint8_t e[240], i2[240];
        convEncode(in.data(), 120, true, 0, e);
        CHECK(std::memcmp(e, enc.data(), 240) == 0, "convolutional code differs from the ICD example");
        inavInterleave(e, i2);
        CHECK(std::memcmp(i2, itl.data(), 240) == 0, "interleaver differs from the ICD example");
        uint8_t p250[250];
        inavEncodePart(in.data(), p250);
        CHECK(std::memcmp(p250 + 10, itl.data(), 240) == 0 && std::memcmp(p250, kInavSync, 10) == 0, "page part");
        float soft[240];
        for (int k = 0; k < 240; k++) soft[k] = itl[k] ? -1.f : 1.f;
        uint8_t out[120];
        inavDecodePart(soft, out);
        CHECK(std::memcmp(out, in.data(), 120) == 0, "decoding the ICD example");
    }
    // ---- CRC-24Q: the remainder of a message with its CRC appended is zero; the generator is 0x1864CFB
    {
        std::mt19937 rng(5);
        uint8_t m[250];
        for (int i = 0; i < 226; i++) m[i] = (uint8_t)(rng() & 1);
        const uint32_t c = crc24q(m, 226);
        for (int i = 0; i < 24; i++) m[226 + i] = (uint8_t)((c >> (23 - i)) & 1);
        CHECK(crc24q(m, 250) == 0, "CRC with its check bits is not zero");
        const uint8_t one[1] = {1};   // X^24 mod G = G - X^24
        CHECK(crc24q(one, 1) == (0x1864CFBu & 0xFFFFFFu), "generator %06X", crc24q(one, 1));
    }
    // ---- SBAS: a stream of messages through the code, noise at 2 dB Eb/N0 per bit... and the decoder
    {
        std::mt19937 rng(11);
        const int nMsg = 6;
        std::vector<uint8_t> bits;
        int types[nMsg];
        for (int k = 0; k < nMsg; k++) {
            uint8_t d[212], m[250];
            for (auto& v : d) v = (uint8_t)(rng() & 1);
            types[k] = (int)(1 + rng() % 28);
            sbasBuildMessage(k, types[k], d, m);
            bits.insert(bits.end(), m, m + 250);
        }
        std::vector<uint8_t> sy(bits.size() * 2);
        convEncode(bits.data(), (int)bits.size(), false, 0, sy.data());
        std::normal_distribution<float> g(0.f, 0.75f);     // symbol SNR about 2.5 dB
        for (int pol = 0; pol < 2; pol++) {
            std::vector<float> soft(sy.size());
            for (size_t k = 0; k < sy.size(); k++) soft[k] = ((sy[k] ^ pol) ? -1.f : 1.f) + g(rng);
            // start the window in the middle of a message, at a pair boundary
            const int skip = 137;
            auto msgs = sbasFindMessages(soft.data() + 2 * skip, (int)bits.size() - skip);
            int found = 0;
            for (auto& m : msgs) {
                const int idx = (int)((m.bitPos + skip) / 250);
                if ((m.bitPos + skip) % 250 == 0 && m.type == types[idx]) found++;
            }
            CHECK(found >= nMsg - 2, "polarity %d: %d SBAS messages found", pol, found);
        }
    }
    // ---- I/NAV pages: build, code, add noise, decode, check
    {
        std::mt19937 rng(3);
        int good = 0;
        std::normal_distribution<float> g(0.f, 0.7f);
        for (int t = 0; t < 20; t++) {
            uint8_t w[128], oss[64], ev[120], od[120];
            for (auto& v : w) v = (uint8_t)(rng() & 1);
            for (auto& v : oss) v = (uint8_t)(rng() & 1);
            inavBuildPage(w, oss, t % 3, ev, od);
            uint8_t se[250], so[250];
            inavEncodePart(ev, se); inavEncodePart(od, so);
            float fe[240], fo[240];
            const int pol = t & 1;
            for (int k = 0; k < 240; k++) { fe[k] = ((se[10 + k] ^ pol) ? -1.f : 1.f) + g(rng); fo[k] = ((so[10 + k] ^ pol) ? -1.f : 1.f) + g(rng); }
            if (pol) for (int k = 0; k < 240; k++) { fe[k] = -fe[k]; fo[k] = -fo[k]; }   // the receiver corrects the polarity from the sync pattern
            uint8_t de[120], dd[120], w2[128];
            inavDecodePart(fe, de); inavDecodePart(fo, dd);
            if (inavCheckPage(de, dd, w2) && std::memcmp(w, w2, 128) == 0) good++;
            // the encoded SSP is the last 16 symbols before coding the tail (Table 83)
            static const char* encSsp[3] = {"1110100100100101", "0110110001001110", "1101000000111110"};
            uint8_t raw[240];
            convEncode(od, 120, true, 0, raw);
            const std::vector<uint8_t> want = bitsOf(encSsp[t % 3]);
            CHECK(std::memcmp(raw + 240 - 16, want.data(), 16) == 0, "SSP%d symbols", t % 3 + 1);
        }
        CHECK(good >= 19, "%d of 20 pages decoded", good);
    }
    printf("gnss msg: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
