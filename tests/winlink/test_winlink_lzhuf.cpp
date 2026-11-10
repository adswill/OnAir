// Winlink LZHUF (B2 framing): the published wl2k-go vectors decode, and the encoder gives the same bytes; round trips of empty, short,
// repetitive, random and large (window wrap, frequency rescale) data; corrupted CRC, length and truncated data are refused.
#include "dect2/winlink/lzhuf.h"
#include "winlink_vectors.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2::winlink;
#include "../common/test_util.h"

int main() {
    // published vectors: decode, and encode bit-exact
    for (size_t i = 0; i < wlvec::kNumVecs; i++) {
        const wlvec::Vec& v = wlvec::kVecs[i];
        std::vector<uint8_t> out;
        std::string err;
        CHECK(lzhufDecompress(v.lzh, v.lzhLen, out, true, &err), "vector %zu decodes: %s", i, err.c_str());
        CHECK(out.size() == v.plainLen && !memcmp(out.data(), v.plain, v.plainLen), "vector %zu plain text", i);
        const std::vector<uint8_t> c = lzhufCompress(v.plain, v.plainLen, true);
        CHECK(c.size() == v.lzhLen && !memcmp(c.data(), v.lzh, v.lzhLen), "vector %zu encodes byte for byte (%zu vs %zu bytes)", i, c.size(), v.lzhLen);
    }
    // CRC of the B2 header, independent check: CRC-CCITT XMODEM of "123456789" is 0x31C3; the B2 form appends two zero bytes, which
    // for an augmented-message CRC gives the same value as the plain XMODEM CRC of the message
    {
        const char* s = "123456789";
        CHECK(b2Crc16((const uint8_t*)s, 9) == 0x31C3, "xmodem crc %04X", b2Crc16((const uint8_t*)s, 9));
    }
    // round trips
    std::mt19937 rng(7);
    std::vector<std::vector<uint8_t>> cases;
    cases.push_back({});
    cases.push_back({'A'});
    cases.push_back(std::vector<uint8_t>(5000, ' '));
    cases.push_back(std::vector<uint8_t>(70000, 'x'));    // long runs: the frequency table is rebuilt (MAX_FREQ)
    {
        std::vector<uint8_t> r(30000);
        for (auto& b : r) b = (uint8_t)rng();
        cases.push_back(r);                               // incompressible
    }
    {
        std::string t;
        while (t.size() < 200000) t += "Mid: ABCDEFGHIJKL\r\nSubject: test " + std::to_string(rng() % 1000) + "\r\nBody text with some words\r\n";
        cases.push_back(std::vector<uint8_t>(t.begin(), t.end()));   // larger than the window many times
    }
    for (size_t i = 0; i < cases.size(); i++) {
        for (int b2 = 0; b2 < 2; b2++) {
            const auto c = lzhufCompress(cases[i], b2 != 0);
            std::vector<uint8_t> out;
            std::string err;
            CHECK(lzhufDecompress(c.data(), c.size(), out, b2 != 0, &err) && out == cases[i], "round trip %zu b2 %d: %s", i, b2, err.c_str());
        }
    }
    CHECK(lzhufCompress(cases[0]).size() == 6, "empty input is 6 bytes (CRC + length)");
    // damage
    {
        const wlvec::Vec& v = wlvec::kVecs[wlvec::kNumVecs - 1];
        std::vector<uint8_t> c(v.lzh, v.lzh + v.lzhLen), out;
        for (size_t pos : {(size_t)0, (size_t)3, (size_t)100, c.size() - 1}) {
            auto d = c;
            d[pos] ^= 0x10;
            CHECK(!lzhufDecompress(d.data(), d.size(), out, true), "flip at %zu refused", pos);
        }
        auto t = c;
        t.resize(t.size() / 2);
        CHECK(!lzhufDecompress(t.data(), t.size(), out, true), "truncated refused");
        CHECK(!lzhufDecompress(c.data(), 3, out, true), "short header refused");
        // without the CRC a truncated stream still ends: unexpected end of data
        std::vector<uint8_t> nb(c.begin() + 2, c.begin() + 2 + (c.size() - 2) / 2);
        CHECK(!lzhufDecompress(nb.data(), nb.size(), out, false), "truncated (no CRC) refused");
    }
    return testSummary("winlink lzhuf");
}
