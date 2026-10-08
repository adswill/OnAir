// Known answers for the Iridium frame layer, from public sources only (nothing here was checked against an off-air recording):
//  - iridium-toolkit tests/test_parser.py: a real ISY frame (RAW bits after the unique word) that the toolkit decodes as "ISY: Sync=OK";
//  - iridium-toolkit bitsparser.py: unique words, UW_DOWNLINK symbols, the FILL blocks of ring alerts and messages (BCH words of poly 1207),
//    the de-interleavers (re-written here from the Python string code as an independent check), the pager checksum rule;
//  - iridium-toolkit FORMAT.md: the printed IRA line (sat 16 beam 37 xyz (+1396,+0711,+0862) -> pos +28.82/+026.99 alt 797, 1 page, FILL=10,
//    432 symbols) and the printed MSG line (ric 3525766 fmt 5 seq 30, csum 0b, 60 characters, msg: hex);
//  - gr-iridium lib/iridium_qpsk_demod_impl.cc: the delta symbol to bit mapping.
#include "dect2/iridium_frame.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef std::vector<uint8_t> Bits;
static Bits B(const std::string& s) { Bits b; for (char c : s) b.push_back(c == '1'); return b; }
static std::string S(const Bits& b) { std::string s; for (uint8_t x : b) s += char('0' + x); return s; }
static std::string swapStr(const std::string& s) { std::string o = s; for (size_t i = 0; i + 1 < o.size(); i += 2) std::swap(o[i], o[i + 1]); return o; }
static unsigned remainder(unsigned poly, const std::string& bits) {
    unsigned long long v = 0;
    for (char c : bits) v = (v << 1) | (c == '1');
    int pl = 0; for (unsigned p = poly; p; p >>= 1) pl++;
    for (int sh = 64 - pl; sh >= 0; sh--) if ((v >> (sh + pl - 1)) & 1) v ^= (unsigned long long)poly << sh;
    return (unsigned)v;
}
// the toolkit's de_interleave / de_interleave3, as written there
static void pyDeInt(const std::string& g, std::string& odd, std::string& even) {
    std::vector<std::string> sym;
    for (size_t z = 0; z + 1 < g.size(); z += 2) sym.push_back(std::string() + g[z + 1] + g[z]);
    even.clear(); odd.clear();
    for (long x = (long)sym.size() - 2; x >= 0; x -= 2) even += sym[x];
    for (long x = (long)sym.size() - 1; x >= 0; x -= 2) odd += sym[x];
}
static void pyDeInt3(const std::string& g, std::string& first, std::string& second, std::string& third) {
    std::vector<std::string> sym;
    for (size_t z = 0; z + 1 < g.size(); z += 2) sym.push_back(std::string() + g[z + 1] + g[z]);
    first.clear(); second.clear(); third.clear();
    for (long x = (long)sym.size() - 3; x >= 0; x -= 3) third += sym[x];
    for (long x = (long)sym.size() - 2; x >= 0; x -= 3) second += sym[x];
    for (long x = (long)sym.size() - 1; x >= 0; x -= 3) first += sym[x];
}
static bool evenParity(const std::string& s) { int n = 0; for (char c : s) n += c == '1'; return (n & 1) == 0; }

int main() {
    // ---- unique words and the delta symbol mapping ----
    {
        // toolkit: iridium_access / uplink_access; UW_DOWNLINK = [0,2,2,2,2,0,0,0,2,0,0,2] after undoing the differential coding
        CHECK(S(iridiumUniqueWordBits(true)) == "001100000011000011110011", "downlink unique word");
        CHECK(S(iridiumUniqueWordBits(false)) == "110011000011110011111100", "uplink unique word");
        std::vector<uint8_t> steps;
        iridiumBitsToSteps(iridiumUniqueWordBits(true), steps);
        int acc = 0;
        const int uwDl[12] = { 0, 2, 2, 2, 2, 0, 0, 0, 2, 0, 0, 2 }, uwUl[12] = { 2, 2, 0, 0, 0, 2, 0, 0, 2, 0, 2, 2 };
        for (int i = 0; i < 12; i++) { acc = (acc + steps[i]) & 3; CHECK(acc == uwDl[i], "UW DL symbol %d: %d", i, acc); }
        iridiumBitsToSteps(iridiumUniqueWordBits(false), steps);
        acc = 0;
        for (int i = 0; i < 12; i++) { acc = (acc + steps[i]) & 3; CHECK(acc == uwUl[i], "UW UL symbol %d: %d", i, acc); }
        // gr-iridium decode_deqpsk: delta 0,1,2,3 -> code 0,2,3,1, written (bit1, bit0)
        std::vector<uint8_t> st = { 0, 1, 2, 3 }, bits;
        iridiumStepsToBits(st, bits);
        CHECK(S(bits) == "00" "10" "11" "01", "step to bits: %s", S(bits).c_str());
        iridiumBitsToSteps(bits, steps);
        CHECK(steps == st, "bits to step");
    }
    // ---- the real ISY frame of iridium-toolkit's tests/test_parser.py ----
    const std::string isyRaw = "0001000110111111000000100000001000100011000100"
                               "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101"
                               "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101";
    {
        CHECK(isyRaw.size() == 358, "ISY KAT length %zu", isyRaw.size());
        IridiumBurstBits b; b.bits = B(isyRaw);
        IridiumFrame f = decodeIridiumBurst(b);
        CHECK(f.type == IridiumType::ISY && f.typeName == "ISY", "type %s", f.typeName.c_str());
        CHECK(f.ok && f.corrected == 0 && f.lcwFt == 7, "ok %d corrected %d ft %d", f.ok, f.corrected, f.lcwFt);
        CHECK(S(iridiumBuildIsy()) == isyRaw, "iridiumBuildIsy differs from the real frame");
        // two bit errors in the link control word (corrected by its BCH codes) and one in the sync pattern
        for (int pos : { 3, 11, 25, 40, 45 }) {
            Bits e = B(isyRaw); e[pos] ^= 1;
            IridiumBurstBits bb; bb.bits = e;
            IridiumFrame g = decodeIridiumBurst(bb);
            CHECK(g.type == IridiumType::ISY && g.lcwFt == 7, "ISY with an error at %d: %s ft %d", pos, g.typeName.c_str(), g.lcwFt);
        }
        Bits e = B(isyRaw); e[100] ^= 1;
        IridiumBurstBits bb; bb.bits = e;
        IridiumFrame g = decodeIridiumBurst(bb);
        CHECK(g.type == IridiumType::ISY && !g.ok && g.corrected == 1, "ISY with a wrong sync symbol: ok %d corrected %d", g.ok, g.corrected);
    }
    // ---- FILL blocks (bitsparser.py): BCH words of poly 1207 with even parity; they come out of our interleaver as the toolkit expects ----
    {
        const std::string fa = "10100010011100111011111101101101", fb = "01010100010001011100001011100110";
        CHECK(remainder(1207, fa.substr(0, 31)) == 0 && remainder(1207, fb.substr(0, 31)) == 0, "FILL is not a BCH(1207) word");
        CHECK(evenParity(fa) && evenParity(fb), "FILL parity");
        Bits ira = iridiumBuildIra(16, 37, 28.8, 27.0, 797, { 0x113dbaf8 });
        CHECK(ira.size() == 864, "IRA length %zu (432 symbols expected)", ira.size());
        std::string in = swapStr(S(ira));   // the toolkit's own order
        std::string o, e;
        pyDeInt(in.substr(864 - 64, 64), o, e);
        CHECK(o == fa && e == fb, "last group of an IRA is not the FILL pattern");
        // ten FILL groups after one page and its end page, like FORMAT.md's "FILL=10"
        int fills = 0;
        for (int g = 0; g < 12; g++) { pyDeInt(in.substr(96 + 64 * g, 64), o, e); if (o == fa && e == fb) fills++; }
        CHECK(fills == 10, "FILL groups %d (FORMAT.md example: 10 with one page)", fills);
        std::string a, b, c;
        pyDeInt3(in.substr(0, 96), a, b, c);
        CHECK(remainder(1207, a.substr(0, 31)) == 0 && remainder(1207, b.substr(0, 31)) == 0 && remainder(1207, c.substr(0, 31)) == 0 &&
              evenParity(a) && evenParity(b) && evenParity(c), "ring alert header blocks through the toolkit's de_interleave3");
        CHECK(a.substr(0, 7) == "0010000", "sat bits %s", a.substr(0, 7).c_str());
        // IBC: a 6-bit header (poly 29) then four groups of 64
        Bits ibc = iridiumBuildIbc(28, 24, 1.77e9);
        CHECK(ibc.size() == 262, "IBC length %zu (131 symbols expected)", ibc.size());
        std::string ii = swapStr(S(ibc));
        CHECK(remainder(29, ii.substr(0, 6)) == 0, "IBC header");
        pyDeInt(ii.substr(6, 64), o, e);
        CHECK(remainder(1207, o.substr(0, 31)) == 0 && remainder(1207, e.substr(0, 31)) == 0 && evenParity(o) && evenParity(e), "IBC blocks");
        CHECK(o.substr(0, 7) == "0011100" && o.substr(7, 6) == "011000", "IBC sat/beam bits %s", o.substr(0, 13).c_str());
    }
    // ---- the printed ring alert of FORMAT.md ----
    {
        // sat:016 beam:37 xyz=(+1396,+0711,+0862) pos=(+28.82/+026.99) alt=797 RAI:48 ?10 bc_sb:07 P01: PAGE(tmsi:113dbaf8 msc_id:02) {OK} FILL=10
        std::vector<IridiumPage> pg(1);
        pg[0].tmsi = 0x113dbaf8; pg[0].mscId = 2;
        Bits bits = iridiumBuildIraRaw(16, 37, 1396, 711, 862, 48, 1, 0, 7, pg);
        IridiumBurstBits b; b.bits = bits;
        IridiumFrame f = decodeIridiumBurst(b);
        CHECK(f.type == IridiumType::IRA && f.ok, "type %s ok %d", f.typeName.c_str(), f.ok);
        CHECK(f.satId == 16 && f.beamId == 37, "sat %d beam %d", f.satId, f.beamId);
        CHECK(std::fabs(f.lat - 28.82) < 0.005 && std::fabs(f.lon - 26.99) < 0.005, "pos %+.3f/%+.3f", f.lat, f.lon);
        CHECK(int(f.altKm) == 797, "alt %.2f (printed 797)", f.altKm);
        CHECK(f.paged == 1, "paged %d", f.paged);
        // the other example of the iridium-toolkit wiki: sat 81 beam 20 pos +46.64/-115.08 alt 791, 1 page (position rounded to 4 km units)
        Bits b2 = iridiumBuildIra(81, 20, 46.64, -115.08, 791, { 0x0ca5b2e2 });
        IridiumBurstBits bb; bb.bits = b2;
        IridiumFrame g = decodeIridiumBurst(bb);
        CHECK(g.type == IridiumType::IRA && g.satId == 81 && g.beamId == 20 && g.paged == 1, "wiki IRA: %s sat %d beam %d", g.typeName.c_str(), g.satId, g.beamId);
        CHECK(std::fabs(g.lat - 46.64) < 0.1 && std::fabs(g.lon + 115.08) < 0.1 && std::fabs(g.altKm - 791) < 4, "wiki IRA pos %+.2f/%+.2f alt %.1f", g.lat, g.lon, g.altKm);
    }
    // ---- the printed pager message of FORMAT.md ----
    {
        const std::string text = "UgDMLCae6BeEkeFXoJoGD+ZGu9M4pwq+KC3MlqBwZYRMIRgFfspbEZAFroq0";
        const std::string hex = "ab9e24d990f0e56d0b2c5d796358df2b7c788aed47eae66b4e1df8ab970d9cdd9c6177b56694d934b3c6cdcf8628b6a0c6e5bf8b0f";
        // "csum:0b" is the inverted 7-bit sum of the text: the toolkit's messagechecksum
        int c = 0;
        for (char ch : text) c = (c + ch) % 128;
        CHECK(((~c) & 127) == 0x0b, "checksum of the printed text %02x", (~c) & 127);
        Bits bits = iridiumBuildMsg(3525766, 30, 0, 1, text);
        IridiumBurstBits b; b.bits = bits; b.timeSec = 100;
        IridiumFrame f = decodeIridiumBurst(b);
        CHECK(f.type == IridiumType::MSG && f.ok, "type %s ok %d crcOk %d", f.typeName.c_str(), f.ok, f.crcOk);
        CHECK(f.ric == 3525766 && f.msgFmt == 5 && f.msgSeq == 30 && f.msgChecksum == 0x0b, "ric %d fmt %d seq %d csum %02x", f.ric, f.msgFmt, f.msgSeq, f.msgChecksum);
        CHECK(f.block == 0 && f.blocks == 1, "part %d of %d", f.block, f.blocks);
        CHECK(f.msgText == text, "text %s", f.msgText.c_str());
        CHECK(f.hex == hex + ".", "msg field %s", f.hex.c_str());
        IridiumMsgAssembler as;
        auto out = as.feed(f, 100);
        CHECK(out.size() == 1 && out[0].complete && out[0].text == text && out[0].ric == 3525766 && out[0].seq == 30, "assembled %zu", out.size());
        // 26 blocks of 21 bits: header, 24 data blocks, one trailer ("len:13/T1/F00")
        CHECK(bits.size() == 32 + 64 * 13, "MSG length %zu", bits.size());
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
