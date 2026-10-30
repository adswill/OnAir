// nRF24 Enhanced ShockBurst coding and the sniffer: CRC reference values, every address length / payload length / CRC size / PID / NO_ACK
// encoded and found again from bits with junk before and after, damaged frames refused, no strong frame in random bits, CRC-8 candidates marked weak.
#include "dect2/nrf24/nrf24_proto.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> bitsOf(const std::string& s) {
    std::vector<uint8_t> b;
    for (unsigned char c : s) for (int i = 7; i >= 0; i--) b.push_back((c >> i) & 1);
    return b;
}
// independent byte-wise references
static uint16_t refCrc16(const std::string& s) {
    uint16_t c = 0xFFFF;
    for (unsigned char ch : s) { c ^= (uint16_t)(ch << 8); for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
    return c;
}
static uint8_t refCrc8(const std::string& s) {
    uint8_t c = 0xFF;
    for (unsigned char ch : s) { c ^= ch; for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1); }
    return c;
}

int main() {
    const std::vector<uint8_t> nine = bitsOf("123456789");
    CHECK(nrf24::crc16(nine.data(), nine.size()) == 0x29B1, "CRC-16/CCITT-FALSE check value 0x%04X", nrf24::crc16(nine.data(), nine.size()));
    CHECK(nrf24::crc16(nine.data(), nine.size()) == refCrc16("123456789"), "crc16 against the byte-wise reference");
    CHECK(nrf24::crc8(nine.data(), nine.size()) == refCrc8("123456789"), "crc8 against the byte-wise reference");

    std::mt19937 rng(7);
    int found = 0, tried = 0;
    for (int L = 3; L <= 5; L++)
        for (int len : {0, 1, 5, 17, 31, 32})
            for (int crc : {16, 8})
                for (int pid = 0; pid < 4; pid++) {
                    nrf24::Frame f;
                    for (int i = 0; i < L; i++) f.address.push_back((uint8_t)rng());
                    if (pid == 0) f.address[0] |= 0x80; else if (pid == 1) f.address[0] &= 0x7F;
                    for (int i = 0; i < len; i++) f.payload.push_back((uint8_t)rng());
                    f.pid = pid; f.noAck = (pid & 1) != 0; f.crcBits = crc;
                    std::vector<uint8_t> bits = nrf24::encode(f);
                    CHECK(bits.size() == (size_t)(8 + 8 * L + 9 + 8 * len + crc), "frame size L%d len%d crc%d: %zu", L, len, crc, bits.size());
                    // junk before (not alternating long enough to matter) and after
                    std::vector<uint8_t> in;
                    const int junk = (int)(rng() % 7);
                    for (int i = 0; i < junk; i++) in.push_back((uint8_t)(i % 3 == 0));
                    in.insert(in.end(), bits.begin(), bits.end());
                    for (int i = 0; i < 20; i++) in.push_back((uint8_t)(rng() & 1));
                    const auto r = nrf24::sniff(in);
                    tried++;
                    bool ok = false;
                    for (const auto& c : r) if (c.frame.address == f.address && c.frame.payload == f.payload && c.frame.pid == f.pid && c.frame.noAck == f.noAck && c.frame.crcBits == crc && c.weak == (crc == 8)) ok = true;
                    if (ok) found++; else CHECK(false, "frame not found: L%d len%d crc%d pid%d (%zu candidates)", L, len, crc, pid, r.size());
                }
    printf("  %d of %d frames found\n", found, tried);

    // damaged: any single bit error in the frame body is refused as that frame
    {
        nrf24::Frame f; f.address = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7}; f.payload = {'h', 'i', '!', 1, 2, 3}; f.crcBits = 16; f.pid = 2;
        const std::vector<uint8_t> good = nrf24::encode(f);
        int accepted = 0;
        for (size_t i = 8; i < good.size(); i++) {
            std::vector<uint8_t> bad = good;
            bad[i] ^= 1;
            for (const auto& c : nrf24::sniff(bad)) if (c.frame.address == f.address && c.frame.payload == f.payload && !c.weak) accepted++;
        }
        CHECK(accepted == 0, "%d damaged copies accepted", accepted);
        // the exact frame again, extra bits in front that continue the preamble
        std::vector<uint8_t> lead = {1, 0, 1, 0, 1, 0};
        lead.insert(lead.end(), good.begin(), good.end());
        bool ok = false;
        for (const auto& c : nrf24::sniff(lead)) if (c.frame.address == f.address && c.frame.payload == f.payload) ok = true;
        CHECK(ok, "a longer preamble run is fine");
        // a slipped bit inside: refused
        std::vector<uint8_t> slip = good;
        slip.erase(slip.begin() + 40);
        int a2 = 0;
        for (const auto& c : nrf24::sniff(slip)) if (c.frame.payload == f.payload && !c.weak) a2++;
        CHECK(a2 == 0, "a lost bit is not accepted");
    }
    // random bits: no strong (CRC-16) frame in 20000 bursts of 200 bits starting with a preamble; CRC-8 candidates are rare per burst
    {
        int strong = 0, weak = 0;
        for (int k = 0; k < 20000; k++) {
            std::vector<uint8_t> b = {1, 0, 1, 0, 1, 0, 1, 0};
            for (int i = 0; i < 192; i++) b.push_back((uint8_t)(rng() & 1));
            for (const auto& c : nrf24::sniff(b)) (c.weak ? weak : strong)++;
        }
        printf("  random bits: %d strong, %d weak candidates in 20000 bursts\n", strong, weak);
        CHECK(strong <= 3, "%d CRC-16 chance frames in random bits", strong);
        CHECK(weak < 12000, "weak candidates %d", weak);
    }
    // limits
    {
        nrf24::Frame f; f.address = {1, 2}; CHECK(nrf24::encode(f).empty(), "address too short");
        f.address = {1, 2, 3}; f.payload.assign(33, 0); CHECK(nrf24::encode(f).empty(), "payload too long");
        CHECK(nrf24::sniff({}).empty() && nrf24::sniff(std::vector<uint8_t>(30, 1)).empty(), "empty and constant input");
    }
    return testSummary("nrf24 proto");
}
