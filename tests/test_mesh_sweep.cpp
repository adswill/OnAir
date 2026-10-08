// Mesh (LoRa): frame success against SNR for SF 7..12 (125 kHz, 4/5, 20-byte frames) and the two mesh defaults, compared with the
// demodulation floors of the Semtech SX1276 datasheet (table 13: SF7 -7.5 dB ... SF12 -20 dB). Prints the table; fails when a setting
// needs more than 1.5 dB above the datasheet floor for 90 % of its frames.
#include "data/mesh/testutil.h"
using namespace dect2;
using namespace meshtest;

int main() {
    struct Row { int sf; double bw; int cr; double floorDb; const char* name; };
    const Row rows[] = {
        {7, 125e3, 5, -7.5, "SF7"}, {8, 125e3, 5, -10, "SF8"}, {9, 125e3, 5, -12.5, "SF9"}, {10, 125e3, 5, -15, "SF10"},
        {11, 125e3, 5, -17.5, "SF11"}, {12, 125e3, 5, -20, "SF12"},
        {11, 250e3, 5, -17.5, "Meshtastic LongFast SF11 250 kHz"}, {8, 62.5e3, 8, -10, "MeshCore EU SF8 62.5 kHz 4/8"},
    };
    printf("frames decoded (of N) against in-band SNR\n");
    for (const auto& r : rows) {
        lora::Params p; p.sf = r.sf; p.bwHz = r.bw; p.cr = r.cr; p.preamble = 16; p.ldro = lora::autoLdro(r.sf, r.bw);
        const int frames = 20;
        double ninety = 99;
        printf("%-34s", r.name);
        for (double d = 3; d >= -3; d -= 1) {
            Trial t;
            t.p = p; t.rate = 4 * r.bw; t.snrDb = r.floorDb + d; t.frames = frames; t.seed = (uint32_t)(r.sf * 100 + (int)(d + 10));
            const int ok = runTrial(t);
            printf(" %+5.1f:%2d", t.snrDb, ok);
            if (ok >= 0.9 * frames) ninety = t.snrDb;
        }
        printf("  | 90%% at %.1f dB (datasheet floor %.1f)\n", ninety, r.floorDb);
        CHECK(ninety <= r.floorDb + 1.5, "%s needs %.1f dB", r.name, ninety);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
