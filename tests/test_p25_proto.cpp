// P25 Phase 1 protocol: the NID BCH, Reed-Solomon, Hamming and Golay codes with errors, the trellis code, TSBK CRC, link control and
// encryption sync fields, the IMBE interleave.
#include "dect2/p25_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main() {
    // frame sync: the levels are the 48-bit word 0x5575F5FF77FF read a dibit at a time (the generator shares the table, so only this catches a wrong one)
    for (int k = 0; k < p25::kSyncSym; k++) {
        const int dd = (int)((0x5575F5FF77FFULL >> (46 - 2 * k)) & 3);
        CHECK(p25::kSync[k] == p25::dibitToSym(dd), "sync symbol %d", k);
    }
    // NID: up to 10 bit errors corrected
    for (int t = 0; t < 200; t++) {
        const int nac = (int)(rnd() & 0xFFF), duid = (int)(rnd() & 15);
        uint8_t b[64];
        p25::nidEncode(nac, duid, b);
        const int ne = t % 11;
        for (int e = 0; e < ne; e++) b[rnd() % 63] ^= 1;
        int n2 = -1, d2 = -1;
        CHECK(p25::nidDecode(b, n2, d2) && n2 == nac && d2 == duid, "nid %d errors", ne);
    }
    // RS: (24,12), (24,16), (36,20) correct up to (n-k)/2 symbol errors
    const int rs[3][2] = {{24, 12}, {24, 16}, {36, 20}};
    for (const auto& c : rs) {
        const int n = c[0], k = c[1], tt = (n - k) / 2;
        for (int t = 0; t < 100; t++) {
            uint8_t s[36], o[36];
            for (int i = 0; i < k; i++) s[i] = (uint8_t)(rnd() & 63);
            p25::rsEncode(s, n, k);
            memcpy(o, s, sizeof s);
            const int ne = t % (tt + 1);
            for (int e = 0; e < ne; e++) s[rnd() % n] ^= (uint8_t)(1 + rnd() % 63);
            int errs = 0;
            CHECK(p25::rsDecode(s, n, k, &errs) && !memcmp(s, o, (size_t)n), "rs(%d,%d) %d errors", n, k, ne);
        }
    }
    // Hamming(10,6) one error; Golay(24,12) three errors, minimum distance 8; Golay(18,6)
    for (int h = 0; h < 64; h++) for (int b = 0; b < 10; b++) CHECK(p25::hamming10Decode((uint16_t)(p25::hamming10Encode(h) ^ (1 << b))) == h, "hamming %d bit %d", h, b);
    int dmin = 99;
    for (int d = 1; d < 4096; d++) dmin = std::min(dmin, __builtin_popcount(p25::golay24Encode(d)));
    CHECK(dmin == 8, "golay distance %d", dmin);
    for (int t = 0; t < 300; t++) {
        const int d = (int)(rnd() & 0xFFF);
        uint32_t c = p25::golay24Encode(d);
        for (int e = 0; e < t % 4; e++) c ^= 1u << (rnd() % 24);
        CHECK(p25::golay24Decode(c) == d, "golay24 %d errors", t % 4);
        const int h = d & 63;
        uint32_t c18 = p25::golay18Encode(h);
        for (int e = 0; e < t % 4; e++) c18 ^= 1u << (rnd() % 18);
        CHECK(p25::golay18Decode(c18) == h, "golay18");
    }
    // TSBK through the trellis code, with level noise and a few wrong symbols
    for (int t = 0; t < 100; t++) {
        p25::Tsbk a; a.opcode = (int)(rnd() & 63); a.args = ((uint64_t)rnd() << 40) ^ ((uint64_t)rnd() << 16) ^ rnd(); a.last = t & 1;
        uint8_t b[96], d[98], r[96];
        p25::tsbkBits(a, b);
        p25::trellisEncode(b, d);
        float lv[98];
        for (int i = 0; i < 98; i++) lv[i] = (float)p25::dibitToSym(d[i]) + ((int)(rnd() % 100) - 50) / 100.f;
        { const int i = (int)(rnd() % 98); lv[i] += lv[i] > 0 ? -2.f : 2.f; }   // one symbol a level off
        p25::Tsbk o;
        CHECK(p25::trellisDecode(lv, r) && p25::tsbkParse(r, o) && o.opcode == a.opcode && o.args == a.args && o.last == a.last, "tsbk %d", t);
        r[10] ^= 1;
        CHECK(!p25::tsbkParse(r, o), "tsbk crc misses an error");
    }
    // link control and encryption sync fields
    {
        p25::Lc l; l.lco = p25::kLcGroup; l.svcOpts = 0xC4; l.tg = 1201; l.src = 1234567;
        uint8_t h[12]; p25::Lc r;
        p25::lcHexbits(l, h);
        CHECK(p25::lcParse(h, r) && r.lco == 0 && r.tg == 1201 && r.src == 1234567 && r.svcOpts == 0xC4, "group lc");
        l.lco = p25::kLcUnit; l.dst = 76543;
        p25::lcHexbits(l, h);
        CHECK(p25::lcParse(h, r) && r.lco == 3 && r.dst == 76543 && r.src == 1234567, "unit lc");
        p25::Crypto c; c.algId = 0x84; c.keyId = 0x1234; c.tg = 999; c.mi[0] = 0xAB;
        uint8_t e[16], hd[20]; p25::Crypto o, o2;
        p25::esHexbits(c, e); p25::esParse(e, o);
        CHECK(o.algId == 0x84 && o.keyId == 0x1234 && o.mi[0] == 0xAB, "es");
        p25::hduHexbits(c, hd); p25::hduParse(hd, o2);
        CHECK(o2.algId == 0x84 && o2.keyId == 0x1234 && o2.tg == 999, "hdu");
    }
    // IMBE interleave round trip
    {
        uint8_t bits[144], d[72], back[144];
        for (int i = 0; i < 144; i++) bits[i] = (uint8_t)(rnd() & 1);
        char fr[8][23], f2[8][23];
        p25::imbeUnflatten(bits, fr);
        p25::imbeInterleave(fr, d);
        p25::imbeDeinterleave(d, f2);
        p25::imbeFlatten(f2, back);
        CHECK(!memcmp(bits, back, 144), "imbe interleave");
    }
    printf(fails ? "p25 proto: %d FAILED\n" : "p25 proto: all passed\n", fails);
    return fails ? 1 : 0;
}
