// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR voice: the three AMBE+2 frames of a voice burst reach the vocoder (a fake mbelib stand-in, tests/p25/fake_mbelib.c, loaded at run time) and come out as sound.
//  1. the 216 voice bits of a burst (burstVoicePayload: the two halves either side of the sync / EMB) are split into three consecutive 72 bit frames;
//  2. a valid AMBE+2 frame (Golay protected C0 and C1, C1 scrambled by the PN sequence of TS 102 361-1 annex B, built here from the code words) is
//     put into a burst at the place of every one of the three frames, through the burst layout, and decodes to 160 samples that are not silence;
//  3. the receiver: generator -> DmrReceiver; every voice burst of the followed slot is decoded, and 480 samples of 8 kHz speech reach the audio tap
//     as 2880 samples of 48 kHz. (The generator's voice bits are random: this proves the path, not the quality of the speech.)
// NOT covered: a recording of real DMR (none is available here), so the bit order inside the 72 bit frame is that of the NXDN / DSD tables.
#include "dect2/dmr/dmr_ambe.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/nxdn/nxdn_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::dmr;
#include "../common/test_util.h"

// where the interleave puts bit i of a 72 bit frame: cell [row][col] of the code words
static void cellOf(int i, int& row, int& col) {
    uint8_t bits[72] = {};
    bits[i] = 1;
    char fr[4][24];
    nxdn::ambeFrame(bits, fr);
    row = col = -1;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 24; c++) if (fr[r][c]) { row = r; col = c; }
}

// A frame whose C0 data is zero (a valid Golay word), C1 = PN scramble of that (so that it demodulates to the zero word), C2 and C3 random
static void makeFrame(uint8_t bits[72], unsigned seed) {
    char fr[4][24] = {};
    unsigned short pr[24];
    pr[0] = 0;                                             // 16 * the 12 C0 data bits
    for (int i = 1; i < 24; i++) pr[i] = (unsigned short)(173 * pr[i - 1] + 13849);
    for (int i = 1, j = 22; j >= 0; j--, i++) fr[1][j] = (char)(pr[i] >> 15);
    srand(seed);
    for (int j = 0; j < 11; j++) fr[2][j] = (char)(rand() & 1);
    for (int j = 0; j < 14; j++) fr[3][j] = (char)(rand() & 1);
    for (int i = 0; i < 72; i++) { int r, c; cellOf(i, r, c); bits[i] = (uint8_t)fr[r][c]; }
}

static void testSplit() {
    // the interleave is a permutation of the 4 x 24 cells that carries 72 bits
    bool seen[4][24] = {};
    for (int i = 0; i < 72; i++) {
        int r, c; cellOf(i, r, c);
        CHECK(r >= 0 && !seen[r][c], "bit %d lands on a used or no cell", i);
        if (r >= 0) seen[r][c] = true;
    }
    // C0 = 24 bits, C1 = 23, C2 = 11, C3 = 14
    int cnt[4] = {};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 24; c++) cnt[r] += seen[r][c];
    CHECK(cnt[0] == 24 && cnt[1] == 23 && cnt[2] == 11 && cnt[3] == 14, "cells per code word %d %d %d %d", cnt[0], cnt[1], cnt[2], cnt[3]);

    // a burst with bits 0..215 numbered by position: the frames are consecutive, across the centre field
    Bits vs(216);
    for (int i = 0; i < 216; i++) vs[i] = (uint8_t)((i * 7 + i / 5) & 1);
    const Bits burst = makeVoiceBurst(vs, kSyncBsVoice, 1, 0, 0, Bits());
    CHECK(burst.size() == 264, "burst size %zu", burst.size());
    Bits back;
    burstVoicePayload(burst, back);
    CHECK(back == vs, "burst payload round trip");
    uint8_t f[3][72];
    dmrVoiceFrames(back, f);
    int bad = 0;
    for (int i = 0; i < 216; i++) bad += f[i / 72][i % 72] != vs[i];
    CHECK(bad == 0, "frame split: %d bits wrong", bad);
    // frame 2 straddles the centre: its first 36 bits are burst bits 72..107, the rest 156..191
    for (int i = 0; i < 36; i++) CHECK(f[1][i] == burst[72 + i] && f[1][36 + i] == burst[156 + i], "frame 2 bit %d", i);
    for (int i = 0; i < 72; i++) CHECK(f[2][i] == burst[192 + i], "frame 3 bit %d", i);
}

static int nonzero(const int16_t* p, int n) { int k = 0; for (int i = 0; i < n; i++) k += p[i] != 0; return k; }

static void testDecode() {
    std::string path;
    if (!dmrAmbeAvailable(&path)) { printf("no vocoder in this build: decode tests skipped\n"); return; }
    DmrAmbe a;
    CHECK(a.ok(), "decoder");
    int totalNz = 0;
    for (int n = 0; n < 8; n++) {
        Bits vs(216);
        for (int k = 0; k < 3; k++) {
            uint8_t f[72];
            makeFrame(f, 100u + (unsigned)(n * 3 + k));
            for (int i = 0; i < 72; i++) vs[(size_t)(k * 72 + i)] = f[i];
        }
        const Bits burst = makeVoiceBurst(vs, n & 1 ? -1 : kSyncMsVoice, 3, 0, 0, Bits(32, 0));
        Bits payload;
        burstVoicePayload(burst, payload);
        int16_t pcm[480];
        memset(pcm, 0x55, sizeof pcm);
        a.decodeBurst(payload, pcm);
        int filled = 0;
        for (int16_t s : pcm) filled += s != 0x5555;
        CHECK(filled == 480, "burst %d: %d of 480 samples written", n, filled);
        totalNz += nonzero(pcm, 480);
    }
    printf("direct decode: %d of %d samples non-zero\n", totalNz, 8 * 480);
    CHECK(totalNz > 8 * 480 / 2, "valid AMBE+2 frames decode to silence (%d non-zero)", totalNz);
}

static void testReceiver() {
    if (!dmrAmbeAvailable()) return;
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(2.4e6);
    size_t tapped = 0, nz = 0;
    uint64_t bursts = 0;
    rx.setAudioTap([&](const float* l, const float* r, size_t n) { tapped += n; for (size_t i = 0; i < n; i++) { nz += l[i] != 0.f; if (l[i] != r[i]) nz += 1000000; } });
    rx.setVoiceCallback([&](int, int, const uint8_t*) { bursts++; });
    DmrGenConfig c;
    c.snrDb = 30; c.traffic = 2;
    DmrSignal sig(c);
    std::vector<cf32> g(65536);
    for (int blk = 0; blk < 110; blk++) { sig.generate(g.data(), g.size()); rx.feed(g.data(), g.size()); }
    DmrTelemetry t;
    rx.telemetry(t, 0);
    printf("receiver: %llu voice bursts, %llu decoded, slot %d, %zu samples at 48 kHz, %zu non-zero, vocoder %s\n", (unsigned long long)bursts, (unsigned long long)t.voiceDecoded, t.audioSlot, tapped, nz, t.ambePath.c_str());
    CHECK(t.ambe, "telemetry says no vocoder");
    CHECK(bursts > 10, "only %llu voice bursts", (unsigned long long)bursts);
    CHECK(t.voiceDecoded > 5 && t.voiceDecoded <= bursts, "%llu decoded of %llu", (unsigned long long)t.voiceDecoded, (unsigned long long)bursts);
    // the telemetry is up to 0.25 s old; the tap has everything decoded by the end
    CHECK(tapped >= (size_t)t.voiceDecoded * 2880 && tapped % 2880 == 0, "tap got %zu samples for %llu bursts", tapped, (unsigned long long)t.voiceDecoded);
    CHECK(nz < 1000000, "left and right differ");
}

int main() {
    {   // test hook of the loader: the library is taken from DECT2_NATIVE_LIBDIR, by the file name in DECT2_MBELIB_PATH
        const std::string full = FAKE_MBELIB_PATH, dir = full.substr(0, full.find_last_of("/\\"));
#ifdef _WIN32
        _putenv_s("DECT2_NATIVE_LIBDIR", dir.c_str());
        _putenv_s("DECT2_MBELIB_PATH", full.c_str());
#else
        setenv("DECT2_NATIVE_LIBDIR", dir.c_str(), 1);
        setenv("DECT2_MBELIB_PATH", full.c_str(), 1);
#endif
    }
    testSplit();
    testDecode();
    testReceiver();
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
