// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Voice slot content (frame layer): the toolkit's order CRC24 -> RS6 -> RS8 -> zero-sum -> vocoder. Known answers for the two Reed-Solomon codes
// come from the toolkit's own rs.py / rs6.py parameters run through the reedsolo library (rs_encode_msg); a vocoder frame must stay "VOC"
// (what the codec gets), the data kinds must not, and data slots must never reach the codec.
#include "dect2/iridium/iridium_voice.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static IridiumBurstBits slot(const std::vector<uint8_t>& bitsMsbFirst) {   // 312 bits in the toolkit's order -> a burst
    IridiumBurstBits b;
    b.bits = iridiumBuildLcwFrame(0, 0, 0, bitsMsbFirst);
    b.downlink = true; b.freqHz = 1.62e9;
    return b;
}
static void bytesMsb(std::vector<uint8_t>& bits, const std::vector<uint8_t>& by) {
    for (uint8_t x : by) for (int i = 7; i >= 0; i--) bits.push_back((x >> i) & 1);
}

int main() {
    // RS8: message 1..31, checks from reedsolo (prim 0x11d, fcr 0, 16 symbols, the first 8 sent)
    std::vector<uint8_t> by;
    for (int i = 1; i <= 31; i++) by.push_back(uint8_t(i));
    for (uint8_t c : {0xa4, 0x66, 0x38, 0x20, 0x45, 0xf0, 0x89, 0xcf}) by.push_back(c);
    std::vector<uint8_t> bits;
    bytesMsb(bits, by);
    IridiumBurstBits b = slot(bits);
    IridiumFrame f = decodeIridiumBurst(b);
    CHECK(f.type == IridiumType::Voice && f.vtype == "VOD", "RS8 slot classified %s/%s", f.typeName.c_str(), f.vtype.c_str());
    bits[5] ^= 1;   // one bit error: no longer an exact code word -> a vocoder frame
    f = decodeIridiumBurst(slot(bits));
    CHECK(f.vtype == "VOC", "damaged slot classified %s", f.vtype.c_str());

    // RS6: message 1..42 (six bit symbols), checks from reedsolo (prim 0x43, fcr 54, 10 symbols)
    std::vector<uint8_t> b6;
    auto sym6 = [&](int v) { for (int i = 5; i >= 0; i--) b6.push_back((v >> i) & 1); };
    for (int i = 1; i <= 42; i++) sym6(i);
    for (int c : {28, 41, 47, 30, 46, 12, 61, 11, 9, 50}) sym6(c);
    b6.resize(312, 0);
    f = decodeIridiumBurst(slot(b6));
    CHECK(f.vtype == "VO6", "RS6 slot classified %s", f.vtype.c_str());

    // zero-sum: bytes 35..37 zero, all 39 bytes add up to 0 mod 256
    std::vector<uint8_t> z;
    for (int i = 0; i < 35; i++) z.push_back(uint8_t(0x11 + 7 * i));
    unsigned sum = 0;
    for (uint8_t x : z) sum += x;
    z.push_back(0); z.push_back(0); z.push_back(0); z.push_back(uint8_t((256 - (sum & 255)) & 255));
    std::vector<uint8_t> zb;
    bytesMsb(zb, z);
    f = decodeIridiumBurst(slot(zb));
    CHECK(f.vtype == "VOZ", "zero-sum slot classified %s", f.vtype.c_str());

    // random vocoder-like frames stay VOC (false data match is about 2^-64)
    int voc = 0;
    for (uint32_t s = 1; s <= 200; s++) {
        IridiumBurstBits v; v.bits = iridiumBuildVoice([&] { std::vector<uint8_t> p(39); uint32_t x = s * 2654435761u; for (auto& q : p) { x = x * 1664525u + 1013904223u; q = uint8_t(x >> 24); } p[0] |= 1; p[36] |= 1; return p; }());
        v.downlink = true; v.freqHz = 1.62e9;
        if (decodeIridiumBurst(v).vtype == "VOC") voc++;
    }
    CHECK(voc == 200, "%d of 200 random frames stayed VOC", voc);

    // data slots are counted and never reach a codec
    IridiumVoice v;
    v.setSilent(true);
    bits.assign(0, 0); bytesMsb(bits, by);
    for (int i = 0; i < 6; i++) { IridiumBurstBits d = slot(bits); d.timeSec = 1.0 + 0.09 * i; v.feed(d, decodeIridiumBurst(d)); }
    v.closeAll();
    const auto c = v.calls();
    CHECK(c.size() == 1 && c[0].dataSlots == 6 && c[0].readable == 0 && v.framesToCodec() == 0, "data slots: %zu calls, %u data, %llu to codec",
          c.size(), c.empty() ? 0u : c[0].dataSlots, (unsigned long long)v.framesToCodec());
    CHECK(!c.empty() && c[0].status == "data", "data call status %s", c.empty() ? "-" : c[0].status.c_str());
    return testSummary("iridium vslot");
}
