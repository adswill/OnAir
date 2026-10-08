// DSC symbol code, number fields and call layout against ITU-R M.493 (Table 1 in full, the packing rules and the call tables).
#include "dect2/marine_dsc.h"
#include <cmath>
#include <cstdio>
#include <string>
#include "data/marine/dsc_table1.inc"
using namespace dect2;
using namespace dect2::marine;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    // Table 1: every one of the 128 symbols, bit for bit
    for (int s = 0; s < 128; s++) {
        const unsigned b = dscSymbolBits(s);
        std::string t;
        for (int i = 9; i >= 0; i--) t += ((b >> i) & 1) ? 'Y' : 'B';
        CHECK(t == kTable1[s], "symbol %d: %s, table %s", s, t.c_str(), kTable1[s]);
        int back = -1;
        CHECK(dscSymbolDecode(b, back) && back == s, "decode %d", s);
        // any single bit error is detected
        for (int k = 0; k < 10; k++) { int x; CHECK(!dscSymbolDecode(b ^ (1u << k), x), "single bit error in symbol %d bit %d not detected", s, k); }
    }
    // MMSI: (M, I) (D, X4) (X5, X6) (X7, X8) (X9, 0), section 5.2
    {
        int s[5];
        CHECK(dscMmsiToSymbols("232123456", s) && s[0] == 23 && s[1] == 21 && s[2] == 23 && s[3] == 45 && s[4] == 60, "MMSI symbols");
        CHECK(dscMmsiFromSymbols(s) == "232123456", "MMSI back");
        CHECK(dscMmsiToSymbols("002320064", s) && s[0] == 0 && s[1] == 23 && s[2] == 20 && s[3] == 6 && s[4] == 40, "coast station MMSI");
        const int bad[5] = {23, 21, 105, 45, 60};
        CHECK(dscMmsiFromSymbols(bad).empty(), "a service symbol is not a digit pair");
    }
    // position: quadrant, 4 digits latitude (deg, min), 5 digits longitude; all 9 = not available (8.1.2.4); time 8888 = not available
    {
        int p[5]; double lat, lon; std::string txt;
        dscPositionToSymbols(50.85, -1.3, p);         // 50 51' N 001 18' W: digits 1 5051 00118
        CHECK(p[0] == 15 && p[1] == 5 && p[2] == 10 && p[3] == 1 && p[4] == 18, "position symbols %d %d %d %d %d", p[0], p[1], p[2], p[3], p[4]);
        CHECK(dscPositionFromSymbols(p, lat, lon, &txt) && std::abs(lat - 50.85) < 1e-6 && std::abs(lon + 1.3) < 1e-6, "position back %s", txt.c_str());
        dscPositionToSymbols(-33.9, 151.2, p);        // SE quadrant = 2
        CHECK(p[0] / 10 == 2 && dscPositionFromSymbols(p, lat, lon) && lat < -33.8 && lon > 151.1, "southern hemisphere");
        const int nine[5] = {99, 99, 99, 99, 99};
        CHECK(!dscPositionFromSymbols(nine, lat, lon), "all nines");
        int h, m;
        CHECK(!dscTimeFromSymbols(88, 88, h, m) && dscTimeFromSymbols(12, 34, h, m) && h == 12 && m == 34 && !dscTimeFromSymbols(24, 0, h, m), "time");
    }
    // frequency: char 1 = units and tens of 100 Hz, char 2 = hundreds and thousands, char 3 = ten-thousands and hundred-thousands (8.2.2.1);
    // Table 13 note 2: character 1 is the last character sent, so the symbols go out 08 41 45 for 8414.5 kHz and 90 00 16 for VHF channel 16
    {
        int f[3];
        dscFrequencyToSymbols(84145, f);               // 8414.5 kHz
        CHECK(f[0] == 8 && f[1] == 41 && f[2] == 45, "8414.5 kHz symbols %d %d %d", f[0], f[1], f[2]);
        CHECK(dscFrequencyText(f) == "8414.5 kHz", "text %s", dscFrequencyText(f).c_str());
        const int vhf[3] = {90, 0, 16};
        CHECK(dscFrequencyText(vhf) == "VHF ch 16", "vhf channel");
        const int none[3] = {126, 126, 126};
        CHECK(dscFrequencyText(none).empty(), "no information");
    }
    // the ECC: even parity over the information symbols, one format specifier and one EOS (10.2)
    {
        std::vector<int> body = {112, 23, 21, 23, 45, 60, 106, 15, 50, 10, 1, 18, 12, 34, 109};
        int x = 127;
        for (int s : body) x ^= s;
        CHECK(dscEcc(body, 127) == (x & 127), "ECC");
    }
    // the layout of a distress alert (Table 4.1 of M.493-11): 112 112, self ID (5), nature, position (5), time (2), subsequent communication, EOS ECC EOS EOS
    {
        const auto body = dscBuildDistress("232123456", 106, 50.85, -1.3, 12, 34, 109);
        CHECK(body.size() == 1 + 5 + 1 + 5 + 2 + 1, "distress body %zu", body.size());
        const auto sl = dscFrameSymbols(body, 127);
        CHECK(sl[0] == 125 && sl[2] == 125 && sl[10] == 125 && sl[1] == 111 && sl[11] == 106 && sl[13] == 105 && sl[15] == 104, "phasing: DX 125 and RX 111 .. 104");
        CHECK(sl[12] == 112 && sl[14] == 112 && sl[16] == 23, "two format specifiers then the self-identification");
        CHECK(sl[17] == 112 && sl[19] == 112 && sl[21] == 23, "the format specifiers again in the RX places (5 places later)");
        const size_t eos = 12 + 2 * (2 + body.size() - 1);
        CHECK(sl[eos] == 127 && sl[eos + 2] == dscEcc(body, 127) && sl[eos + 4] == 127 && sl[eos + 6] == 127, "EOS ECC EOS EOS");
        CHECK(sl[eos + 5] == 127 && sl[eos + 7] == dscEcc(body, 127) && sl.size() == eos + 8, "EOS and ECC once more in RX");
        // parse back
        const DscCall c = dscParseCall(body, 127, true, 0);
        CHECK(c.format == 112 && c.fromMmsi == "232123456" && c.nature == 106 && c.hasPos && c.hasTime && c.utcHour == 12 && c.utcMin == 34 && c.distress, "parsed distress");
        CHECK(c.posText == "50d51'N 001d18'W", "position text '%s'", c.posText.c_str());
        CHECK(c.telecmd2 == 109, "subsequent communication");
    }
    // distress acknowledgement (Table 4.2): 116, category 112, self-ID, 110, distress MMSI, nature, position, time, subsequent
    {
        const auto body = dscBuildDistressAck("002320064", "232123456", 106, 50.85, -1.3, 12, 34, 109);
        const DscCall c = dscParseCall(body, 127, true, 0);
        CHECK(c.format == 116 && c.category == 112 && c.fromMmsi == "002320064" && c.distressMmsi == "232123456" && c.telecmd1 == 110 && c.hasPos && c.distress, "distress acknowledgement: %s", c.text.c_str());
    }
    // all ships safety call with a working frequency (Table 4.5), individual routine call (Table 4.8)
    {
        const DscCall a = dscParseCall(dscBuildAllShips("232123456", 108, 109, 126, 82910, -1), 127, true, 0);
        CHECK(a.format == 116 && a.category == 108 && a.freqRx == "8291.0 kHz" && a.freqTx.empty() && a.telecmd1 == 109, "all ships: %s rx '%s'", a.text.c_str(), a.freqRx.c_str());
        const DscCall i = dscParseCall(dscBuildIndividual("002320064", 100, "232123456", 109, 126, 82910, 82910), 117, true, 0);
        CHECK(i.format == 120 && i.to == "002320064" && i.fromMmsi == "232123456" && i.category == 100 && i.eos == 117, "individual: %s", i.text.c_str());
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
