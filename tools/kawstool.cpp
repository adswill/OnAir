// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS generator / receiver: kawstool tx <profile 0-5> <CALL> <text> out.wav   write a 12 kHz WAV burst (1.5 s silence, then the burst) carrying a chat message
//                           kawstool rx <profile 0-5> in.wav                    search the stream for a burst and print the messages decoded
#include "dect2/kaws/kaws_arq.h"
#include "dect2/kaws/kaws_phy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace dect2::kaws;

int main(int argc, char** argv) {
    if (argc >= 6 && !std::strcmp(argv[1], "tx")) {
        int p = std::atoi(argv[2]);
        if (p < 0 || p > 5) { fprintf(stderr, "profile 0..5 (data profiles)\n"); return 2; }
        Chat c;
        c.from = argv[3]; c.to = "CQ"; c.firstMessage = true; c.text = argv[4];
        std::vector<Bits> blocks = splitMessage(kProfiles[p].K, 1, encodeChat(c));
        if (blocks.empty() || (int)blocks.size() > std::max(1, blocksPerSlot(p) ? blocksPerSlot(p) : 1)) { fprintf(stderr, "message needs %zu blocks; more than one slot of profile %d: shorten it\n", blocks.size(), p); return 2; }
        std::vector<cf> burst = buildBurst(p, blocks);
        std::vector<cf> stream(3600, cf(0, 0));
        stream.insert(stream.end(), burst.begin(), burst.end());
        stream.resize(stream.size() + 2400);
        if (!writeWav(argv[5], toAudio12k(stream))) { fprintf(stderr, "cannot write %s\n", argv[5]); return 1; }
        printf("%zu block(s), %.2f s on air\n", blocks.size(), onAirSeconds(p, (int)blocks.size()));
        return 0;
    }
    if (argc >= 4 && !std::strcmp(argv[1], "rx")) {
        int p = std::atoi(argv[2]);
        if (p < 0 || p > 5) return 2;
        std::vector<float> a;
        int rate = 0;
        if (!readWav(argv[3], a, rate) || rate != kAudioRate) { fprintf(stderr, "need a 12 kHz mono 16-bit WAV\n"); return 1; }
        std::vector<cf> iq = fromAudio12k(a);
        for (int nb = std::max(1, blocksPerSlot(p)); nb >= 1; nb--) {
            RxBurst r = receiveBurst(p, nb, iq, 0, std::min<int>(kSearchHi, (int)iq.size() - 9000));
            if (!r.sync) { printf("no burst found (statistic %.1f)\n", r.stat); return 1; }
            ArqReceiver rx("RX");
            int good = 0;
            for (size_t b = 0; b < r.blocks.size(); b++) if (r.iters[b] >= 0 && rx.onBlock(kProfiles[p].K, r.blocks[b])) good++;
            printf("burst at %.3f s, offset %.2f Hz, SNR %.1f dB, %d of %d blocks good\n", r.tau / kBaseRate, r.deltaHz, r.snrDb, good, nb);
            if (rx.complete(1)) {
                Chat c;
                if (decodeChat(rx.message(1), c)) { printf("%s: %s\n", c.from.c_str(), c.text.c_str()); return 0; }
            }
        }
        return 1;
    }
    fprintf(stderr, "usage: kawstool tx <profile> <CALL> <text> out.wav | kawstool rx <profile> in.wav\n");
    return 2;
}
