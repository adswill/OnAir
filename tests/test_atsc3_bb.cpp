// ATSC 3.0 baseband packet header (A/322 5.2.2): the worked example of the standard, round trips, the counter extension, and scrambling.
#include "dect2/atsc3_bb.h"
#include <cstdio>

using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    // the example of 5.2.2.1: pointer 130, MODE = 1, OFI = 0 gives 10000010 00000100
    BbHeader h; h.pointer = 130; h.twoByteBase = true;
    auto b = makeBbHeader(h);
    CHECK(b.size() == 2 && b[0] == 0x82 && b[1] == 0x04, "pointer 130 example");
    // one-byte base field
    h = BbHeader(); h.pointer = 5;
    b = makeBbHeader(h);
    BbHeader p;
    CHECK(b.size() == 1 && b[0] == 5 && parseBbHeader(b.data(), 1, p) && p.pointer == 5 && p.headerBytes == 1, "one byte base field");
    // no ALP packet starts: pointer 8191 in a two-byte base field
    h = BbHeader(); h.pointer = 8191; h.twoByteBase = true;
    b = makeBbHeader(h);
    CHECK(parseBbHeader(b.data(), (int)b.size(), p) && p.pointer == 8191 && p.twoByteBase, "pointer 8191");
    // the example of 5.2.2.3.1: OFI = 01, EXT_LEN = 5 with a counter: header of 2 + 1 + 5 = 8 bytes
    h = BbHeader(); h.pointer = 0; h.ofi = 1; h.extType = 0; h.extLen = 5; h.counter = 0x1234;
    b = makeBbHeader(h);
    CHECK(b.size() == 8, "short extension with counter is 8 bytes");
    CHECK(parseBbHeader(b.data(), (int)b.size(), p) && p.ofi == 1 && p.extType == 0 && p.extLen == 5 && p.counter == 0x1234 && p.headerBytes == 8, "parse counter extension");
    // long extension mode
    h = BbHeader(); h.pointer = 700; h.ofi = 2; h.extType = 7; h.extLen = 300;
    b = makeBbHeader(h);
    CHECK((int)b.size() == 4 + 300 && parseBbHeader(b.data(), (int)b.size(), p) && p.ofi == 2 && p.extType == 7 && p.extLen == 300 && p.pointer == 700, "long extension");
    // truncated headers are refused
    CHECK(!parseBbHeader(b.data(), 3, p) && !parseBbHeader(b.data(), 0, p), "truncated header");

    // a whole packet: header, payload, padding
    std::vector<uint8_t> payload(5000);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i * 7 + 3);
    int used = 0;
    auto pk = makeBbPacket(1056, payload, 0, -1, &used);
    CHECK(pk.size() == 1056 && used == 1055 && pk[0] == 0 && pk[1] == payload[0], "packet without counter");
    pk = makeBbPacket(1056, payload, 10, 77, &used);
    CHECK(parseBbHeader(pk.data(), (int)pk.size(), p) && p.counter == 77 && p.pointer == 10 && used == 1056 - p.headerBytes, "packet with counter");
    // a short payload: the padding goes into the header's extension field and the payload ends the packet
    std::vector<uint8_t> small(10, 0xAB);
    pk = makeBbPacket(100, small, 0, -1, &used);
    CHECK(used == 10 && pk.size() == 100 && parseBbHeader(pk.data(), (int)pk.size(), p) && p.headerBytes == 90 && p.extType == 7 && pk[90] == 0xAB && pk[99] == 0xAB, "padding in the extension field");
    pk = makeBbPacket(100, small, 0, 5, &used);
    CHECK(parseBbHeader(pk.data(), (int)pk.size(), p) && p.headerBytes == 90 && p.counter == 5 && pk[99] == 0xAB, "counter and padding");
    std::vector<uint8_t> mid(40, 0xCD);
    pk = makeBbPacket(100, mid, 0, -1, &used);
    CHECK(parseBbHeader(pk.data(), (int)pk.size(), p) && p.headerBytes == 60 && p.ofi == 2 && used == 40, "long extension padding");
    std::vector<uint8_t> almost(98, 1);
    pk = makeBbPacket(100, almost, 0, -1, &used);
    CHECK(used == 98 && parseBbHeader(pk.data(), (int)pk.size(), p) && p.headerBytes == 2, "two byte header when two bytes are missing");

    // scrambling is its own inverse and the sequence is balanced
    auto bits = bytesToBits(pk);
    auto orig = bits;
    bbScramble(bits);
    CHECK(bits != orig, "scrambling changes the bits");
    bbScramble(bits);
    CHECK(bits == orig, "scrambling twice restores the bits");
    auto seq = bbScrambleSequence(65535);
    int ones = 0;
    for (auto v : seq) ones += v;
    CHECK(ones > 32000 && ones < 33500, "scrambling sequence is balanced");
    CHECK(bytesToBits(bitsToBytes(orig)) == orig, "bits and bytes");
    printf(fails ? "atsc3 bb: FAILED\n" : "atsc3 bb: ok\n");
    return fails ? 1 : 0;
}
