// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* Reference wrapper: an OnAir speech codec plug-in around YOUR OWN mbelib or mbelib-neo. This file contains no codec; it only calls the
 * library you build. OnAir does not ship mbelib, mbelib-neo or any binary built from this file (see docs/CODECS.md, "AMBE+2, AMBE and IMBE
 * plug-ins").
 *
 * Choose the codec at compile time:  -DCODEC_AMBE2 (DMR, NXDN, dPMR; plug-in file onair-codec-ambe2)
 *                                    -DCODEC_AMBE  (D-STAR;           onair-codec-ambe)
 *                                    -DCODEC_IMBE  (P25 phase 1;      onair-codec-imbe)
 *
 * mbelib (szechyjs / lwvmobile), header mbelib.h, library libmbe:
 *   cc -shared -fPIC -O2 -DCODEC_AMBE2 mbelib_wrapper.c -lmbe -o onair-codec-ambe2.dylib          (.so on Linux, .dll on Windows/MSYS2)
 * mbelib-neo (arancormonk), header mbelib-neo/mbelib.h, library libmbe-neo:
 *   cc -shared -fPIC -O2 -DCODEC_AMBE2 -DMBELIB_NEO mbelib_wrapper.c -lmbe-neo -o onair-codec-ambe2.dylib
 *   (add -I/usr/local/include or wherever mbelib-neo installed its include folder)
 * Copy the result into the OnAir codecs folder and press "Look again".
 *
 * Why the two builds: mbelib-neo kept the function names of mbelib 1.3 but changed the frame functions' arguments
 *   mbelib 1.3:  mbe_processAmbe3600x2450Frame(short* out, int* errs, int* errs2, char* err_str, char fr[4][24], char d[49], cur, prev, enh, int uvquality)
 *   mbelib-neo:  mbe_processAmbe3600x2450Frame(short* out, mbe_process_result* result, const char fr[4][24], char d[49], cur, prev, enh)
 * and its mbe_parms is about twice as large. A wrapper compiled with the old mbelib.h but run with libmbe-neo (or the other way round)
 * corrupts memory and crashes: this file refuses to start in that case (onair_speech_open returns NULL, OnAir says the decoder would
 * not start), and OnAir runs every plug-in call on its codec guard, so a crash only disables the plug-in. */
#if defined(MBELIB_NEO) || (defined(__has_include) && __has_include(<mbelib-neo/mbelib.h>) && !defined(MBELIB_CLASSIC))
#include <mbelib-neo/mbelib.h>
#define WRAP_NEO 1
#else
#include <mbelib.h>
#define WRAP_NEO 0
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define API __declspec(dllexport)
#else
#include <dlfcn.h>
#define API __attribute__((visibility("default")))
#endif

#if defined(CODEC_IMBE)
#define ROWS 8
#define COLS 23
#else
#define ROWS 4
#define COLS 24
#endif

typedef struct { mbe_parms cur, prev, prevEnh; } State;

/* the interface version of OnAir's plug-in ABI this file is written for (OnAir refuses another number) */
API int onair_speech_abi(void) { return 1; }

/* Is the library that is really loaded mbelib-neo? Only it exports mbe_versionString / mbe_initProcessResult. */
static int loadedIsNeo(void) {
#ifdef _WIN32
    HMODULE mods[] = {GetModuleHandleA("libmbe-neo.dll"), GetModuleHandleA("mbe-neo.dll"), GetModuleHandleA("libmbe.dll"), GetModuleHandleA("mbelib.dll")};
    for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++)
        if (mods[i] && (GetProcAddress(mods[i], "mbe_versionString") || GetProcAddress(mods[i], "mbe_initProcessResult"))) return 1;
    return 0;
#else
    return dlsym(RTLD_DEFAULT, "mbe_versionString") != NULL || dlsym(RTLD_DEFAULT, "mbe_initProcessResult") != NULL;
#endif
}

API void* onair_speech_open(void) {
    if (loadedIsNeo() != WRAP_NEO) return NULL;   /* built for the other library: calling it would corrupt memory */
    State* s = (State*)calloc(1, sizeof *s);
    if (s) mbe_initMbeParms(&s->cur, &s->prev, &s->prevEnh);
    return s;
}

API void onair_speech_close(void* state) { free(state); }

API int onair_speech_sample_rate(void) { return 8000; }

/* coded: the ROWS x COLS code-word bits, row by row, most significant bit first (12 bytes for AMBE / AMBE+2, 23 for IMBE) */
API int onair_speech_decode(void* state, const uint8_t* coded, size_t nbytes, int16_t* pcm, size_t max_samples) {
    State* s = (State*)state;
    char fr[ROWS][COLS];
    char data[88];
    short out[160];
    if (!s || !coded || !pcm || max_samples < 160 || nbytes < (size_t)((ROWS * COLS + 7) / 8)) return -1;
    for (int i = 0; i < ROWS * COLS; i++) fr[i / COLS][i % COLS] = (char)((coded[i / 8] >> (7 - (i % 8))) & 1);
    memset(data, 0, sizeof data);
#if WRAP_NEO
    mbe_process_result res;
    mbe_initProcessResult(&res);
#if defined(CODEC_AMBE2)
    mbe_processAmbe3600x2450Frame(out, &res, (const char(*)[24])fr, data, &s->cur, &s->prev, &s->prevEnh);
#elif defined(CODEC_AMBE)
    mbe_processAmbe3600x2400Frame(out, &res, (const char(*)[24])fr, data, &s->cur, &s->prev, &s->prevEnh);
#elif defined(CODEC_IMBE)
    mbe_processImbe7200x4400Frame(out, &res, (const char(*)[23])fr, data, &s->cur, &s->prev, &s->prevEnh);
#else
#error "define CODEC_AMBE2, CODEC_AMBE or CODEC_IMBE"
#endif
#else
    char err[64];
    int errs = 0, errs2 = 0;
    memset(err, 0, sizeof err);
#if defined(CODEC_AMBE2)
    mbe_processAmbe3600x2450Frame(out, &errs, &errs2, err, fr, data, &s->cur, &s->prev, &s->prevEnh, 3);
#elif defined(CODEC_AMBE)
    mbe_processAmbe3600x2400Frame(out, &errs, &errs2, err, fr, data, &s->cur, &s->prev, &s->prevEnh, 3);
#elif defined(CODEC_IMBE)
    mbe_processImbe7200x4400Frame(out, &errs, &errs2, err, fr, data, &s->cur, &s->prev, &s->prevEnh, 3);
#else
#error "define CODEC_AMBE2, CODEC_AMBE or CODEC_IMBE"
#endif
#endif
    for (int i = 0; i < 160; i++) pcm[i] = out[i];
    return 160;
}
