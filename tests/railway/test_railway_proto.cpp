#include "dect2/railway/railway_proto.h"
#include "dect2/railway/railway_gen.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2::railway;
static void checksum(std::vector<uint8_t>& f) {
    const auto c = crc16(f.data(), f.size() - 2); f[f.size() - 2] = uint8_t(c >> 8); f.back() = uint8_t(c);
}
static std::vector<uint8_t> frame(const std::vector<uint8_t>& payload) {
    auto f = demoFrame(Protocol::Klw); f.resize(11); f[1] = uint8_t(payload.size() + 11); f[10] = uint8_t(payload.size());
    f.insert(f.end(), payload.begin(), payload.end()); f.resize(f.size() + 2); checksum(f); return f;
}
int main() {
    CHECK(crc16((const uint8_t*)"123456789", 9) == 0x31C3, "CRC-16/XMODEM check vector");
    Message m; const auto t = demoFrame(Protocol::Tdcs), k = demoFrame(Protocol::Klw);
    CHECK(parseFrame(t, m) && m.protocol == Protocol::Tdcs && m.train == "T112" && m.locomotive == "HXD3D-0521", "TDCS identity");
    CHECK(m.speedKmh == 124 && m.kilometerValid && std::fabs(m.kilometer - 1161.011) < 1e-9 && m.driver == 123456 && m.station == 130 && m.carCount == 18 && m.length == 41, "TDCS fields");
    for (unsigned mileage : {9999888u, 8888888u, 9999999u, 0xFFFFFFu, 0xC0007Bu}) {
        auto f = t; for (int j = 0; j < 3; j++) f[25 + j] = uint8_t(mileage >> (j * 8)); checksum(f);
        CHECK(parseFrame(f, m), "mileage frame");
        if (mileage == 0xC0007B) CHECK(m.kilometerValid && m.increasingMileage && m.kilometer == -0.123, "signed mileage");
        else CHECK(!m.kilometerValid && m.testFrame == (mileage == 9999888 || mileage == 8888888), "sentinel mileage");
    }
    CHECK(parseFrame(k, m) && m.actionCode == 0x92 && m.pressureKpa == 486 && m.tailId == "123456" && m.locomotive == "HXD3C-0375", "KLW fields");
    CHECK(parseFrame(frame({0x12,0x34,0,0x24,0xFF,0xA5}), m) && m.actionCode == 0x24 && m.locomotive == "1234" && m.parameter.empty(), "six-byte variant");
    CHECK(parseFrame(frame({0x12,0x34,0,0x92,0x05,0x00,0x12,0x23,0x90,0x34,0x56,0,1,2,3}), m) && m.tailId == "123456" && m.locomotive == "HXD3C-1234" && m.pressureKpa == 500 && m.extra == "00010203", "15-byte variant");
    CHECK(parseFrame(frame({0x23,0x90,0x03,0x75,0,0xA1,0x12,0x34,0x56,0xFF,0xFF,0x26,0x10,0x20,0x12,0x34,0x56}), m) && m.clockSync == "2026-10-20T12:34:56", "clock variant");
    CHECK(parseFrame(frame({0x23,0x90,0x03,0x75,0,0xA2,0x12,0x34,0x56,0xFF,0xFF,0x26,0x02,0x30,0x25,0x61,0x56}), m) && m.clockSync.empty(), "invalid clock");
    for (size_t i = 0; i < t.size(); i++) { auto damaged = t; damaged[i] ^= 1; CHECK(!parseFrame(damaged, m), "CRC mutation %zu", i); }
    auto foreign = k; foreign[2] = 0x42; checksum(foreign); CHECK(!parseFrame(foreign, m), "foreign address");
    for (bool inverted : {false, true}) for (int errorCount : {0, 1, 2}) {
        auto bits = airBits(k, inverted);
        for (size_t at = 192 + 7 * 26; at < 192 + 8 * 26; at += 26) {
            if (errorCount) bits[at + 2] ^= 1;
            if (errorCount == 2) bits[at + 23] ^= 1;
        }
        FrameFinder finder; std::vector<Message> found; for (auto b : bits) finder.feed(b != 0, found);
        CHECK(found.size() == 1 && found[0].raw == k && found[0].correctedBits == errorCount, "BCH and polarity: %d %d (%zu frames)", inverted, errorCount, found.size());
    }
    for (size_t j = 0; j < 26; j++) {
        auto bits = airBits(t); bits[192 + 7 * 26 + j] ^= 1;
        FrameFinder f; std::vector<Message> found; for (auto b : bits) f.feed(b != 0, found);
        CHECK(found.size() == 1 && found[0].raw == t, "single BCH bit %zu", j);
        for (size_t other = 0; other < j; other++) {
            auto pair = airBits(t); pair[192 + 7 * 26 + j] ^= 1; pair[192 + 7 * 26 + other] ^= 1;
            FrameFinder two; std::vector<Message> corrected; for (auto b : pair) two.feed(b != 0, corrected);
            CHECK(corrected.size() == 1 && corrected[0].raw == t && corrected[0].correctedBits == 2, "double BCH bits %zu,%zu", j, other);
        }
    }
    FrameFinder finder; std::vector<Message> found; unsigned seed = 123;
    for (int i = 0; i < 200000; i++) { seed = seed * 1664525u + 1013904223u; finder.feed((seed >> 31) != 0, found); }
    CHECK(found.empty(), "noise emits nothing");
    for (auto b : airBits(t)) finder.feed(b != 0, found);
    CHECK(found.size() == 1 && found[0].raw == t, "recover after noise");
    FrameFinder recovery; found.clear(); auto bad = k; bad.back() ^= 1;
    for (auto b : airBits(bad)) recovery.feed(b != 0, found);
    CHECK(found.empty() && recovery.crcFailures > 0, "bad CRC rejected");
    for (auto b : airBits(k)) recovery.feed(b != 0, found);
    CHECK(found.size() == 1 && found[0].raw == k, "recover after bad CRC");
    // A valid header without the CIR sync and BCH words is not an air frame.
    FrameFinder raw; found.clear(); for (auto byte : k) for (int j = 7; j >= 0; j--) raw.feed(((byte >> j) & 1) != 0, found);
    CHECK(found.empty(), "no legacy raw-byte shortcut");
    return testSummary("test_railway_proto");
}
