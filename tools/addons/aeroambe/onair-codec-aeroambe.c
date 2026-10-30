/* Speech codec plug-in for OnAir: the AMBE 4800 bit/s vocoder of the Inmarsat Aero C channel (codec id "aeroambe",
 * core/include/dect2/codec/speech_codec.h). Built by the user, never distributed with OnAir: the vocoder is patented
 * (see the patent notice of mbelib) and OnAir ships no patented codec (docs/CODECS.md).
 *
 * It wraps the mbelib of libaeroambe (github.com/jontio/libaeroambe, mbelib-master: mbe_processAmbe4800x3600Frame), the same
 * decoder JAERO uses. One frame in = the 12 bytes OnAir hands over (the 96 coded bits as received, low bit of byte 0 first);
 * 160 samples out at 8 kHz. The bit order into mbelib (rW / rX) is libaeroambe's aeroambe.h.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mbelib.h"

#ifdef _WIN32
#define AEROAMBE_API __declspec(dllexport)
#else
#define AEROAMBE_API __attribute__((visibility("default")))
#endif

static const unsigned char rX[96] = {
    23, 11, 14, 2, 5, 8, 9, 11, 22, 10, 13, 1, 4, 7, 8, 10, 21, 9, 12, 0, 3, 6, 7, 9, 20, 8, 11, 14, 2, 5, 6,
    8, 19, 7, 10, 13, 1, 4, 5, 7, 18, 6, 9, 12, 0, 3, 4, 6, 17, 5, 8, 11, 14, 2, 3, 5, 16, 4, 7, 10, 13, 1, 2,
    4, 15, 3, 6, 9, 12, 0, 1, 3, 14, 2, 5, 8, 11, 12, 0, 2, 13, 1, 4, 7, 10, 11, 13, 1, 12, 0, 3, 6, 9, 10, 12, 0
};
static const unsigned char rW[96] = {
    0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2,
    2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5,
    0, 0, 1, 2, 3, 4, 4, 5, 0, 0, 1, 2, 3, 4, 5, 5, 0, 0, 1, 2, 3, 4, 5, 5
};

struct state {
    mbe_parms cur, prev, prevEnh;
    char ambe_fr[6][24];
    char ambe_d[72];
    char err[1024];
};

AEROAMBE_API void* onair_speech_open(void) {
    struct state* s = (struct state*)calloc(1, sizeof(struct state));
    if (s) mbe_initMbeParms(&s->cur, &s->prev, &s->prevEnh);
    return s;
}
AEROAMBE_API void onair_speech_close(void* s) { free(s); }
AEROAMBE_API int onair_speech_sample_rate(void) { return 8000; }
AEROAMBE_API int onair_speech_abi(void) { return 1; }

AEROAMBE_API int onair_speech_decode(void* st, const uint8_t* coded, size_t n, int16_t* pcm, size_t max) {
    struct state* s = (struct state*)st;
    int errs = 0, errs2 = 0;
    if (!s || !coded || n != 12 || max < 160) return -1;
    for (int i = 0; i < 96; i++) s->ambe_fr[rW[i]][rX[i]] = (char)((coded[i >> 3] >> (i & 7)) & 1);
    mbe_processAmbe4800x3600Frame((short*)pcm, &errs, &errs2, s->err, s->ambe_fr, s->ambe_d, &s->cur, &s->prev, &s->prevEnh, 1);
    return 160;
}
