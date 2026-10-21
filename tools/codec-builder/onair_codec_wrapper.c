/* OnAir speech codec plug-in wrapper for the build helper (tools/codec-builder). This file contains no codec: it only calls the
 * mbelib-neo library that the helper script builds on the user's own computer. OnAir ships neither mbelib-neo nor any binary built from this.
 * ABI: docs/CODECS.md "Speech codec plug-ins" and core/include/dect2/codec/speech_codec.h.
 *
 * Choose the codec at compile time: -DCODEC_AMBE2 (DMR, NXDN, dPMR)  -DCODEC_AMBE (D-STAR)  -DCODEC_IMBE (P25)
 *                                   -DCODEC_TETRA                    -DCODEC_YSF  (Yaesu Fusion)
 * Written against mbelib-neo 2.x (mbelib-neo/mbelib.h, result-returning API). */
#include <mbelib-neo/mbelib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

#if defined(CODEC_TETRA)

API void* onair_speech_open(void) { return mbe_tetraDecoderAlloc(); }
API void onair_speech_close(void* state) { mbe_tetraDecoderFree((mbe_tetra_decoder*)state); }
API int onair_speech_sample_rate(void) { return 8000; }

/* coded: 137 bytes, one bit per byte (0 or 1), B(1)..B(137). 240 samples out. */
API int onair_speech_decode(void* state, const uint8_t* coded, size_t nbytes, int16_t* pcm, size_t max_samples) {
    if (!state || !coded || nbytes < MBE_TETRA_FRAME_BITS || max_samples < MBE_TETRA_FRAME_SAMPLES) return -1;
    if (mbe_tetraDecodeFrame((mbe_tetra_decoder*)state, coded, 0, pcm) != 0) return -1;
    return MBE_TETRA_FRAME_SAMPLES;
}

#elif defined(CODEC_AMBE2) || defined(CODEC_AMBE) || defined(CODEC_IMBE) || defined(CODEC_YSF)

typedef struct { mbe_parms cur, prev, prevEnh; } State;

API void* onair_speech_open(void) {
    State* s = (State*)calloc(1, sizeof *s);
    if (s) mbe_initMbeParms(&s->cur, &s->prev, &s->prevEnh);
    return s;
}
API void onair_speech_close(void* state) { free(state); }
API int onair_speech_sample_rate(void) { return 8000; }

#if defined(CODEC_IMBE)
#define ROWS 8
#define COLS 23
#else
#define ROWS 4
#define COLS 24
#endif

API int onair_speech_decode(void* state, const uint8_t* coded, size_t nbytes, int16_t* pcm, size_t max_samples) {
    State* s = (State*)state;
    short out[160];
    mbe_process_result res;
    int rc;
    if (!s || !coded || max_samples < 160) return -1;
    memset(&res, 0, sizeof res);
#if defined(CODEC_YSF)
    /* Yaesu Fusion frames: 9 bytes (V/D mode 1 channel bits) and 18 bytes (voice FR) need Yaesu's undocumented bit order and
     * channel decoding, which this helper does not provide: they are refused. 7 bytes = the 49 voice bits of V/D mode 2
     * after error correction, MSB first: decoded as AMBE+2. */
    char d[49];
    if (nbytes != 7) return -1;
    for (int i = 0; i < 49; i++) d[i] = (char)((coded[i / 8] >> (7 - (i % 8))) & 1);
    rc = mbe_processAmbe2450Data(out, &res, d, &s->cur, &s->prev, &s->prevEnh);
#else
    char fr[ROWS][COLS];
    /* coded: the ROWS x COLS code-word bits, row by row, MSB first (12 bytes AMBE / AMBE+2, 23 bytes IMBE) */
    if (nbytes < (size_t)((ROWS * COLS + 7) / 8)) return -1;
    for (int i = 0; i < ROWS * COLS; i++) fr[i / COLS][i % COLS] = (char)((coded[i / 8] >> (7 - (i % 8))) & 1);
#if defined(CODEC_IMBE)
    char d[88];
    memset(d, 0, sizeof d);
    rc = mbe_processImbe7200x4400Frame(out, &res, fr, d, &s->cur, &s->prev, &s->prevEnh);
#else
    char d[49];
    memset(d, 0, sizeof d);
#if defined(CODEC_AMBE2)
    rc = mbe_processAmbe3600x2450Frame(out, &res, fr, d, &s->cur, &s->prev, &s->prevEnh);
#else
    rc = mbe_processAmbe3600x2400Frame(out, &res, fr, d, &s->cur, &s->prev, &s->prevEnh);
#endif
#endif
#endif
    if (rc < 0) return -1;
    for (int i = 0; i < 160; i++) pcm[i] = out[i];
    return 160;
}

#else
#error "define one of CODEC_AMBE2, CODEC_AMBE, CODEC_IMBE, CODEC_TETRA, CODEC_YSF"
#endif
