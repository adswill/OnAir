// PACTOR-I packet layer: CRC, status byte, packet bits round trip, control signals, callsign packets, Huffman text.
#include "dect2/pactor/pactor_proto.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2::pactor;
#include "../common/test_util.h"

int main() {
    // CRC-16 CCITT with start value FFFF: the standard check value of "123456789" is 29B1
    const std::string chk = "123456789";
    CHECK(crc16((const uint8_t*)chk.data(), chk.size()) == 0x29B1, "crc check value %04X", crc16((const uint8_t*)chk.data(), chk.size()));

    for (int c = 0; c < 4; c++) for (int h = 0; h < 2; h++) for (int m = 0; m < 2; m++) for (int q = 0; q < 2; q++) {
        Status s; s.count = c; s.huffman = h; s.breakIn = m; s.qrt = q;
        const Status r = statusParse(statusByte(s));
        CHECK(r.count == c && r.huffman == (bool)h && r.breakIn == (bool)m && r.qrt == (bool)q, "status round trip");
    }

    // packets, both sizes, both headers
    for (int fast = 0; fast < 2; fast++) for (uint8_t hdr : {kHeaderA, kHeaderB}) {
        Packet p;
        p.header = hdr;
        p.status = 0x05;
        for (int i = 0; i < (fast ? kDataFast : kDataSlow); i++) p.data.push_back((uint8_t)(0x41 + i));
        const std::vector<uint8_t> bits = packetBits(p);
        CHECK(bits.size() == (size_t)(fast ? kBitsFast : kBitsSlow), "packet length %zu", bits.size());
        Packet q;
        int he = 9, cm = -1;
        CHECK(packetParse(bits, q, &he, &cm) && he == 0 && cm == 0 && q.data == p.data && q.status == p.status && q.header == hdr, "parse");
        std::vector<uint8_t> bad = bits;
        bad[20] ^= 1;
        CHECK(!packetParse(bad, q), "one data bit error must fail the CRC");
        bad = bits; bad[3] ^= 1;
        CHECK(packetParse(bad, q, &he) && he == 1, "one header bit error is reported, the CRC does not cover the header");
        // every single bit error in the CRC-covered part is caught
        int missed = 0;
        for (size_t i = 8; i < bits.size(); i++) { bad = bits; bad[i] ^= 1; if (packetParse(bad, q)) missed++; }
        CHECK(missed == 0, "%d single-bit errors passed the CRC", missed);
    }

    // control signals: the values of the public description, the pairs, distances
    CHECK(kCs[0] == 0x4D5 && kCs[1] == 0xAB2 && kCs[2] == 0x34B && kCs[3] == 0xD2C, "CS values");
    for (int a = 1; a <= 4; a++) {
        const std::vector<uint8_t> b = csBits(a);
        CHECK(b.size() == 12 && csMatch(b.data()) == a, "CS%d match", a);
        std::vector<uint8_t> e = b;
        e[2] ^= 1;
        int d = 0;
        CHECK(csMatch(e.data(), &d) == a && d == 1, "CS%d with one error", a);
        e[7] ^= 1;
        CHECK(csMatch(e.data(), &d) == a && d == 2, "CS%d with two errors", a);
        for (int c = a + 1; c <= 4; c++) {
            int dist = 0;
            const std::vector<uint8_t> o = csBits(c);
            for (int k = 0; k < 12; k++) dist += b[(size_t)k] != o[(size_t)k];
            CHECK(dist >= 6, "CS%d and CS%d only %d bits apart", a, c, dist);
        }
    }
    // the bit patterns as written in the description (least significant bit right)
    { const std::vector<uint8_t> b = csBits(1); const uint8_t w[12] = {0,1,0,0,1,1,0,1,0,1,0,1}; CHECK(std::vector<uint8_t>(w, w + 12) == b, "CS1 bits"); }

    // callsign packets
    {
        std::string call;
        CHECK(callParse(callData("LY1XYZ"), call) && call == "LY1XYZ", "call %s", call.c_str());
        CHECK(!callParse(std::vector<uint8_t>{'H','E','L','L','O','W','O','R'}, call), "text is not a call");
        CHECK(callData("AB1CD").size() == 8 && callData("AB1CD")[7] == kCallPad, "padding");
    }

    // Huffman: the entries printed in the public description, round trip, prefix freedom, loading a table
    {
        huffReset();
        CHECK(huffEntries() >= 7 && !huffComplete(), "built-in table");
        std::vector<uint8_t> bits;
        CHECK(huffEncode("tea set at sea", bits), "encode");
        CHECK(huffDecode(bits) == "tea set at sea", "decode \"%s\"", huffDecode(bits).c_str());
        CHECK(!huffEncode("xyz", bits), "unknown characters are refused");
        bool unk = false;
        const std::vector<uint8_t> junk = {1, 1, 1, 1, 1, 1, 1, 1};
        CHECK(huffDecode(junk, &unk) == "~" && unk, "unknown code");
        // a user table file
        const std::string path = "/tmp/pactor_huffman_test.txt";
        FILE* f = fopen(path.c_str(), "w");
        if (f) {
            fprintf(f, "# test\n32\t10\ne\t011\nt\t00000\ns\t00100\na\t01000\n13\t001100\n10\t001101\nn\t110\no\t111\n");
            fclose(f);
            CHECK(huffLoad(path) && huffEntries() == 9, "load: %zu entries", huffEntries());
            CHECK(huffEncode("no tea", bits) && huffDecode(bits) == "no tea", "table from file");
            huffReset();
            CHECK(huffEntries() == 7, "reset");
        } else printf("  (no /tmp, file loading not tested)\n");
    }
    // bits <-> bytes: least significant bit first
    { const std::vector<uint8_t> b = bytesToBits({0x01, 0x80}); CHECK(b[0] == 1 && b[7] == 0 && b[15] == 1 && bitsToBytes(b) == std::vector<uint8_t>({0x01, 0x80}), "bit order"); }
    return testSummary("pactor proto");
}
