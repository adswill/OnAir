// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CATS (#643): known values (CRC, half float), whisker build/parse, framing for several LDPC chunk sizes, and 2-FSK loopback
// at several carrier offsets and SNRs with every field checked.
#include "dect2/cats/cats.h"
#include <cmath>
#include <random>
using namespace dect2::cats;
#include "../common/test_util.h"

static std::vector<uint8_t> build(const std::string& comment, bool gps) {
    Builder b;
    b.addId("EB1ABC", 11, 7);
    b.addTime(1767225600ull);
    if (gps) b.addGps(52.25, -1.5, 30123.0, 5, 90.0, 12.5);
    if (!comment.empty()) b.addComment(comment);
    return b.semi;
}

static void checkPacket(const Packet& p, const std::string& comment, bool gps, const char* tag) {
    CHECK(p.hasId && p.callsign == "EB1ABC" && p.ssid == 11 && p.icon == 7, "%s: identification", tag);
    CHECK(p.hasTime && p.unixTime == 1767225600ull, "%s: timestamp", tag);
    CHECK(p.comment == comment, "%s: comment (%zu bytes)", tag, p.comment.size());
    CHECK(p.hasGps == gps, "%s: gps present", tag);
    if (gps) {
        CHECK(std::fabs(p.lat - 52.25) < 1e-6 && std::fabs(p.lon + 1.5) < 1e-6, "%s: position %f %f", tag, p.lat, p.lon);
        CHECK(std::fabs(p.altM - 30128.0) < 1.0, "%s: altitude %f (half float of 30123)", tag, p.altM);
        CHECK(p.errM == 5 && std::fabs(p.headingDeg - 90.0) < 1.5 && std::fabs(p.speedMps - 12.5) < 0.01, "%s: error/heading/speed", tag);
    }
}

int main() {
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc16(digits, 9) == 0x906E, "CRC-16/SDLC check value %04x", crc16(digits, 9));
    CHECK(floatToHalf(1.0f) == 0x3C00 && floatToHalf(-2.0f) == 0xC000 && halfToFloat(0x3C00) == 1.0f && halfToFloat(0xC000) == -2.0f, "half float");

    // libCATS writes latitude and longitude as little-endian int32 = degrees * 2^31 / 90 (or / 180), north and east positive: its
    // lat_to_int32 negates a value scaled by (float)(1 << 31), which is -2^31, so the two signs cancel (truncating, we round: 1 LSB).
    // Pin 52.25 N, 1.5 W at the whisker (type 2, length 14).
    {
        const auto s = build("", true);
        size_t at = 0; while (at + 14 < s.size() && !(s[at] == 2 && s[at + 1] == 14)) at++;
        CHECK(at + 14 < s.size(), "gps whisker found");
        auto rd = [&](size_t o) { return (int32_t)((uint32_t)s[o] | (uint32_t)s[o + 1] << 8 | (uint32_t)s[o + 2] << 16 | (uint32_t)s[o + 3] << 24); };
        CHECK(std::llabs((long long)rd(at + 2) - 0x4a4fa4fa) <= 1, "latitude raw %08x (north positive)", (unsigned)rd(at + 2));
        CHECK(std::llabs((long long)rd(at + 6) - (int32_t)0xfeeeeeef) <= 1 && rd(at + 6) < 0, "longitude raw %08x (west negative)", (unsigned)rd(at + 6));
    }

    // body round trips for sizes that pick different LDPC codes (8, 16, 32, 128 and 512 byte chunks)
    for (size_t n : {0u, 20u, 60u, 200u, 700u, 1500u}) {
        std::string c; for (size_t i = 0; i < n; i++) c += (char)('a' + i % 26);
        const bool gps = n != 60;
        Packet p;
        CHECK(decodeBody(encodeBody(build(c, gps)), p), "body decode, comment %zu", n);
        checkPacket(p, c, gps, "body");
    }
    // a corrupted data byte must fail the CRC
    {
        auto body = encodeBody(build("hello", true));
        body[3] ^= 0x10;
        Packet p;
        CHECK(!decodeBody(body, p) || p.comment != "hello", "a flipped bit is not accepted as the same packet");
    }
    // radio loopback
    const double fs = 48000;
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double offs[] = {0, 1500, -2200};
    const double snrs[] = {40, 25, 18};
    for (double off : offs) for (double snr : snrs) {
        const std::string c = "Balloon flight 4, 30 km, all fine";
        auto iq = modulate(encodeFrame(build(c, true)), fs, off);
        std::vector<std::complex<float>> cap(3000, {0.f, 0.f});
        cap.insert(cap.end(), iq.begin(), iq.end());
        const float sg = (float)std::sqrt(std::pow(10.0, -snr / 10.0) / 2.0);
        for (auto& x : cap) x += std::complex<float>(sg * nd(rng), sg * nd(rng));
        const auto pk = demodulate(cap, fs);
        char tag[64]; snprintf(tag, sizeof tag, "radio off %.0f snr %.0f", off, snr);
        CHECK(pk.size() == 1, "%s: %zu packets", tag, pk.size());
        if (pk.size() == 1) checkPacket(pk[0], c, true, tag);
    }
    return testSummary("cats_loop");
}
