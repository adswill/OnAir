/* OnAir codec plug-in "aeroambe": Inmarsat Aero C channel voice (AMBE 4800x3600), built by the USER around mbelib.
 *
 * This file contains no vocoder. The decoder is mbe_processAmbe4800x3600Frame of the mbelib copy that ships inside
 * https://github.com/jontio/libaeroambe (folder mbelib-master). That code may be covered by patents: check the rules where you live.
 * OnAir never includes, downloads or links it (docs/CODECS.md).
 *
 * The bit handling below is libaeroambe's AeroAMBE::to_decode_slot (MIT licence, Copyright (c) 2018 libaeroambe team): the 12 bytes of a
 * frame are unpacked least significant bit first, bit i goes to ambe_fr[rW[i]][rX[i]], then mbelib decodes. The tables rW / rX are copied
 * from libaeroambe/aeroambe.h. OnAir hands over exactly the 12 bytes JAERO hands to libaeroambe.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mbelib.h"

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static const unsigned char rX[96] = {
    23, 11, 14, 2, 5, 8, 9, 11, 22, 10, 13, 1, 4, 7, 8, 10, 21, 9, 12, 0, 3, 6, 7, 9, 20, 8, 11, 14, 2, 5, 6,
    8, 19, 7, 10, 13, 1, 4, 5, 7, 18, 6, 9, 12, 0, 3, 4, 6, 17, 5, 8, 11, 14, 2, 3, 5, 16, 4, 7, 10, 13, 1, 2,
    4, 15, 3, 6, 9, 12, 0, 1, 3, 14, 2, 5, 8, 11, 12, 0, 2, 13, 1, 4, 7, 10, 11, 13, 1, 12, 0, 3, 6, 9, 10, 12, 0};
static const unsigned char rW[96] = {
    0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2,
    2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5,
    0, 0, 1, 2, 3, 4, 4, 5, 0, 0, 1, 2, 3, 4, 5, 5, 0, 0, 1, 2, 3, 4, 5, 5};

typedef struct {
    mbe_parms cur, prev, prevEnh;
} State;

EXPORT int onair_speech_abi(void) { return 1; }
EXPORT int onair_speech_sample_rate(void) { return 8000; }

EXPORT void* onair_speech_open(void) {
    State* s = (State*)calloc(1, sizeof(State));
    if (!s) return NULL;
    mbe_initMbeParms(&s->cur, &s->prev, &s->prevEnh);
    return s;
}

EXPORT void onair_speech_close(void* st) { free(st); }

EXPORT int onair_speech_decode(void* st, const uint8_t* coded, size_t nbytes, int16_t* pcm, size_t max_samples) {
    State* s = (State*)st;
    char fr[6][24];
    char d[96];
    char err[1024];
    int errs = 0, errs2 = 0, i;
    unsigned char bits[96];
    if (!s || !coded || nbytes != 12 || max_samples < 160) return -1;
    for (i = 0; i < 96; i++) bits[i] = (unsigned char)((coded[i / 8] >> (i % 8)) & 1);
    memset(fr, 0, sizeof fr);
    for (i = 0; i < 96; i++) fr[rW[i]][rX[i]] = (char)bits[i];
    memset(d, 0, sizeof d);
    mbe_processAmbe4800x3600Frame((short*)pcm, &errs, &errs2, err, fr, d, &s->cur, &s->prev, &s->prevEnh, 1);
    return 160;
}
