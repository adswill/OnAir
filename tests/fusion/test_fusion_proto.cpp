// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu Fusion frame coding: sync, dibit mapping, Golay, convolutional code, FICH with CRC, DCH units, V/D mode 2 voice, whole frames of
// every data type, and error correction / detection.
#include "dect2/fusion/fusion_gen.h"
#include "dect2/fusion/fusion_proto.h"
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2::fusion;
#include "../common/test_util.h"

static int pop(uint32_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

int main() {
    // sync word D471C9634D: 20 symbols, first byte 0xD4 = 11 01 01 00 -> -3 +3 +3 +1
    int8_t s[kSyncDibits];
    syncLevels(s);
    CHECK(s[0] == -3 && s[1] == 3 && s[2] == 3 && s[3] == 1, "sync start %d %d %d %d", s[0], s[1], s[2], s[3]);
    for (int d = 0; d < 4; d++) CHECK(levelDibit(dibitLevel(d)) == d, "dibit %d", d);
    CHECK(dibitLevel(0) == 1 && dibitLevel(1) == 3 && dibitLevel(2) == -1 && dibitLevel(3) == -3, "Table 3-1 mapping");

    // Golay (24,12,8): the minimum weight is 8, three errors are corrected
    int minw = 99;
    for (uint32_t d = 1; d < 4096; d++) minw = std::min(minw, pop(golay24((uint16_t)d)));
    CHECK(minw == 8, "Golay minimum weight %d", minw);
    std::mt19937 rng(7);
    int goodG = 0;
    for (int i = 0; i < 400; i++) {
        const uint16_t d = (uint16_t)(rng() & 0xFFF);
        uint32_t w = golay24(d);
        for (int e = 0; e < 3; e++) w ^= 1u << (rng() % 24);   // up to three flips (two on the same bit cancel)
        uint16_t o = 0; int errs = 0;
        if (golay24Decode(w, o, errs) && o == d) goodG++;
    }
    CHECK(goodG == 400, "Golay corrected %d of 400", goodG);

    // CRC-16: x16+x12+x5+1, init 0, inverted result; a flipped bit changes it
    std::vector<uint8_t> bits(32);
    for (auto& b : bits) b = (uint8_t)(rng() & 1);
    const uint16_t c0 = crc16(bits.data(), 32);
    bits[9] ^= 1;
    CHECK(crc16(bits.data(), 32) != c0, "CRC unchanged by a flipped bit");
    std::vector<uint8_t> zero(16, 0);
    CHECK(crc16(zero.data(), 16) == 0xFFFF, "CRC of zeros is the inverted initial value, got %04X", crc16(zero.data(), 16));

    // whitening: PN(9,5) from 111001001: the first bits are 1 0 0 1 and the operation is its own inverse
    uint8_t w[40] = {};
    whiten(w, 40);
    CHECK(w[0] == 1 && w[1] == 0 && w[2] == 0 && w[3] == 1, "PN start %d%d%d%d", w[0], w[1], w[2], w[3]);
    whiten(w, 40);
    int nz = 0; for (uint8_t b : w) nz += b;
    CHECK(nz == 0, "whiten twice is not identity");

    // convolutional code and its decoder with errors
    std::vector<uint8_t> in(100);
    for (auto& b : in) b = (uint8_t)(rng() & 1);
    for (int i = 96; i < 100; i++) in[(size_t)i] = 0;
    std::vector<uint8_t> enc = convEncode(in);
    CHECK(enc.size() == 200, "coded size %zu", enc.size());
    int err = 0;
    CHECK(convDecode(enc, err) == in && err == 0, "clean decode");
    enc[11] ^= 1; enc[60] ^= 1; enc[150] ^= 1;
    CHECK(convDecode(enc, err) == in && err == 3, "decode with 3 errors (metric %d)", err);

    // interleaver inverse
    std::vector<uint8_t> v(200);
    for (auto& b : v) b = (uint8_t)(rng() & 1);
    CHECK(deinterleaveDibits(interleaveDibits(v, 5), 5) == v, "interleave 5");
    std::vector<uint8_t> v2(360);
    for (auto& b : v2) b = (uint8_t)(rng() & 1);
    CHECK(deinterleaveDibits(interleaveDibits(v2, 9), 9) == v2, "interleave 9");
    // read by columns: output 0 is input 0, output 1 is input 5 (rows of 5 dibits)
    std::vector<uint8_t> idx(200);
    for (int i = 0; i < 100; i++) { idx[(size_t)(2 * i)] = (uint8_t)((i >> 1) & 1); idx[(size_t)(2 * i + 1)] = (uint8_t)(i & 1); }
    const std::vector<uint8_t> il = interleaveDibits(idx, 5);
    CHECK(il.size() == 200 && il[2] == idx[10] && il[3] == idx[11], "interleave column order");

    // FICH: every field, clean
    Fich f;
    f.fi = 2; f.cs = 2; f.cm = 3; f.bn = 1; f.bt = 3; f.fn = 5; f.ft = 7; f.mr = 2; f.voip = 1; f.dt = 3; f.sqlOff = 0; f.sqlCode = 99;
    const std::vector<uint8_t> fe = fichEncode(f);
    CHECK(fe.size() == 200, "FICH is 200 bits");
    Fich g; int fixes = 0;
    CHECK(fichDecode(fe.data(), g, fixes) && fixes == 0, "FICH clean decode");
    CHECK(g.fi == 2 && g.cm == 3 && g.bn == 1 && g.bt == 3 && g.fn == 5 && g.ft == 7 && g.mr == 2 && g.voip == 1 && g.dt == 3 && g.sqlCode == 99, "FICH fields");
    // FICH error correction: 8 bit errors spread over the frame
    int fixedOk = 0;
    for (int t = 0; t < 200; t++) {
        std::vector<uint8_t> x = fe;
        for (int e = 0; e < 8; e++) x[(size_t)(e * 25 + (int)(rng() % 25))] ^= 1;
        if (fichDecode(x.data(), g, fixes) && g.sqlCode == 99 && g.dt == 3) fixedOk++;
    }
    CHECK(fixedOk >= 195, "FICH with 8 spread errors: %d of 200 decoded", fixedOk);
    // heavy corruption never passes the CRC
    int falseOk = 0;
    for (int t = 0; t < 500; t++) {
        std::vector<uint8_t> x(200);
        for (auto& b : x) b = (uint8_t)(rng() & 1);
        if (fichDecode(x.data(), g, fixes)) falseOk++;
    }
    CHECK(falseOk == 0, "random bits passed the FICH check %d times", falseOk);

    // DCH units of both sizes
    for (int nb : {20, 10}) {
        uint8_t d[20], o[20];
        for (int i = 0; i < nb; i++) d[i] = (uint8_t)rng();
        std::vector<uint8_t> e = dchEncode(d, nb);
        CHECK((int)e.size() == (nb == 20 ? 360 : 200), "DCH %d coded size %zu", nb, e.size());
        CHECK(dchDecode(e.data(), (int)e.size(), o, fixes) && std::equal(d, d + nb, o), "DCH %d clean", nb);
        std::vector<uint8_t> x = e;
        x[5] ^= 1; x[(size_t)e.size() / 2] ^= 1; x[e.size() - 3] ^= 1;
        CHECK(dchDecode(x.data(), (int)x.size(), o, fixes) && std::equal(d, d + nb, o), "DCH %d with 3 errors", nb);
        for (int k = 0; k < 40; k++) x[(size_t)(rng() % e.size())] ^= 1;
        CHECK(!dchDecode(x.data(), (int)x.size(), o, fixes), "DCH %d with 40 errors passed the CRC", nb);
    }

    // V/D mode 2 voice: 49 bits through 104 and back, and the majority vote against one flip per repeated bit
    uint8_t v49[49], r49[49];
    for (auto& b : v49) b = (uint8_t)(rng() & 1);
    std::vector<uint8_t> ve = vd2VoiceEncode(v49);
    CHECK(ve.size() == 104, "VD2 voice size %zu", ve.size());
    vd2VoiceDecode(ve.data(), r49);
    CHECK(std::equal(v49, v49 + 49, r49), "VD2 voice clean");
    // flip one coded bit: the interleaver spreads neighbours, a single flip is always outvoted
    int voted = 0;
    for (int i = 0; i < 104; i++) {
        std::vector<uint8_t> x = ve;
        x[(size_t)i] ^= 1;
        vd2VoiceDecode(x.data(), r49);
        int bad = 0; for (int k = 0; k < 27; k++) bad += r49[k] != v49[k];
        if (bad == 0) voted++;
    }
    CHECK(voted == 104, "single flip outvoted in the first 27 bits: %d of 104", voted);

    // whole frames of every data type from the generator: build, parse, compare
    for (int dt = 0; dt < 4; dt++) {
        GenParams p;
        p.dt = dt; p.commFrames = 9; p.dest = "CQCQCQ"; p.src = "LY1ABC"; p.down = "DB0XYZ"; p.up = "DB0XYZ";
        p.rem[0] = "12345"; p.rem[3] = "67890"; p.slow = "55.0N 25.3E test";
        std::vector<uint8_t> all;
        std::vector<FrameContent> sent;
        CHECK(genTransmissionBits(p, all, &sent), "generator dt %d", dt);
        CHECK(all.size() == sent.size() * 960, "bits %zu frames %zu", all.size(), sent.size());
        int okFrames = 0;
        for (size_t k = 0; k < sent.size(); k++) {
            // the sync word
            uint64_t sw = 0;
            for (int i = 0; i < 40; i++) sw = (sw << 1) | all[k * 960 + (size_t)i];
            CHECK(sw == kSyncWord, "frame %zu sync %llX", k, (unsigned long long)sw);
            FrameContent c;
            if (!parseFrameBits(&all[k * 960], c)) continue;
            bool same = c.fich.fi == sent[k].fich.fi && c.fich.dt == dt && c.fich.fn == sent[k].fich.fn && c.units == sent[k].units && c.voice.size() == sent[k].voice.size();
            for (size_t i = 0; same && i < c.voice.size(); i++) same = c.voice[i].bytes == sent[k].voice[i].bytes && c.voice[i].kind == sent[k].voice[i].kind;
            for (bool b : c.unitOk) same = same && b;
            if (same) okFrames++;
            else printf("  dt %d frame %zu differs\n", dt, k);
        }
        CHECK(okFrames == (int)sent.size(), "dt %d: %d of %zu frames round-tripped", dt, okFrames, sent.size());
    }

    // field helpers
    uint8_t fld[10];
    padField("LY1ABC", fld, 10);
    CHECK(trimField(fld, 10) == "LY1ABC" && fld[9] == ' ', "callsign field");
    return testSummary("test_fusion_proto");
}
