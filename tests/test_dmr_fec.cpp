// DMR codes against known answers: the worked idle message of TS 102 361-1 annex D, the generator matrices of annex B, and values taken from
// bursts that were captured off the air (published as test vectors by the ok-dmrlib project, https://github.com/OK-DMR/ok-dmrlib).
#include "dect2/dmr_fec.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2::dmr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) v.push_back((uint8_t)strtoul(std::string(s + i, 2).c_str(), nullptr, 16));
    return v;
}
static Bits bitsOfString(const char* s) {
    Bits b;
    for (; *s; s++) if (*s == '0' || *s == '1') b.push_back((uint8_t)(*s - '0'));
    return b;
}
static Bits bitsOfHex(const char* s) {
    const auto v = hex(s);
    Bits b;
    bytesToBits(v.data(), v.size(), b);
    return b;
}

static int minDistance(unsigned k, unsigned (*par)(unsigned)) {
    int best = 99;
    for (unsigned i = 1; i < (1u << k); i++) {
        const int w = popcount32(i) + popcount32(par(i));
        if (w < best) best = w;
    }
    return best;
}

static void testShortCodes() {
    // the specification calls it (20,8,7); the matrix of table B.11 gives distance 8 (the shortened code of distance 7 plus one parity bit)
    CHECK(minDistance(8, golay2008Parity) >= 7, "Golay (20,8) minimum distance %d (the specification says 7)", minDistance(8, golay2008Parity));
    CHECK(minDistance(7, qr1676Parity) == 6, "QR (16,7,6) minimum distance %d", minDistance(7, qr1676Parity));
    CHECK(minDistance(4, hamming743Parity) == 3, "Hamming (7,4,3) minimum distance");

    // slot type of a captured control burst: colour code 5, data type CSBK (3), parity 0xF2B (the slot type 0101 0011 1111 0010 1011)
    CHECK(golay2008Parity((5 << 4) | 3) == 0xF2B, "Golay parity of CC5/CSBK is %03X, captured 0F2B", golay2008Parity((5 << 4) | 3));
    // every error pattern of up to three bits is corrected
    int bad = 0;
    for (unsigned info = 0; info < 256; info += 37) {
        const unsigned w = (info << 12) | golay2008Parity(info);
        for (int a = 0; a < 20; a++)
            for (int b = a; b < 20; b++)
                for (int c = b; c < 20; c++) {
                    unsigned x = w ^ (1u << a);
                    if (b != a) x ^= 1u << b;
                    if (c != b) x ^= 1u << c;
                    unsigned out = 0;
                    const int e = golay2008Decode(x, out);
                    if (e < 0 || out != info) bad++;
                }
    }
    CHECK(bad == 0, "Golay: %d patterns of up to 3 errors not corrected", bad);
    {   // a word four errors away from every codeword is refused, or decoded to something at distance 3 or more: never reported as clean
        std::mt19937 rng(5);
        int clean = 0;
        for (int i = 0; i < 2000; i++) {
            unsigned out = 0;
            const unsigned w = rng() & 0xFFFFF;
            if (golay2008Decode(w, out) == 0) clean++;
        }
        CHECK(clean < 10, "Golay: %d of 2000 random words reported as valid (expected about 2000/4096)", clean);
    }

    // EMB parities of a captured call: colour code 1, PI 0, LCSS 1, 3, 3, 2, 0 -> the 7 bit word is CC(4) PI(1) LCSS(2)
    const unsigned lcss[5] = {1, 3, 3, 2, 0}, want[5] = {0x191, 0x174, 0x174, 0x107, 0x1E2};
    for (int i = 0; i < 5; i++) {
        const unsigned info7 = (1u << 3) | lcss[i];
        CHECK(qr1676Parity(info7) == want[i], "QR parity for LCSS %u is %03X, captured %03X", lcss[i], qr1676Parity(info7), want[i]);
        // up to two errors anywhere in the 16 bits are corrected
        const unsigned w = (info7 << 9) | want[i];
        int badQr = 0;
        for (int a = 0; a < 16; a++)
            for (int b = a; b < 16; b++) {
                unsigned x = w ^ (1u << a), out = 0;
                if (b != a) x ^= 1u << b;
                if (qr1676Decode(x, out) < 0 || out != info7) badQr++;
            }
        CHECK(badQr == 0, "QR: %d patterns of up to 2 errors not corrected", badQr);
    }
    // TACT: Hamming (7,4)
    for (unsigned info = 0; info < 16; info++) {
        const unsigned w = (info << 3) | hamming743Parity(info);
        for (int a = 0; a < 7; a++) {
            unsigned out = 0;
            CHECK(hamming743Decode(w ^ (1u << a), out) == 1 && out == info, "Hamming (7,4) single error at %d of word %u", a, info);
        }
    }
}

static void testHamming() {
    const HammingKind kinds[4] = {kHam1393, kHam15113, kHam16114, kHam17123};
    for (HammingKind k : kinds) {
        const int ki = hammingInfoBits(k), p = hammingParityBits(k);
        int bad = 0, detect = 0, dbl = 0;
        for (unsigned info = 0; info < (1u << ki); info += (ki > 9 ? 13 : 1)) {
            const unsigned w = (info << p) | hammingParity(k, info);
            unsigned x = w;
            if (hammingCorrect(k, x) != 0 || x != w) bad++;
            for (int a = 0; a < ki + p; a++) {
                x = w ^ (1u << a);
                if (hammingCorrect(k, x) != 1 || x != w) bad++;
                // two errors: the (16,11,4) code must notice them; the others may mistake them for one
                for (int b = a + 1; b < ki + p; b++) {
                    unsigned y = w ^ (1u << a) ^ (1u << b);
                    const int e = hammingCorrect(k, y);
                    if (k == kHam16114) { if (e != -1) dbl++; }
                    else if (e == 0) dbl++;      // never a silent pass
                }
            }
        }
        (void)detect;
        CHECK(bad == 0, "Hamming kind %d: %d single error faults", (int)k, bad);
        CHECK(dbl == 0, "Hamming kind %d: %d double errors not noticed", (int)k, dbl);
    }
}

static void testBptc() {
    // annex D: the idle message. 96 pseudo random information bits (table D.2) give this transmit sequence; the sequence was read off tables
    // D.3 (encoded bits) and E.1 (position of every bit in the burst), which are printed independently of the interleaving formula
    const Bits info = bitsOfHex("FF83DF1732094ED1E7CD8A91");
    const char* expect =
        "0101001111000010010111101010101110101000011001110001110111000111001110000011101111011001001101100011111101101110010001100101000101"
        "110001101101001000110010100110110101001111110001100001000010110100";
    Bits tx;
    bptc196Encode(info, tx);
    CHECK(tx.size() == 196, "BPTC output length %zu", tx.size());
    const Bits want = bitsOfString(expect);
    CHECK(tx == want, "BPTC encoding of the annex D idle message differs from the specification");
    Bits back;
    CHECK(bptc196Decode(tx, back) == 0 && back == info, "BPTC clean decode");

    // the burst payload of a captured idle message in the MMDVM format (first 12 bytes of its 33 byte frame): 53 C2 5E AB A8 67 1D C7 38 3B D9 36
    {
        const Bits b = bitsOfHex("53C25EABA8671DC7383BD936");
        for (int i = 0; i < 96; i++) CHECK(b[i] == tx[i], "idle burst bit %d differs from the published idle frame", i);
    }

    // random errors: a few single errors per matrix are corrected, a heavy burst is refused
    std::mt19937 rng(11);
    int fixed = 0, refused = 0, wrong = 0;
    for (int trial = 0; trial < 400; trial++) {
        Bits d(96);
        for (auto& x : d) x = rng() & 1;
        Bits t;
        bptc196Encode(d, t);
        Bits e = t;
        const int n = 1 + (int)(rng() % 3);
        for (int i = 0; i < n; i++) e[rng() % 196] ^= 1;
        Bits out;
        const int r = bptc196Decode(e, out);
        if (r >= 0 && out == d) fixed++;
        else if (r < 0) refused++;
        else wrong++;
    }
    CHECK(fixed >= 380, "BPTC corrected only %d of 400 patterns with up to 3 errors", fixed);
    CHECK(wrong < 10, "BPTC mis-corrected %d of 400 patterns", wrong);
    (void)refused;
    Bits d(96), t, out;
    for (auto& x : d) x = rng() & 1;
    bptc196Encode(d, t);
    for (int i = 20; i < 60; i++) t[i] ^= 1;
    const int r = bptc196Decode(t, out);
    CHECK(!(r >= 0 && out == d), "BPTC claims to have repaired a 40 bit burst error");
}

static void testReedSolomon() {
    // link control with Reed-Solomon parity, from captured headers and terminators (parity masked with 969696 / 999999)
    const struct { const char* w; uint32_t mask; } v[] = {{"0300002635a903d475cb8795", 0x969696}, {"03000003d4752635a96fed09", 0x969696},
                                                          {"03000003d4752635a960e206", 0x999999}, {"0300002635a903d475c4889a", 0x999999}};
    for (auto& t : v) {
        auto w = hex(t.w);
        uint8_t par[3];
        rs129Parity(w.data(), par);
        CHECK(par[0] == (w[9] ^ (t.mask >> 16 & 0xFF)) && par[1] == (w[10] ^ (t.mask >> 8 & 0xFF)) && par[2] == (w[11] ^ (t.mask & 0xFF)),
              "Reed-Solomon parity of %s", t.w);
        // table B.18: the parity of a single one in position i
        w[9] ^= (uint8_t)(t.mask >> 16); w[10] ^= (uint8_t)(t.mask >> 8); w[11] ^= (uint8_t)t.mask;
        CHECK(rs129Correct(w.data()) == 0, "clean word reported as damaged");
        for (int pos = 0; pos < 12; pos++) {
            auto x = w;
            x[pos] ^= (uint8_t)(0x31 + pos);
            const int e = rs129Correct(x.data());
            CHECK(e == 1 && x == w, "single symbol error at %d not corrected", pos);
        }
        // two wrong symbols can be seen as one other error, but are never reported as a clean word
        auto x = w;
        x[1] ^= 0x55; x[7] ^= 0x0F;
        CHECK(rs129Correct(x.data()) != 0, "two wrong symbols reported as a clean word");
    }
    // generator matrix, table B.18
    const uint8_t rows[9][3] = {{0x1C, 0xBC, 0xFD}, {0x89, 0x31, 0x08}, {0xAD, 0x41, 0x36}, {0x7D, 0x71, 0x16}, {0xF3, 0xA6, 0x3A},
                                {0x08, 0x83, 0x7B}, {0x3F, 0x6F, 0x02}, {0x6C, 0x0D, 0xA7}, {0x0E, 0x38, 0x40}};
    for (int i = 0; i < 9; i++) {
        uint8_t m[9] = {}, p[3];
        m[i] = 1;
        rs129Parity(m, p);
        CHECK(!memcmp(p, rows[i], 3), "Reed-Solomon generator matrix row %d", i);
    }
}

static void testCrc() {
    // data headers captured off the air: CRC-CCITT of the first ten octets, inverted, then the data header mask CCCC
    const char* headers[] = {"8DA300000100000101002B97", "8DA30000010008350100B731", "4DA123386323383B05005757", "023A2337FC2337FE820081A3",
                             "434E2337FE2337FC84781BD1", "01402337FC2337FE000FF83A", "800500010627FCE7001BACAF"};
    for (const char* h : headers) {
        const auto b = hex(h);
        const uint16_t crc = (uint16_t)(crcCcitt(b.data(), 10) ^ kMaskDataHeader);
        CHECK(crc == (uint16_t)((b[10] << 8) | b[11]), "header CRC of %s: calculated %04X", h, crc);
    }
    // a captured CSBK (mask A5A5) and a PI header (mask 6969), both after BPTC decoding
    {
        const auto b = hex("BD00801D23386323383B5889");
        CHECK((uint16_t)(crcCcitt(b.data(), 10) ^ kMaskCsbk) == 0x5889, "CSBK CRC");
        const auto p = hex("211003D537D57A0000092B13");
        CHECK((uint16_t)(crcCcitt(p.data(), 10) ^ kMaskPi) == 0x2B13, "PI header CRC");
    }
    // message CRC-32: captured data messages (octet pairs swapped, the four CRC octets sent least significant first)
    const struct { const char* data; const char* crc; } m[] = {
        {"d6790062620003bf000700000000000000000000", "210b9a3d"},
        {"45000038fb410000401125490c23380b0d0008fd0fa10fa10024276a0d1a22047fffffff694728710f0a522c2d82534e6c0048564770402b", "82616528"},
        {"0600fb4f3d3f82afc6d80b42ce88668afc7d8b1807e83c308d95bb8be5dd59e95b2837e795af87005ae2a743535ca421601d", "c76ae25c"}};
    for (auto& t : m) {
        const auto d = hex(t.data), c = hex(t.crc);
        const uint32_t v = crc32Msg(d.data(), d.size());
        const uint32_t got = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
        CHECK(v == got, "message CRC-32: calculated %08X, captured %08X", v, got);
    }
    // CRC-9 of confirmed blocks: data octets, 7 bit serial number, then the data type mask (rate 3/4: 1FF, rate 1/2: 0F0)
    const struct { const char* d; unsigned dbsn; unsigned mask; unsigned crc; } c9[] = {
        {"c11b621ad48b979117419eb172ebe0b8", 1, kMaskRate34, 173}, {"747ea34235ccc44a774b", 1, kMaskRate12, 69},
        {"af12f44fde4b826afd58", 1, kMaskRate12, 84}, {"fe1e799c52b5e42654c223831222184e", 1, kMaskRate34, 205}};
    for (auto& t : c9) {
        const auto d = hex(t.d);
        const unsigned v = (crc9(d.data(), d.size(), t.dbsn) ^ t.mask) & 0x1FF;
        CHECK(v == t.crc, "CRC-9 of %s: calculated %u, captured %u", t.d, v, t.crc);
    }
    // the five bit checksum of an embedded LC (sum of the nine octets modulo 31)
    const auto lc = hex("00000000086520baf8");
    CHECK(checksum5(lc.data()) == 17, "embedded LC checksum");
}

static void testEmbedded() {
    // the fragments of a captured call (four bursts, 32 bits each) and four more complete embedded messages (128 bits), with the IDs they carry
    {
        const char* fr[4] = {"0A00030A", "170A0605", "0C112200", "05223F3A"};
        Bits f[4];
        for (int i = 0; i < 4; i++) f[i] = bitsOfHex(fr[i]);
        uint8_t lc[9];
        const int e = embLcDecode(f, lc);
        CHECK(e == 0, "embedded LC of a captured call: result %d", e);
        const auto want = hex("00000000086520baf8");
        CHECK(!memcmp(lc, want.data(), 9), "embedded LC contents");
        Bits g[4];
        embLcEncode(lc, g);
        for (int i = 0; i < 4; i++) CHECK(f[i] == g[i], "embedded LC fragment %d does not re-encode to the captured bits", i);
    }
    const struct { const char* h; const char* lc; } v[] = {
        {"0300030a0f0006060f033a030c393530", "00000000000620baef"}, {"03000a03060906060f03030a05000c09", "00000000000920baef"},
        {"0a00030a170906060f12110305120f0a", "00000000086520baef"}, {"09001d17271e81b730060c9a9a8e09b1", "0300002635a903d475"}};
    for (auto& t : v) {
        const Bits all = bitsOfHex(t.h);
        Bits f[4];
        for (int i = 0; i < 4; i++) f[i].assign(all.begin() + i * 32, all.begin() + i * 32 + 32);
        uint8_t lc[9];
        const int e = embLcDecode(f, lc);
        const auto want = hex(t.lc);
        CHECK(e == 0 && !memcmp(lc, want.data(), 9), "embedded LC %s: result %d", t.h, e);
        // one flipped bit in each fragment is repaired
        for (int i = 0; i < 4; i++) f[i][(i * 7 + 3) % 32] ^= 1;
        const int e2 = embLcDecode(f, lc);
        CHECK(e2 >= 1 && !memcmp(lc, want.data(), 9), "embedded LC with four wrong bits: result %d", e2);
    }
}

static void testShortLc() {
    // Activity Update messages captured on the CACH of a base station (68 bits = four pieces of 17)
    const char* vec[2] = {"00000000000010010000000000000011000000110011000010011001101000000000",
                          "00110000001110010011000000110000010101011010111111110101011010101001"};
    for (int n = 0; n < 2; n++) {
        const Bits all = bitsOfString(vec[n]);
        Bits p[4];
        for (int i = 0; i < 4; i++) p[i].assign(all.begin() + i * 17, all.begin() + i * 17 + 17);
        uint32_t lc = 0;
        const int e = shortLcDecode(p, lc);
        CHECK(e == 0, "short LC %d: result %d", n, e);
        CHECK((lc >> 24) == 1, "short LC %d: opcode %u (Activity Update is 1)", n, lc >> 24);
        Bits q[4];
        shortLcEncode(lc, q);
        for (int i = 0; i < 4; i++) CHECK(p[i] == q[i], "short LC %d does not re-encode to the captured bits", n);
    }
    // the null message (all zero) is a valid word
    Bits z[4];
    for (auto& b : z) b.assign(17, 0);
    uint32_t lc = 99;
    CHECK(shortLcDecode(z, lc) == 0 && lc == 0, "null short LC");
}

static void testCach() {
    // Known answer: where the TACT and payload bits sit in the 24 bits, and the Hamming(7,4) words. Values of the OP25 project (dmr_const.h:
    // cach_tact_bits {0, 4, 8, 12, 14, 18, 22}, cach_payload_bits {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 15, 16, 17, 19, 20, 21, 23}, hamming_7_4) and the
    // MMDVM firmware (DMRTX.cpp createCACH), which agree with each other.
    {
        static const unsigned ham[16] = {0, 11, 22, 29, 39, 44, 49, 58, 69, 78, 83, 88, 98, 105, 116, 127};
        for (unsigned info = 0; info < 16; info++) CHECK(((info << 3) | hamming743Parity(info)) == ham[info], "Hamming (7,4) word of %u: %u, OP25 has %u", info, (info << 3) | hamming743Parity(info), ham[info]);
        static const int tactPos[7] = {0, 4, 8, 12, 14, 18, 22};
        static const int payPos[17] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 15, 16, 17, 19, 20, 21, 23};
        for (int at = 0; at < 2; at++)
            for (int tc = 0; tc < 2; tc++)
                for (int ls = 0; ls < 4; ls++) {
                    const unsigned pay = 0x1A5C3;           // 1 1010 0101 1100 0011
                    Bits c;
                    cachEncode(at, tc, ls, pay, c);
                    const unsigned tact = ham[(at << 3) | (tc << 2) | ls];
                    for (int i = 0; i < 7; i++) CHECK(c[(size_t)tactPos[i]] == ((tact >> (6 - i)) & 1u), "CACH TACT bit %d (at %d tc %d lcss %d)", i, at, tc, ls);
                    for (int i = 0; i < 17; i++) CHECK(c[(size_t)payPos[i]] == ((pay >> (16 - i)) & 1u), "CACH payload bit %d", i);
                }
    }
    std::mt19937 rng(3);
    for (int i = 0; i < 200; i++) {
        const int at = rng() & 1, tc = rng() & 1, ls = rng() & 3;
        const unsigned pay = rng() & 0x1FFFF;
        Bits c;
        cachEncode(at, tc, ls, pay, c);
        int a2, t2, l2;
        unsigned p2;
        const int e = cachDecode(c, a2, t2, l2, p2);
        CHECK(e == 0 && a2 == at && t2 == tc && l2 == ls && p2 == pay, "CACH round trip");
        // a single wrong TACT bit is corrected, the payload bits are passed on as received
        Bits d = c;
        d[8] ^= 1;      // the LCSS(1) position
        const int e2 = cachDecode(d, a2, t2, l2, p2);
        CHECK(e2 == 1 && a2 == at && t2 == tc && l2 == ls, "CACH with one wrong TACT bit");
    }
}

static void testTrellis() {
    // rate 3/4 payloads captured off the air: 196 on-air bits and the 18 data octets they carry
    const struct { const char* bits; const char* data; } v[] = {
        {"0010100000101111111000101011010111010010111111110010001011100010011101100010111100111110110100100111001000101110111110100001001000"
         "100010011100101111101100101111001000101001011100100010111101110010", "006200014100480019804a00200054004100"},
        {"0010010011110110110100100011111111100010001101010010001000010010101011101101001001111111110100100110100011100010111100100001001011"
         "111010111000101111000001111000001000100100011100101111100001110010", "02f24400590020004d004100520045004b00"},
        {"0010001100100010001000100010001000100010101011000010110111110010001000100010001000100010000100001101011100100010001000100010001000"
         "100010101001000000100100100010001000100010001000100010001010100110", "0538000000000000000000000000f486aed8"}};
    for (auto& t : v) {
        const auto d = hex(t.data);
        Bits tx;
        trellis34Encode(d.data(), tx);
        CHECK(tx == bitsOfString(t.bits), "trellis encoding of %s", t.data);
        float sym[98];
        for (int k = 0; k < 98; k++) sym[k] = (float)dibitToSymbol((tx[2 * k] << 1) | tx[2 * k + 1]);
        uint8_t out[18];
        const int e = trellis34Decode(sym, out);
        CHECK(e == 0 && !memcmp(out, d.data(), 18), "trellis decode of %s: %d", t.data, e);
        // noise: add Gaussian values to the amplitudes; the soft decoder should survive an error vector that would break hard decisions
        std::mt19937 rng(9);
        std::normal_distribution<float> nd(0.f, 0.55f);
        int ok = 0;
        for (int trial = 0; trial < 50; trial++) {
            float s2[98];
            for (int k = 0; k < 98; k++) s2[k] = sym[k] + nd(rng);
            if (trellis34Decode(s2, out) >= 0 && !memcmp(out, d.data(), 18)) ok++;
        }
        CHECK(ok >= 40, "trellis decode with noise: %d of 50 right", ok);
    }
    // a non-codeword that ends in a wrong state is refused or flagged: here a stream of constant symbols
    float flat[98];
    for (auto& x : flat) x = 3.f;
    uint8_t out[18];
    const int e = trellis34Decode(flat, out);
    CHECK(e != 0, "garbage reported as a clean trellis word");
}

static void testRate1() {
    uint8_t d[24], o[24];
    for (int i = 0; i < 24; i++) d[i] = (uint8_t)(i * 17 + 3);
    Bits tx;
    rate1Encode(d, tx);
    bool pad = false;
    rate1Decode(tx, o, &pad);
    CHECK(!memcmp(d, o, 24) && pad && tx.size() == 196, "rate 1 round trip");
    // table B.10B: transmit index 0 is I(191) (the first bit of the first octet), 96..99 are pad bits, 100 is I(95) (the first bit of octet 12)
    CHECK(tx[0] == (d[0] >> 7 & 1) && tx[100] == (d[12] >> 7 & 1) && tx[99] == 0 && tx[195] == (d[23] & 1), "rate 1 bit order");
}

int main() {
    testShortCodes();
    testHamming();
    testBptc();
    testReedSolomon();
    testCrc();
    testEmbedded();
    testShortLc();
    testCach();
    testTrellis();
    testRate1();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
