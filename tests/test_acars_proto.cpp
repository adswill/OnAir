// ACARS block layer: check sequence, parity, repair, field parsing, labels. Known answers come from acarsdec (syndrom.h, acars.c) and libacars (acars.c).
#include "dect2/acars_proto.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// independent bit-by-bit CRC-16/KERMIT (reflected polynomial 0x8408), no table
static uint16_t slowCrc(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return c;
}

// the first rows of acarsdec syndrom.h: the check residue of one flipped bit, byte by byte from the end (8 values per byte)
static const uint16_t kAcarsdecSyn[] = {
    0x1189, 0x2312, 0x4624, 0x8c48, 0x1081, 0x2102, 0x4204, 0x8408,
    0x19d8, 0x33b0, 0x6760, 0xcec0, 0x9591, 0x2333, 0x4666, 0x8ccc,
    0x5adc, 0xb5b8, 0x6361, 0xc6c2, 0x8595, 0x033b, 0x0676, 0x0cec,
    0x1cbb, 0x3976, 0x72ec, 0xe5d8, 0xc3a1, 0x8f53, 0x16b7, 0x2d6e,
    0x0b44, 0x1688, 0x2d10, 0x5a20, 0xb440, 0x6091, 0xc122, 0x8a55,
    0x042b, 0x0856, 0x10ac, 0x2158, 0x42b0, 0x8560, 0x02d1, 0x05a2,
    0x9fd5, 0x37bb, 0x6f76, 0xdeec, 0xb5c9, 0x6383, 0xc706, 0x861d,
    0x81bf, 0x0b6f, 0x16de, 0x2dbc, 0x5b78, 0xb6f0, 0x65f1, 0xcbe2,
};

static std::vector<uint8_t> blockOf(const AcarsBlockSpec& s) {         // the bytes after SOH, up to the suffix, and the check bytes
    auto f = acarsBuildFrame(s, 0);
    return std::vector<uint8_t>(f.begin() + 3, f.end() - 1);          // drop SYN SYN SOH and the DEL
}

int main() {
    // ---- check sequence
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(acarsCrc(digits, 9) == 0x2189, "CRC-16/KERMIT of 123456789: %04x (published check value 0x2189)", acarsCrc(digits, 9));
    for (int v = 0; v < 256; v++) { const uint8_t b = (uint8_t)v; CHECK(acarsCrc(&b, 1) == slowCrc(&b, 1), "table against bitwise CRC at byte %d", v); }
    for (int i = 0; i < 64; i++)
        CHECK(acarsSyndrome(i / 8, i % 8) == kAcarsdecSyn[i], "syndrome of byte %d bit %d: %04x, acarsdec has %04x", i / 8, i % 8, acarsSyndrome(i / 8, i % 8), kAcarsdecSyn[i]);

    // ---- parity and the special characters
    CHECK(acarsWithParity('+') == 0xAB && acarsWithParity('*') == 0x2A, "'+' and '*' with odd parity: %02x %02x", acarsWithParity('+'), acarsWithParity('*'));
    CHECK(acarsWithParity(0x03) == kAcarsEtx && acarsWithParity(0x17) == kAcarsEtb, "ETX and ETB as sent: %02x %02x", acarsWithParity(0x03), acarsWithParity(0x17));
    CHECK(acarsWithParity(0x16) == kAcarsSyn && acarsWithParity(0x01) == kAcarsSoh && acarsWithParity(0x02) == kAcarsStx && acarsWithParity(0x7f) == 0x7f, "SYN SOH STX DEL");
    for (int c = 0; c < 128; c++) CHECK(acarsParityOk(acarsWithParity((uint8_t)c)), "parity of %d", c);

    // ---- a block put together by hand (not by acarsBuildFrame), checked and parsed
    {
        const char* body = "M01AEK0201OMDB0714";                    // downlink text: message number M01, sequence A, flight EK0201, then the text
        std::vector<uint8_t> t;
        auto add = [&](int c) { t.push_back(acarsWithParity((uint8_t)c)); };
        add('2'); for (const char* r = ".A6-EDA"; *r; r++) add(*r);
        add(0x15); add('Q'); add('A'); add('3'); t.push_back(0x02);   // mode, address, NAK, label, block id, STX
        for (const char* p = body; *p; p++) add(*p);
        t.push_back(0x83);                                            // ETX
        const uint16_t crc = slowCrc(t.data(), t.size());
        uint8_t c2[2] = {(uint8_t)(crc & 0xff), (uint8_t)(crc >> 8)};
        uint16_t chk = acarsCrc(t.data(), t.size());
        chk = acarsCrcUpdate(chk, c2[0]); chk = acarsCrcUpdate(chk, c2[1]);
        CHECK(chk == 0, "check bytes low first give residue 0, got %04x", chk);
        int fx = 0, pe = 0;
        auto copy = t;
        CHECK(acarsRepairBlock(copy.data(), (int)copy.size(), c2, fx, pe) && fx == 0 && pe == 0, "good block passes untouched");
        std::vector<uint8_t> t7;
        for (uint8_t b : t) t7.push_back(b & 0x7f);
        AcarsMessage m;
        CHECK(acarsParseBlock(t7.data(), (int)t7.size(), m), "parse");
        CHECK(m.mode == '2' && m.reg == "A6-EDA" && m.ack == '!' && m.label == "QA" && m.blockId == '3' && m.downlink && m.finalBlock, "header fields: %c %s %c %s %c", m.mode, m.reg.c_str(), m.ack, m.label.c_str(), m.blockId);
        CHECK(m.msgNum == "M01" && m.msgSeq == 'A' && m.flightId == "EK0201" && m.text == "OMDB0714", "downlink fields: %s %c %s '%s'", m.msgNum.c_str(), m.msgSeq, m.flightId.c_str(), m.text.c_str());
        CHECK(m.decoded == "from OMDB out 0714", "QA reading: '%s' (acarsdec label_qa: origin, then gate out)", m.decoded.c_str());
        CHECK(std::string(m.labelText) == "Gate out", "label text '%s'", m.labelText.c_str());
    }

    // ---- uplink, label "_<DEL>", no text; and label meanings
    {
        AcarsBlockSpec s; s.reg = "A6-EDA"; s.ack = '3'; s.label[0] = '_'; s.label[1] = 'd'; s.blockId = 'S'; s.hasText = false;
        auto b = blockOf(s);
        std::vector<uint8_t> t7; for (size_t i = 0; i + 2 < b.size(); i++) t7.push_back(b[i] & 0x7f);
        AcarsMessage m;
        CHECK(acarsParseBlock(t7.data(), (int)t7.size(), m), "parse ack-only block");
        CHECK(m.label == "_d" && !m.downlink && m.ack == '3' && m.blockId == 'S' && m.text.empty() && m.labelText == "Command or response", "ack-only uplink: label %s down %d ack %c", m.label.c_str(), m.downlink, m.ack);
        CHECK(std::string(acarsLabelText('Q', '0')) == "Link test" && std::string(acarsLabelText('S', 'A')) == "Media advisory" && std::string(acarsLabelText('Z', 'Z')).empty(), "labels");
    }
    // H1 sublabel and MFI (libacars acars.c): downlink "#M1B/PS ...", uplink "- #M1/WX ..."
    {
        auto parse = [](const char* label, char bid, const std::string& text, AcarsMessage& m) {
            AcarsBlockSpec s; s.reg = "A6-EDA"; s.label[0] = label[0]; s.label[1] = label[1]; s.blockId = bid; s.text = text;
            auto b = blockOf(s);
            std::vector<uint8_t> t7; for (size_t i = 0; i + 2 < b.size(); i++) t7.push_back(b[i] & 0x7f);
            return acarsParseBlock(t7.data(), (int)t7.size(), m);
        };
        AcarsMessage m;
        CHECK(parse("H1", '1', "M02AEK0201#M1B/PS POS N25312", m) && m.sublabel == "M1" && m.mfi == "PS", "downlink sublabel '%s' mfi '%s'", m.sublabel.c_str(), m.mfi.c_str());
        AcarsMessage u;
        CHECK(parse("H1", 'B', "- #MD/AT CLEARED", u) && u.sublabel == "MD" && u.mfi == "AT" && !u.downlink, "uplink sublabel '%s' mfi '%s'", u.sublabel.c_str(), u.mfi.c_str());
    }
    // OOOI text of the Q labels (acarsdec label.c layouts)
    CHECK(acarsDecodeText("Q1", "OMDB07140722145114592500EGLL") == "from OMDB out 0714 off 0722 on 1451 in 1459 to EGLL", "Q1: '%s'", acarsDecodeText("Q1", "OMDB07140722145114592500EGLL").c_str());
    CHECK(acarsDecodeText("QB", "EGLL0722") == "from EGLL off 0722" && acarsDecodeText("QB", "EGLL07") == "" && acarsDecodeText("QB", "egll0722") == "" && acarsDecodeText("QB", "EGLL2961").empty(), "QB checks");

    CHECK(acarsDecodeText("SA", "0EV123045V") == "link established: VHF ACARS at 12:30:45, available: VHF ACARS", "SA: '%s'", acarsDecodeText("SA", "0EV123045V").c_str());
    CHECK(acarsDecodeText("SA", "0LS235959VS/ATC") == "link lost: default SATCOM at 23:59:59, available: VHF ACARS, default SATCOM" && acarsDecodeText("SA", "1EV123045V").empty() && acarsDecodeText("SA", "0EV126045V").empty(), "SA checks");

    // ---- repair: every single flipped bit of a block is found and put back
    {
        AcarsBlockSpec s; s.reg = "A6-EDA"; s.label[0] = 'H'; s.label[1] = '1'; s.blockId = '4'; s.text = "M05AEK0201#M1B/PS POS N25312 E055214,FL350,0714,EGLL,22";
        const auto good = blockOf(s);
        const int len = (int)good.size() - 2;
        uint8_t crc[2] = {good[(size_t)len], good[(size_t)len + 1]};
        int fixedAll = 0, total = 0;
        for (int i = 0; i < len; i++)
            for (int b = 0; b < 8; b++) {
                auto t = good; t[(size_t)i] ^= (uint8_t)(1 << b);
                int fx, pe;
                const bool ok = acarsRepairBlock(t.data(), len, crc, fx, pe);
                total++;
                if (ok && fx == 1 && pe == 1 && memcmp(t.data(), good.data(), (size_t)len) == 0) fixedAll++;
            }
        CHECK(fixedAll == total, "single bit errors repaired: %d of %d", fixedAll, total);
        // an error in a check byte: the text is fine, the block passes
        for (int b = 0; b < 16; b++) {
            uint8_t c2[2] = {crc[0], crc[1]}; c2[b / 8] ^= (uint8_t)(1 << (b % 8));
            auto t = good; int fx, pe;
            CHECK(acarsRepairBlock(t.data(), len, c2, fx, pe) && memcmp(t.data(), good.data(), (size_t)len) == 0, "check byte bit %d", b);
        }
        // two flipped bits in one byte (parity unchanged): the second repair route
        int two = 0, twoTotal = 0;
        for (int i = 0; i < len; i++)
            for (int a = 0; a < 8; a++)
                for (int b = a + 1; b < 8; b++) {
                    auto t = good; t[(size_t)i] ^= (uint8_t)((1 << a) | (1 << b));
                    int fx, pe; twoTotal++;
                    if (acarsRepairBlock(t.data(), len, crc, fx, pe) && fx == 2 && memcmp(t.data(), good.data(), (size_t)len) == 0) two++;
                }
        CHECK(two == twoTotal, "two bit errors in one byte repaired: %d of %d", two, twoTotal);
        // three single bit errors in three bytes: repaired; four: refused
        std::mt19937 rng(7);
        int three = 0, threeTotal = 200, four = 0;
        for (int k = 0; k < threeTotal; k++) {
            auto t = good;
            int p[3];
            for (int j = 0; j < 3; j++) { bool dup; do { p[j] = (int)(rng() % (uint32_t)len); dup = false; for (int q = 0; q < j; q++) dup |= p[q] == p[j]; } while (dup); t[(size_t)p[j]] ^= (uint8_t)(1 << (rng() % 8)); }
            int fx, pe;
            if (acarsRepairBlock(t.data(), len, crc, fx, pe) && memcmp(t.data(), good.data(), (size_t)len) == 0) three++;
            auto u = good;
            for (int j = 0; j < 4; j++) u[(size_t)((p[j % 3] + j / 3 * 7) % len)] ^= 1;
            if (acarsRepairBlock(u.data(), len, crc, fx, pe)) four++;
        }
        CHECK(three >= threeTotal * 99 / 100, "three single bit errors repaired: %d of %d", three, threeTotal);
        printf("  repair: %d/%d single, %d/%d double-in-byte, %d/%d triple, %d of %d four-error blocks passed\n", fixedAll, total, two, twoTotal, three, threeTotal, four, threeTotal);
        // random damage: how often does a repaired block differ from the original (a wrong block let through)?
        int accepted = 0, wrong = 0, trials = 40000;
        for (int k = 0; k < trials; k++) {
            auto t = good;
            const int nerr = 1 + (int)(rng() % 6);
            for (int j = 0; j < nerr; j++) t[rng() % (uint32_t)len] ^= (uint8_t)(1 << (rng() % 8));
            uint8_t c2[2] = {crc[0], crc[1]};
            int fx, pe;
            if (acarsRepairBlock(t.data(), len, c2, fx, pe)) { accepted++; if (memcmp(t.data(), good.data(), (size_t)len) != 0) wrong++; }
        }
        printf("  random 1 to 6 bit errors: %d accepted, %d of them wrong (%.3f %% of all damaged blocks)\n", accepted, wrong, 100.0 * wrong / trials);
        CHECK(wrong * 1000 <= trials * 5, "wrong blocks let through: %d of %d", wrong, trials);
    }

    // ---- the transmission: pre-key, "+*", SYN SYN SOH ... DEL, and it is long enough for the pre-key to last 53 ms at 2400 bit/s
    {
        AcarsBlockSpec s; s.reg = "A6-EDA"; s.label[0] = 'Q'; s.label[1] = '0'; s.blockId = '0'; s.text = "M01AEK0201";
        const auto f = acarsBuildFrame(s);
        CHECK(f.size() == 16 + 2 + 3 + 12 + 1 + 10 + 1 + 2 + 1, "frame length %zu", f.size());
        bool pre = true; for (int i = 0; i < 16; i++) pre &= f[(size_t)i] == 0xff;
        CHECK(pre && f[16] == 0xAB && f[17] == 0x2A && f[18] == 0x16 && f[19] == 0x16 && f[20] == 0x01 && f.back() == 0x7f, "frame layout");
        CHECK(16 * 8 / 2400.0 > 0.0533 && 16 * 8 / 2400.0 < 0.0534, "pre-key duration");
    }
    printf(fails ? "acars proto: %d FAILED\n" : "acars proto: all passed\n", fails);
    return fails ? 1 : 0;
}
