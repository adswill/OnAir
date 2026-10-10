// HF digital, FT8 / FT4 / FT2 / WSPR tables and codecs: the tables against values quoted from a second source (WSJT-X's
// ldpc_174_91_c_generator.f90 row ends, genft4.f90's rvec, genft8.f90 / genft4.f90 Costas arrays and Gray maps, wsprsim_utils.c /
// JTEncode's WSPR sync vector), the generator against the parity checks, CRC and LDPC, lookup3's published test values, and the
// 77-bit and WSPR messages packed and unpacked.
#include "../core/src/hfdig_ftx_int.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace dect2::ftx;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// WSJT-X ldpc_174_91_c_generator.f90: characters 1-6 and 18-23 of each of the 83 hex rows
static const char* kWsjtRowEnds[83] = {
    "8329ce..9f27fc", "761c26..493132", "dc2659..0a1bdc", "1b3f41..ec7f62", "09fda4..34783a", "077ccc..c3d48a", "29b62a..e1a9da",
    "6054fa..0c8c3e", "e20798..84ae90", "775c9c..e56318", "b0b811..13487c", "18a0c9..c5ea32", "76471e..1b12b8", "ffbccb..b47b2e",
    "66a72a..f67170", "c42436..363a18", "0dff73..b1c270", "15b488..94972e", "29a89c..489b0e", "4f126f..bd6b94", "99c472..4e0940",
    "1919b7..b4f1e8", "09db12..6df6b8", "488fc3..4eafb4", "827423..6eb5fe", "abe197..144a9a", "2b500e..bdbdd0", "c474aa..669360",
    "8eba1a..718cec", "753844..42012e", "06ff83..5c1268", "3b3741..ec3f62", "9a4a5a..24842c", "bc29f4..9610a4", "2663ae..b29488",
    "46f231..814418", "3fb2ce..e06fbe", "de8748..1a0a2e", "fcd7cc..ba1412", "f02614..474cec", "441011..dd7012", "088fc3..4eafb4",
    "b8fef1..a078c0", "5afea7..d99a90", "49a701..dc9076", "1944d0..6cc7d0", "251f62..714002", "56471f..0b12b8", "2b8e49..537fa0",
    "6b550a..e95c26", "a18ad2..4f6c84", "10c2e5..d80758", "ef34a4..db2eb0", "7e9c0c..36e000", "3693e5..079e86", "bfb2ce..e07fbe",
    "7ee182..7d4b08", "a066cb..664126", "bb2372..cc4cd2", "ded9db..5609b4", "d9a701..dc9036", "9ad46a..ab5fc4", "e5921c..d7d3c2",
    "4f14da..a73352", "8b8b50..df770e", "22831c..d04b68", "213b83..ee7180", "5d926b..1a4e12", "66ab79..509e56", "958148..d68baa",
    "b8ce02..23ab14", "f4331d..752746", "6da23b..3cf9c8", "a636bc..ae67fe", "5cb0d8..089a20", "f11f10..cdd80a", "1fbb53..30d5ba",
    "fcb86b..a5d034", "a53443..22e34c", "c989d9..d75130", "7bb38b..3ae962", "2644eb..d1f42c", "608cc8..d69600"};
// WSJT-X genft4.f90 rvec
static const char* kRvec = "01001010010111101000100110110100101100001000101001111001010101011011111000101";
// WSJT-X wsprsim_utils.c / wsprd.c pr3, also JTEncode's sync vector
static const char* kSyncRows[9] = {"110000001000111000", "100101111000000010", "010100000010110011", "010001101000011010",
                                   "101010010010110001", "101010001000001001", "001110110011010001", "110000010100110000",
                                   "000110101100011000"};

int main() {
    // LDPC generator rows against WSJT-X's (first and last 6 of the 23 hex characters)
    for (int r = 0; r < kM; r++) {
        char hex[25];
        for (int i = 0; i < 12; i++) snprintf(hex + 2 * i, 3, "%02x", kLdpcGen[r][i]);
        const std::string h(hex, 23), want = kWsjtRowEnds[r];
        const std::string got = h.substr(0, 6) + ".." + h.substr(17, 6);
        CHECK(got == want, "generator row %d: %s, WSJT-X %s", r + 1, got.c_str(), want.c_str());
        CHECK((kLdpcGen[r][11] & 0x1F) == 0, "row %d has bits past 91", r + 1);
    }
    for (int i = 0; i < 77; i++) CHECK(kFt4Rvec[i] == kRvec[i] - '0', "rvec %d", i);
    const uint8_t costas8[7] = {3, 1, 4, 0, 6, 5, 2}, gray8[8] = {0, 1, 3, 2, 5, 6, 4, 7};
    CHECK(!memcmp(kFt8Costas, costas8, 7) && !memcmp(kFt8Gray, gray8, 8), "FT8 Costas / Gray");
    const uint8_t c4[4][4] = {{0, 1, 3, 2}, {1, 0, 2, 3}, {2, 3, 1, 0}, {3, 2, 0, 1}}, g4[4] = {0, 1, 3, 2};
    CHECK(!memcmp(kFt4Costas, c4, 16) && !memcmp(kFt4Gray, g4, 4), "FT4 Costas / Gray");
    for (int i = 0; i < 162; i++) CHECK(kWsprSync[i] == kSyncRows[i / 18][i % 18] - '0', "WSPR sync %d", i);
    int ones = 0;
    for (int i = 0; i < 162; i++) ones += kWsprSync[i];
    printf("WSPR sync: %d ones\n", ones);
    // Mn is the transpose of Nm, and every row count matches
    for (int m = 0; m < kM; m++) {
        int cnt = 0;
        for (int k = 0; k < 7; k++) if (kLdpcNm[m][k]) {
            cnt++;
            const int n = kLdpcNm[m][k] - 1;
            CHECK(kLdpcMn[n][0] == m + 1 || kLdpcMn[n][1] == m + 1 || kLdpcMn[n][2] == m + 1, "Nm/Mn %d %d", m, n);
        }
        CHECK(cnt == kLdpcNumRows[m], "row count %d", m);
    }
    // codewords of random messages satisfy all parity checks (generator against Nm: two tables, one code), decode with errors
    uint32_t s = 7;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return s >> 8; };
    int okLdpc = 0;
    for (int t = 0; t < 50; t++) {
        uint8_t m[77], cw[kN], dec[kN];
        for (int i = 0; i < 77; i++) m[i] = rnd() & 1;
        encode174(m, cw);
        for (int r = 0; r < kM; r++) {
            int x = 0;
            for (int k = 0; k < kLdpcNumRows[r]; k++) x ^= cw[kLdpcNm[r][k] - 1];
            CHECK(x == 0, "codeword %d fails check %d", t, r);
        }
        CHECK(checkCrc(cw), "crc %d", t);
        // BPSK over Gaussian noise at Eb/N0 = 3 dB (sigma 0.69), soft LLRs: the (174,91) code decodes nearly all
        float llr[kN];
        const double sig = 0.69;
        for (int i = 0; i < kN; i++) {
            double g = 0;
            for (int k = 0; k < 12; k++) g += (rnd() & 0xFFFF) / 65536.0;
            const double y = (cw[i] ? 1.0 : -1.0) + sig * (g - 6.0);
            llr[i] = (float)(2 * y / (sig * sig));
        }
        if (ldpcDecode(llr, 40, dec) == 0 && !memcmp(dec, cw, kN)) okLdpc++;
    }
    printf("LDPC at Eb/N0 3 dB: %d of 50 decoded\n", okLdpc);
    CHECK(okLdpc >= 45, "LDPC decoded only %d of 50 at 3 dB", okLdpc);
    // lookup3 hashlittle(): published values
    CHECK(nhash("", 0, 0) == 0xdeadbeefu, "lookup3 empty %08x", nhash("", 0, 0));
    CHECK(nhash("Four score and seven years ago", 30, 0) == 0x17770551u, "lookup3 %08x", nhash("Four score and seven years ago", 30, 0));
    CHECK(nhash("Four score and seven years ago", 30, 1) == 0xcd628161u, "lookup3 %08x", nhash("Four score and seven years ago", 30, 1));
    // 77-bit messages: pack, unpack, the same text back
    const char* msgs[] = {"CQ K1ABC FN42", "K1ABC W9XYZ -12", "W9XYZ K1ABC R-05", "K1ABC W9XYZ RR73", "K1ABC W9XYZ 73", "K1ABC W9XYZ RRR",
                          "CQ DX K1ABC FN42", "CQ 123 K1ABC FN42", "K1ABC/R W9XYZ EN37", "K1ABC W9XYZ/P R JO62", "QRZ W9XYZ EN37",
                          "K1ABC W9XYZ +07", "K1ABC W9XYZ -45", "CQ PJ4/K1ABC", "TNX BOB 73 GL", "7123456789ABCDEF01", "DE W9XYZ",
                          "CQ 3DA0XYZ KG53", "K1ABC W9XYZ R EN37", "CQ TEST DL1XYZ JO62", "G4ABC DL1XYZ RR73"};
    CallHash hash;
    for (const char* m : msgs) {
        uint8_t b[77];
        CHECK(pack77(m, b), "cannot pack \"%s\"", m);
        const std::string u = unpack77(b, &hash);
        CHECK(u == m, "\"%s\" came back as \"%s\"", m, u.c_str());
    }
    {   // hashed calls are shown once heard
        uint8_t b[77];
        pack77("<PJ4/K1ABC> W9XYZ RR73", b);
        CHECK(unpack77(b, &hash) == "<PJ4/K1ABC> W9XYZ RR73", "type 4 hashed: %s", unpack77(b, &hash).c_str());
        CallHash fresh;
        CHECK(unpack77(b, &fresh) == "<...> W9XYZ RR73", "unknown hash: %s", unpack77(b, &fresh).c_str());
    }
    // WSPR messages
    std::map<uint32_t, std::string> wh;
    const char* w[] = {"K1ABC FN42 37", "G4XYZ IO91 23", "PJ4/K1ABC 30", "DL1XYZ/P 10", "K1ABC/12 33", "<K1ABC> FN42AX 37"};
    for (const char* m : w) {
        uint8_t d[7];
        CHECK(wsprPack(m, d), "WSPR cannot pack %s", m);
        int dbm = -1;
        const std::string u = wsprUnpack(d, wh, &dbm);
        CHECK(u == m, "WSPR \"%s\" came back as \"%s\"", m, u.c_str());
        // through the code: encode, interleave, deinterleave, Fano
        uint8_t coded[162], ch[162];
        wsprConvEncode(d, coded);
        wsprInterleave(coded, ch);
        float llr[162], dl[162];
        for (int i = 0; i < 162; i++) llr[i] = ch[i] ? 3.f : -3.f;
        for (int e = 0; e < 10; e++) llr[(e * 37 + 5) % 162] *= -1;
        wsprDeinterleave(llr, dl);
        uint8_t back[7];
        CHECK(wsprFano(dl, back, 100000) && !memcmp(back, d, 7), "Fano failed on %s", m);
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
