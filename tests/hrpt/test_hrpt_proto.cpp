// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT protocol layers: known-answer vectors written out here, independent of the code under test, where the standards give them
// (CCSDS ASM and the convolutional code's impulse response, the NOAA HRPT sync words, 10-bit packing), then round trips of the pieces.
#include "dect2/hrpt/hrpt_gen.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // --- the fast Viterbi against the Meteor one and the message, on a noisy random stream (soft values of random size, 4 % hard errors)
    {
        std::mt19937 rng(11);
        const int N = 20000;
        std::vector<uint8_t> msg((size_t)N);
        hrpt::CcsdsEncoder e;
        hrpt::Viterbi fast;
        meteor::Viterbi27 ref;
        for (int i = 0; i < N; i++) {
            msg[(size_t)i] = (uint8_t)(rng() & 1);
            uint8_t a, b;
            e.push(msg[(size_t)i], a, b);
            b ^= 1;   // the decoders have the code without the inversion
            int sa = (a ? 1 : -1) * (int)(20 + rng() % 100), sb = (b ? 1 : -1) * (int)(20 + rng() % 100);
            if (rng() % 100 < 4) sa = -sa;
            if (rng() % 100 < 4) sb = -sb;
            fast.push(sa, sb);
            ref.push(sa, sb);
        }
        fast.flush(); ref.flush();
        const auto& f = fast.out(); const auto& r = ref.out();
        int ef = 0, er = 0;
        for (size_t i = 0; i < f.size() && i < msg.size(); i++) ef += f[i] != msg[i];
        for (size_t i = 0; i < r.size() && i < msg.size(); i++) er += r[i] != msg[i];
        CHECK(f.size() == (size_t)N && r.size() == (size_t)N, "decoded %zu / %zu bits", f.size(), r.size());
        CHECK(ef <= er + 10 && ef < N / 100, "fast Viterbi: %d bit errors, Meteor's: %d", ef, er);
    }
    // --- CCSDS convolutional code (131.0-B): G1 = 171 octal, G2 = 133 octal with G2 inverted. The impulse response is the generator,
    // newest input bit at the first tap: one 1 then zeros gives G1 = 1111001 and ~G2 = ~1011011 = 0100100 on the two outputs.
    {
        hrpt::CcsdsEncoder e;
        const int g1[7] = {1, 1, 1, 1, 0, 0, 1}, g2n[7] = {0, 1, 0, 0, 1, 0, 0};
        for (int i = 0; i < 7; i++) {
            uint8_t a, b;
            e.push(i == 0, a, b);
            CHECK(a == g1[i] && b == g2n[i], "impulse response step %d: %d %d", i, a, b);
        }
        // a second, independent route: the polynomial arithmetic by hand over a random message (G1 / G2 as bit masks of the last 7 inputs)
        std::mt19937 rng(3);
        hrpt::CcsdsEncoder e2;
        int hist[7] = {};   // hist[0] newest
        int bad = 0;
        for (int i = 0; i < 2000; i++) {
            const int bit = (int)(rng() & 1);
            for (int k = 6; k > 0; k--) hist[k] = hist[k - 1];
            hist[0] = bit;
            int a = 0, b = 0;
            for (int k = 0; k < 7; k++) { a ^= hist[k] & g1[k]; b ^= hist[k] & (1 - g2n[k]); }
            b ^= 1;
            uint8_t ea, eb;
            e2.push(bit, ea, eb);
            bad += ea != a || eb != b;
        }
        CHECK(bad == 0, "%d code bits differ from the polynomial arithmetic", bad);
        // the encoded sync word of meteor_proto (G2 not inverted) equals ours with the second output flipped, from bit 12 on
        hrpt::CcsdsEncoder s;
        uint64_t mine = 0;
        for (int i = 0; i < 32; i++) {
            uint8_t a, b;
            s.push(0x1ACFFC1Du >> (31 - i) & 1, a, b);
            mine = mine << 2 | (uint64_t)a << 1 | (uint64_t)(b ^ 1);
        }
        const uint64_t mask = (1ull << 52) - 1;
        CHECK(((mine ^ meteor::encodedAsm()) & mask) == 0, "encoded sync word differs: %016llx vs %016llx", (unsigned long long)mine, (unsigned long long)meteor::encodedAsm());
    }
    // --- CADU sync word and CCSDS randomiser start (131.0-B: 1ACFFC1D; sequence FF 48 0E C0 9A 0D 70 BC)
    {
        CHECK(meteor::kAsm == 0x1ACFFC1Du, "ASM");
        uint8_t z[8] = {};
        meteor::pnXor(z, 8);
        const uint8_t pn[8] = {0xFF, 0x48, 0x0E, 0xC0, 0x9A, 0x0D, 0x70, 0xBC};
        CHECK(memcmp(z, pn, 8) == 0, "randomiser sequence");
    }
    // --- NOAA HRPT sync words (NOAA KLM User's Guide, minor frame): 0x284 0x16F 0x35C 0x19D 0x20F 0x095, 11090 words a frame, image from word 750
    {
        const uint16_t k[6] = {0x284, 0x16F, 0x35C, 0x19D, 0x20F, 0x095};
        for (int i = 0; i < 6; i++) CHECK(hrpt::kNoaaSync[i] == k[i], "sync word %d", i);
        CHECK(hrpt::kNoaaWords == 11090 && hrpt::kNoaaImageWord == 750, "frame constants");
        // the 60-bit number by hand: 1010000100 0101101111 ...
        CHECK((hrpt::noaaSync60() >> 40) == (0x284ull << 10 | 0x16F), "60-bit sync number");
        // 10-bit packing, MSB first: 0x284, 0x16F -> 1010000100 0101101111 -> A1 16 F0
        uint16_t w[2] = {0x284, 0x16F};
        uint8_t b[3] = {};
        hrpt::packWords(w, 2, b);
        CHECK(b[0] == 0xA1 && b[1] == 0x16 && b[2] == 0xF0, "packing: %02X %02X %02X", b[0], b[1], b[2]);
        uint16_t u[2] = {};
        hrpt::unpackWords(b, 2, u);
        CHECK(u[0] == 0x284 && u[1] == 0x16F, "unpacking");
    }
    // --- AVHRR packet: round trip, and the word positions the layout names (user byte 14 + word 55 + 5 x + channel)
    {
        hrpt::Line l;
        l.day = 23456; l.ms = 12345678;
        for (int c = 0; c < 5; c++) for (int x = 0; x < 2048; x++) l.px[(size_t)c * 2048 + (size_t)x] = hrptTestPixel(c, x, 7);
        const std::vector<uint8_t> u = hrpt::encodeAvhrrPacket(l);
        CHECK((int)u.size() == hrpt::kAvhrrUserBytes, "packet size %zu", u.size());
        hrpt::Line d;
        CHECK(hrpt::decodeAvhrrPacket(u, d), "decode");
        CHECK(d.day == l.day && d.ms == l.ms && d.px == l.px, "round trip");
        // pixel 3, channel 2 sits at 10-bit word 55 + 15 + 2 = 72 of the data area
        const size_t bit = (size_t)(hrpt::kAvhrrDataByte) * 8 + 72 * 10;
        unsigned v = 0;
        for (int k = 0; k < 10; k++) v = v << 1 | (u[(bit + (size_t)k) >> 3] >> (7 - ((bit + (size_t)k) & 7)) & 1u);
        CHECK(v == l.px[2 * 2048 + 3], "word position: %u vs %u", v, l.px[2 * 2048 + 3]);
        std::vector<uint8_t> shortp(100);
        CHECK(!hrpt::decodeAvhrrPacket(shortp, d), "short packet");
        // through VCDUs, Reed-Solomon, the randomiser and back
        meteor::PacketMux mux;
        mux.add(103, 5, u);
        mux.add(30, 6, std::vector<uint8_t>(100, 1));
        mux.add(103, 7, u);
        mux.add(104, 8, u);
        int got = 0, same = 0;
        meteor::PacketDemux dm;
        dm.onPacket = [&](const meteor::Packet& p) { got++; hrpt::Line x; if (p.apid >= 103 && hrpt::decodeAvhrrPacket(p.data, x)) same += x.px == l.px; };
        uint8_t v2[meteor::kVcduBytes], cadu[meteor::kCaduBytes];
        for (int i = 0; i < 40 && mux.next(v2, 11, 9); i++) {
            meteor::encodeCadu(v2, cadu);
            cadu[100] ^= 0x55; cadu[400] ^= 0xFF;   // damage a few bytes: RS repairs them
            uint8_t out[meteor::kVcduBytes];
            const int e = meteor::decodeCadu(cadu + 4, out);
            CHECK(e >= 2, "RS corrected %d", e);
            meteor::Vcdu vc;
            CHECK(meteor::parseVcdu(out, vc) && vc.scid == 11 && vc.vcid == 9, "VCDU header");
            dm.push(vc);
        }
        CHECK(got >= 3 && same >= 2, "%d packets, %d lines intact", got, same);
    }
    // --- NOAA minor frame: round trip; damaged sync words tolerated up to 12 bits, refused beyond
    {
        hrpt::Line l;
        l.day = 300; l.ms = 99999999 & 0x7FFFFFF;
        for (int c = 0; c < 5; c++) for (int x = 0; x < 2048; x++) l.px[(size_t)c * 2048 + (size_t)x] = hrptTestPixel(c, x, 11);
        std::vector<uint16_t> w(hrpt::kNoaaWords);
        hrpt::encodeNoaaFrame(l, w.data());
        hrpt::Line d;
        CHECK(hrpt::decodeNoaaFrame(w.data(), d) && d.px == l.px && d.day == l.day && d.ms == l.ms, "frame round trip: day %u ms %u", d.day, d.ms);
        w[0] ^= 0x155; w[3] ^= 0x2;   // 6 + 1 bits
        CHECK(hrpt::decodeNoaaFrame(w.data(), d), "7 sync bit errors accepted");
        w[1] ^= 0x3FF; w[2] ^= 0x3FF;
        CHECK(!hrpt::decodeNoaaFrame(w.data(), d), "many sync errors refused");
    }
    printf(fails ? "hrpt proto: %d FAILED\n" : "hrpt proto: all passed\n", fails);
    return fails ? 1 : 0;
}
