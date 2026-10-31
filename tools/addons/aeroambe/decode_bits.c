// decode_bits: an OnAir Inmarsat call recording (.bits file (12-byte Aero AMBE frames) -> 8 kHz WAV through libaeroambe's mbelib, with variants:
//   decode_bits in.bits out.wav [uvquality 1..64] [mute: frames whose corrected errors exceed N are replaced by silence, -1 = mbelib's own rule]
//               [post 0/1: the post-filter of aeroambe_post.h] [first frame] [frame count]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mbelib.h"
#include "aeroambe_post.h"
static const unsigned char rX[96] = {23,11,14,2,5,8,9,11,22,10,13,1,4,7,8,10,21,9,12,0,3,6,7,9,20,8,11,14,2,5,6,8,19,7,10,13,1,4,5,7,18,6,9,12,0,3,4,6,17,5,8,11,14,2,3,5,16,4,7,10,13,1,2,4,15,3,6,9,12,0,1,3,14,2,5,8,11,12,0,2,13,1,4,7,10,11,13,1,12,0,3,6,9,10,12,0};
static const unsigned char rW[96] = {0,0,1,1,2,3,4,5,0,0,1,1,2,3,4,5,0,0,1,1,2,3,4,5,0,0,1,2,2,3,4,5,0,0,1,2,2,3,4,5,0,0,1,2,2,3,4,5,0,0,1,2,3,3,4,5,0,0,1,2,3,3,4,5,0,0,1,2,3,3,4,5,0,0,1,2,3,4,4,5,0,0,1,2,3,4,5,5,0,0,1,2,3,4,5,5};
static void w32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); } static void w16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: decode_bits in.bits out.wav [uvquality] [muteErrs] [post] [firstFrame] [frameCount]\n"); return 1; }
    int uvq = argc > 3 ? atoi(argv[3]) : 1, mute = argc > 4 ? atoi(argv[4]) : -1, post = argc > 5 ? atoi(argv[5]) : 0;
    long first = argc > 6 ? atol(argv[6]) : 0, count = argc > 7 ? atol(argv[7]) : -1;
    aeropost pf; aeropost_init(&pf);
    FILE* in = fopen(argv[1], "rb"); if (!in) { perror("in"); return 1; }
    fseek(in, first * 12, SEEK_SET);
    FILE* out = fopen(argv[2], "wb"); if (!out) { perror("out"); fclose(in); return 1; }
    fwrite("RIFF", 1, 4, out); w32(out, 0); fwrite("WAVEfmt ", 1, 8, out); w32(out, 16); w16(out, 1); w16(out, 1); w32(out, 8000); w32(out, 16000); w16(out, 2); w16(out, 16); fwrite("data", 1, 4, out); w32(out, 0);
    mbe_parms cur, prev, prevEnh; mbe_initMbeParms(&cur, &prev, &prevEnh);
    char fr[6][24], d[72], err[1024]; unsigned char b[12]; short pcm[160];
    long n = 0, muted = 0, hist[8] = {0}; long bytes = 0;
    while ((count < 0 || n < count) && fread(b, 1, 12, in) == 12) {
        int errs = 0, errs2 = 0;
        for (int i = 0; i < 96; i++) fr[rW[i]][rX[i]] = (char)((b[i >> 3] >> (i & 7)) & 1);
        mbe_processAmbe4800x3600Frame(pcm, &errs, &errs2, err, fr, d, &cur, &prev, &prevEnh, uvq);
        hist[errs2 > 7 ? 7 : errs2]++;
        if (mute >= 0 && errs2 > mute) { memset(pcm, 0, sizeof pcm); muted++; }
        if (post) aeropost_run(&pf, pcm, 160);
        fwrite(pcm, 2, 160, out); bytes += 320; n++;
    }
    fseek(out, 4, SEEK_SET); w32(out, 36 + (uint32_t)bytes); fseek(out, 40, SEEK_SET); w32(out, (uint32_t)bytes); fclose(out); fclose(in);
    printf("%s: %ld frames (%.1f s), uvquality %d, mute>%d: %ld muted, post-filter %s; corrected-error histogram 0..7+:", argv[2], n, n * 0.02, uvq, mute, muted, post ? "on" : "off");
    for (int i = 0; i < 8; i++) printf(" %ld", hist[i]); printf("\n");
    return 0;
}
