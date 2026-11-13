// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A stand-in for the user's DRA+ add-on (libonair_dra), for the tests of the runtime loader and the CDR audio path only: the five entry points
// OnAir looks up, no codec. Built three times from this file (FAKE_VARIANT, CMakeLists.txt):
//   0  a good library: a frame sent in (0x7FFF sync, header with the frame size in 32-bit words, exactly that many bytes) "decodes" to 1024
//      stereo frames at 48 kHz; L = the byte after the header * 100, R = the frame size in bytes. Anything else sent in is refused (-1).
//   1  not an add-on: DRADecRecvFrame is missing
//   2  DRADecCreate gives no decoder
#ifdef _WIN32
#define FAKE_API __declspec(dllexport)
#else
#define FAKE_API __attribute__((visibility("default")))
#endif
#include <stdlib.h>

#ifndef FAKE_VARIANT
#define FAKE_VARIANT 0
#endif

typedef struct { int nChannels; int nSampleRate; int nFrameSize; } Info;
typedef struct { int have; short pcm[2048]; } Dec;

#if FAKE_VARIANT == 2
FAKE_API void* DRADecCreate(void) { return 0; }
#else
FAKE_API void* DRADecCreate(void) { return calloc(1, sizeof(Dec)); }
#endif
FAKE_API void DRADecDestroy(void* h) { free(h); }
FAKE_API int DRADecGetFrameInfo(void* h, Info* i) { (void)h; i->nChannels = 2; i->nSampleRate = 48000; i->nFrameSize = 0; return 0; }
FAKE_API int DRADecSendData(void* h, unsigned char* p, int n) {
    Dec* d = (Dec*)h;
    if (n < 7 || p[0] != 0x7F || p[1] != 0xFF) return -1;
    unsigned v = (unsigned)p[2] << 24 | (unsigned)p[3] << 16 | (unsigned)p[4] << 8 | p[5];
    unsigned words = (v >> 31) ? (v >> 17) & 0x1FFF : (v >> 21) & 0x3FF;
    if (words < 2 || words * 4 != (unsigned)n) return -1;      // a unit with extension bytes behind the frame is not a frame
    for (int i = 0; i < 1024; i++) { d->pcm[2 * i] = (short)(p[6] * 100); d->pcm[2 * i + 1] = (short)n; }
    d->have = 1;
    return 0;
}
#if FAKE_VARIANT != 1
FAKE_API int DRADecRecvFrame(void* h, unsigned char** pcm, int* len) {
    Dec* d = (Dec*)h;
    if (!d->have) return -1;
    d->have = 0;
    *pcm = (unsigned char*)d->pcm; *len = 1024 * 2 * 2;
    return 0;
}
#endif
