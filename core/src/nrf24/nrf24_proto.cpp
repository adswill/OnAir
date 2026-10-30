// nRF24L01 Enhanced ShockBurst coding and the address sniffer, see nrf24_proto.h
#include "dect2/nrf24/nrf24_proto.h"
#include <algorithm>

namespace dect2 {
namespace nrf24 {

uint16_t crc16(const uint8_t* bits, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        const bool top = (c & 0x8000) != 0;
        c = (uint16_t)(c << 1);
        if (top != (bits[i] != 0)) c ^= 0x1021;
    }
    return c;
}
uint8_t crc8(const uint8_t* bits, size_t n) {
    uint8_t c = 0xFF;
    for (size_t i = 0; i < n; i++) {
        const bool top = (c & 0x80) != 0;
        c = (uint8_t)(c << 1);
        if (top != (bits[i] != 0)) c ^= 0x07;
    }
    return c;
}

static void putBits(std::vector<uint8_t>& v, uint32_t x, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)((x >> i) & 1)); }

std::vector<uint8_t> encode(const Frame& f) {
    std::vector<uint8_t> b;
    if (f.address.size() < (size_t)kMinAddr || f.address.size() > (size_t)kMaxAddr || f.payload.size() > (size_t)kMaxPayload) return b;
    const int first = f.address[0] >> 7;
    putBits(b, first ? 0xAA : 0x55, 8);
    const size_t body = b.size();
    for (uint8_t a : f.address) putBits(b, a, 8);
    putBits(b, (uint32_t)f.payload.size(), 6);
    putBits(b, (uint32_t)(f.pid & 3), 2);
    b.push_back(f.noAck ? 1 : 0);
    for (uint8_t p : f.payload) putBits(b, p, 8);
    if (f.crcBits == 8) putBits(b, crc8(b.data() + body, b.size() - body), 8);
    else putBits(b, crc16(b.data() + body, b.size() - body), 16);
    return b;
}

static uint32_t getBits(const uint8_t* b, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 1 | b[i]; return v; }

std::vector<Candidate> sniff(const std::vector<uint8_t>& bits) {
    std::vector<Candidate> out;
    const size_t N = bits.size();
    std::vector<uint16_t> c16;   // the CRC-16 and CRC-8 after every bit counted from the start being tried
    std::vector<uint8_t> c8;
    for (size_t s = 1; s + 8 * kMinAddr + 9 + 8 <= N && s <= 16; s++) {
        size_t r = 1;
        while (r < s && bits[s - 1 - r] != bits[s - r]) r++;   // bits[s-r-1..s-1] alternate
        if (r < 6) continue;   // the preamble is 8 alternating bits (the first may be lost to the slicer)
        if (bits[s] == bits[s - 1]) continue;   // and it is chosen to alternate into the first bit of the address
        // the longest frame from here: 5 address bytes, control field, 32 payload bytes, 2 CRC bytes
        const size_t maxEnd = std::min(N, s + 8 * (size_t)kMaxAddr + 9 + 8 * (size_t)kMaxPayload + 16);
        c16.assign(maxEnd - s + 1, 0); c8.assign(maxEnd - s + 1, 0);
        uint16_t a = 0xFFFF; uint8_t b8 = 0xFF;
        c16[0] = a; c8[0] = b8;
        for (size_t i = s; i < maxEnd; i++) {
            const bool bit = bits[i] != 0;
            const bool t16 = (a & 0x8000) != 0, t8 = (b8 & 0x80) != 0;
            a = (uint16_t)(a << 1); b8 = (uint8_t)(b8 << 1);
            if (t16 != bit) a ^= 0x1021;
            if (t8 != bit) b8 ^= 0x07;
            c16[i - s + 1] = a; c8[i - s + 1] = b8;
        }
        for (int L = kMaxAddr; L >= kMinAddr; L--) {
            const size_t pcf = s + 8 * (size_t)L;
            if (pcf + 9 > N) continue;
            const int len = (int)getBits(&bits[pcf], 6);
            if (len > kMaxPayload) continue;
            const size_t end = pcf + 9 + 8 * (size_t)len;
            for (int cb = 16; cb >= 8; cb -= 8) {
                if (end + (size_t)cb > N) continue;
                const bool ok = cb == 16 ? c16[end - s] == (uint16_t)getBits(&bits[end], 16) : c8[end - s] == (uint8_t)getBits(&bits[end], 8);
                if (!ok) continue;
                Candidate c;
                for (int i = 0; i < L; i++) c.frame.address.push_back((uint8_t)getBits(&bits[s + 8 * (size_t)i], 8));
                c.frame.pid = (int)getBits(&bits[pcf + 6], 2);
                c.frame.noAck = bits[pcf + 8] != 0;
                for (int i = 0; i < len; i++) c.frame.payload.push_back((uint8_t)getBits(&bits[pcf + 9 + 8 * (size_t)i], 8));
                c.frame.crcBits = cb;
                c.frame.startBit = (int)s;
                c.weak = cb == 8;
                out.push_back(std::move(c));
            }
        }
    }
    // a strong (CRC-16) frame is kept once per place; a CRC-8 candidate is dropped when it overlaps a strong frame and otherwise kept as it is: a chance
    // match is told from a real frame by the repeat the receiver asks for
    auto span = [](const Candidate& c, int& s0, int& e0) { s0 = c.frame.startBit; e0 = s0 + 8 * (int)c.frame.address.size() + 9 + 8 * (int)c.frame.payload.size() + c.frame.crcBits; };
    std::vector<Candidate> res;
    for (auto& c : out) {
        if (c.weak) continue;
        int cs, ce, rs, re;
        span(c, cs, ce);
        bool dup = false;
        for (auto& r : res) { span(r, rs, re); if (cs < re && rs < ce) { dup = true; break; } }
        if (!dup) res.push_back(c);
    }
    const size_t nStrong = res.size();
    for (auto& c : out) {
        if (!c.weak) continue;
        int cs, ce, rs, re;
        span(c, cs, ce);
        bool over = false;
        for (size_t i = 0; i < nStrong; i++) { span(res[i], rs, re); if (cs < re && rs < ce) { over = true; break; } }
        if (!over) res.push_back(c);
    }
    return res;
}

std::string hex(const std::vector<uint8_t>& b) {
    static const char* h = "0123456789ABCDEF";
    std::string s;
    for (uint8_t x : b) { s += h[x >> 4]; s += h[x & 15]; }
    return s;
}
std::string printable(const std::vector<uint8_t>& b) {
    std::string s;
    for (uint8_t x : b) s += (x >= 32 && x < 127) ? (char)x : '.';
    return s;
}

} // namespace nrf24
} // namespace dect2
