// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT layers: convolutional code and Viterbi, the sync word as other decoders know it, the CCSDS PN, Reed-Solomon (255,223)
// dual basis, CADU round trip with errors, M_PDU packet reassembly across VCDUs and a lost VCDU, the MSU-MR JPEG-like strip coder.
#include "dect2/meteor/meteor_proto.h"
#include "dect2/common/stable_random.h"
#include "dect2/meteor/meteor_gen.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
using namespace dect2::meteor;
#include "../common/test_util.h"

int main() {
    std::mt19937 rng(7);
    // the encoded sync word: with every code bit inverted (the other polarity) it is the 0xFCA2B63DB00D9794 the open decoders correlate with
    CHECK(~encodedAsm() == 0xFCA2B63DB00D9794ull, "encoded ASM %016llx", (unsigned long long)encodedAsm());
    // PN
    uint8_t pn[8] = {};
    pnXor(pn, 8);
    CHECK(pn[0] == 0xFF && pn[1] == 0x48 && pn[2] == 0x0E && pn[3] == 0xC0 && pn[4] == 0x9A, "pn %02x %02x %02x %02x", pn[0], pn[1], pn[2], pn[3]);
    // Viterbi: 4000 bits through the code with noise
    {
        std::vector<uint8_t> in(4000);
        for (auto& b : in) b = rng() & 1;
        ConvEncoder e;
        Viterbi27 v;
        dect2::normal_distribution<float> nd(0, 0.7f);
        int raw = 0;
        for (uint8_t b : in) {
            uint8_t a, c;
            e.push(b, a, c);
            const float sa = (a ? 1.f : -1.f) + nd(rng), sc = (c ? 1.f : -1.f) + nd(rng);
            raw += (sa > 0) != (a != 0); raw += (sc > 0) != (c != 0);
            v.push((int)std::lround(sa * 30), (int)std::lround(sc * 30));
        }
        v.flush();
        int err = 0;
        for (size_t i = 0; i < in.size() && i < v.out().size(); i++) err += v.out()[i] != in[i];
        printf("  viterbi: %d raw errors, %d after\n", raw, err);
        CHECK(v.out().size() == in.size() && raw > 300 && err < 10, "viterbi %zu bits, %d errors", v.out().size(), err);
    }
    // RS
    // independent vector: the CCSDS 131.0-B generator polynomial g(x) (conventional basis), coefficients G0..G16 as powers of alpha
    // (x^8+x^7+x^2+x+1), symmetric; x^32 mod g(x) = g(x) - x^32, which is what the parity of the data 0, ..., 0, 1 is.
    {
        static const int G[17] = {0, 249, 59, 66, 4, 43, 126, 251, 97, 30, 3, 213, 50, 66, 170, 5, 24};
        uint8_t ex[255]; int x = 1;
        for (int i = 0; i < 255; i++) { ex[i] = (uint8_t)x; x <<= 1; if (x & 0x100) x ^= 0x187; }
        uint8_t d[223] = {}, par[32];
        d[222] = 1;
        rsEncode(d, par, false);
        bool ok = true;
        for (int j = 0; j < 32; j++) { const int k = 31 - j; if (par[j] != ex[G[k <= 16 ? k : 32 - k]]) ok = false; }
        CHECK(ok, "RS generator matches CCSDS 131.0-B");
    }
    // the CCSDS sync marker and randomiser (131.0-B Annex): sequence 0xFF 0x48 0x0E 0xC0 0x9A 0x0D 0x70 0xBC 0x8E 0x2C 0x93 0xAD
    {
        uint8_t z[12] = {};
        pnXor(z, 12);
        static const uint8_t e[12] = {0xFF, 0x48, 0x0E, 0xC0, 0x9A, 0x0D, 0x70, 0xBC, 0x8E, 0x2C, 0x93, 0xAD};
        CHECK(!std::memcmp(z, e, 12), "PN sequence");
    }
    {
        int okAll = 1;
        for (int trial = 0; trial < 50; trial++) {
            uint8_t d[223], cw[255];
            for (auto& x : d) x = (uint8_t)rng();
            std::memcpy(cw, d, 223);
            rsEncode(d, cw + 223, false);
            uint8_t good[255];
            std::memcpy(good, cw, 255);
            CHECK(rsDecode(cw, false) == 0, "clean codeword");
            const int ne = trial % 17;
            for (int k = 0; k < ne; k++) cw[rng() % 255] ^= (uint8_t)(1 + rng() % 255);
            const int r = rsDecode(cw, false);
            if (ne <= 16 && (r < 0 || std::memcmp(cw, good, 255))) okAll = 0;
        }
        CHECK(okAll, "RS corrects up to 16 errors");
        uint8_t d[223], cw[255];
        for (auto& x : d) x = (uint8_t)rng();
        std::memcpy(cw, d, 223); rsEncode(d, cw + 223, false);
        for (int k = 0; k < 40; k++) cw[(k * 6) % 255] ^= 0x5A;
        CHECK(rsDecode(cw, false) < 0, "40 errors reported as failure");
    }
    // CADU with errors
    {
        uint8_t v[kVcduBytes], cadu[kCaduBytes], out[kVcduBytes];
        for (auto& x : v) x = (uint8_t)rng();
        encodeCadu(v, cadu, false);
        CHECK(cadu[0] == 0x1A && cadu[3] == 0x1D, "ASM");
        for (int k = 0; k < 50; k++) cadu[4 + rng() % kCodedBytes] ^= 0xFF;
        CHECK(decodeCadu(cadu + 4, out, false) > 0 && !std::memcmp(out, v, sizeof v), "CADU round trip with 50 byte errors");
    }
    // packets across VCDUs, one VCDU lost
    {
        PacketMux mux;
        std::vector<std::vector<uint8_t>> sent;
        for (int i = 0; i < 40; i++) { std::vector<uint8_t> u(50 + (i * 97) % 900); for (auto& x : u) x = (uint8_t)rng(); sent.push_back(u); mux.add(64 + i % 3, i, u); }
        mux.flush();
        std::vector<std::vector<uint8_t>> vcdus;
        uint8_t v[kVcduBytes];
        while (mux.next(v, 0x9C, 5)) vcdus.emplace_back(v, v + kVcduBytes);
        PacketDemux d;
        std::vector<Packet> got;
        d.onPacket = [&](const Packet& p) { got.push_back(p); };
        for (auto& b : vcdus) { Vcdu vc; CHECK(parseVcdu(b.data(), vc), "vcdu"); d.push(vc); }
        bool same = got.size() == sent.size();
        for (size_t i = 0; same && i < got.size(); i++) same = got[i].data == sent[i] && got[i].seq == (int)i && got[i].apid == 64 + (int)i % 3;
        CHECK(same, "packets: %zu of %zu", got.size(), sent.size());
        got.clear();
        PacketDemux d2;
        d2.onPacket = [&](const Packet& p) { got.push_back(p); };
        for (size_t k = 0; k < vcdus.size(); k++) { if (k == 5) continue; Vcdu vc; parseVcdu(vcdus[k].data(), vc); d2.push(vc); }
        bool ok = got.size() < sent.size() && got.size() + 5 >= sent.size();
        for (auto& p : got) ok = ok && p.data == sent[(size_t)p.seq];
        CHECK(ok && d2.dropped > 0, "lost VCDU: %zu packets, all intact", got.size());
    }
    // image strip coder
    {
        uint8_t px[8][112];
        for (int y = 0; y < 8; y++) for (int x = 0; x < 112; x++) px[y][x] = meteorTestPixel(2, 300 + x, 40 + y);
        const auto u = encodeImagePacket(px, 28, 80, 20000, 1234567);
        McuStrip s;
        CHECK(decodeImagePacket(u, s) && s.mcu == 28 && s.q == 80 && s.ms == 1234567, "strip decodes");
        double se = 0;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 112; x++) se += (s.px[y][x] - px[y][x]) * (double)(s.px[y][x] - px[y][x]);
        const double psnr = 10 * std::log10(255.0 * 255.0 / std::max(1e-9, se / (8 * 112)));
        printf("  strip: %zu bytes, PSNR %.1f dB\n", u.size(), psnr);
        CHECK(psnr > 30, "PSNR %.1f", psnr);
        auto bad = u; bad.resize(20);
        CHECK(!decodeImagePacket(bad, s), "truncated packet rejected");
    }
    // issue #79: picture rows over lost lock. 43 packets a row (14 per channel, three channels, one telemetry) on one 14-bit counter,
    // 1230 ms a row on board. A gap longer than half the counter's range, and one where it wraps, must skip the rows lost.
    {
        struct Gap { const char* name; int lostRows; };
        for (const Gap& g : {Gap{"short gap", 7}, Gap{"long gap (12000 packets)", 279}, Gap{"counter wrapped (16384 + 214 packets)", 386}}) {
            StripRows rows;
            int seq = 0, bad = 0, last = -1;
            uint32_t ms = 86400000 - 30000;   // across midnight too
            auto row = [&](int r, bool send) {
                for (int c = 0; c < 3; c++)
                    for (int k = 0; k < 14; k++) {
                        if (send) { last = rows.place(c, seq, k, ms); if (last != r) bad++; }
                        seq = (seq + 1) & 0x3FFF;
                    }
                seq = (seq + 1) & 0x3FFF;   // telemetry
                ms = (ms + 1230) % 86400000;
            };
            int r = 0;
            for (; r < 50; r++) row(r, true);
            for (int i = 0; i < g.lostRows; i++, r++) row(r, false);
            for (const int e = r + 30; r < e; r++) row(r, true);
            CHECK(bad == 0 && last == r - 1 && std::fabs(rows.rowMs() - 1230) < 1, "%s: %d packets in the wrong row, last row %d of %d", g.name, bad, last, r - 1);
        }
    }
    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
